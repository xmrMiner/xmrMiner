#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include "cuda.h"
#include "cuda_runtime.h"

extern "C"
{
#include "xmrMiner-config.h"
#include "miner.h"
}
#include "cryptonight.h"
#include "cuda_device.hpp"

extern "C"
{
    extern char *device_name[8];
    extern int device_arch[8][2];
    extern int device_mpcount[8];
    extern int device_map[8];
    extern int device_config[8][2];
}

// Zahl der CUDA Devices im System bestimmen
extern "C" int cuda_num_devices()
{
	int version;
	cudaError_t err = cudaDriverGetVersion(&version);
	if(err != cudaSuccess)
	{
		applog(LOG_ERR, "Unable to query CUDA driver version! Is an nVidia driver installed?");
		exit(1);
	}

	if(version < CUDART_VERSION)
	{
		applog(LOG_ERR, "Driver does not support CUDA %d.%d API! Update your nVidia driver!", CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
		exit(1);
	}

	int GPU_N;
	err = cudaGetDeviceCount(&GPU_N);
	if(err != cudaSuccess)
	{
		if(err != cudaErrorNoDevice)
			applog(LOG_ERR, "No CUDA device found!");
		else
			applog(LOG_ERR, "Unable to query number of CUDA devices!");
		exit(1);
	}
	return GPU_N;
}

extern "C" void cuda_deviceinfo()
{
	cudaError_t err;
	int GPU_N;
	err = cudaGetDeviceCount(&GPU_N);
	if(err != cudaSuccess)
	{
		if(err != cudaErrorNoDevice)
			applog(LOG_ERR, "No CUDA device found!");
		else
			applog(LOG_ERR, "Unable to query number of CUDA devices!");
		exit(1);
	}

	for(int i = 0; i < GPU_N; i++)
	{
		cudaDeviceProp props;
		cudaError_t err = cudaGetDeviceProperties(&props, device_map[i]);
		if(err != cudaSuccess)
		{
			printf("\nGPU %d: %s\n%s line %d\n", device_map[i], cudaGetErrorString(err), __FILE__, __LINE__);
			exit(1);
		}

		device_name[i] = strdup(props.name);
		device_mpcount[i] = props.multiProcessorCount;
		device_arch[i][0] = props.major;
		device_arch[i][1] = props.minor;
	}
}

static bool substringsearch(const char *haystack, const char *needle, int &match)
{
	int hlen = (int)strlen(haystack);
	int nlen = (int)strlen(needle);
	for(int i = 0; i < hlen; ++i)
	{
		if(haystack[i] == ' ') continue;
		int j = 0, x = 0;
		while(j < nlen)
		{
			if(haystack[i + x] == ' ')
			{
				++x; continue;
			}
			if(needle[j] == ' ')
			{
				++j; continue;
			}
			if(needle[j] == '#') return ++match == needle[j + 1] - '0';
			if(tolower(haystack[i + x]) != tolower(needle[j])) break;
			++j; ++x;
		}
		if(j == nlen) return true;
	}
	return false;
}

extern "C" int cuda_finddevice(char *name)
{
	int num = cuda_num_devices();
	int match = 0;
	for(int i = 0; i < num; ++i)
	{
		cudaDeviceProp props;
		if(cudaGetDeviceProperties(&props, i) == cudaSuccess)
			if(substringsearch(props.name, name, match)) return i;
	}
	return -1;
}

static uint32_t *d_long_state[8];
static uint32_t *d_ctx_state[8];
static uint32_t *d_ctx_a[8];
static uint32_t *d_ctx_b[8];
static uint32_t *d_ctx_key1[8];
static uint32_t *d_ctx_key2[8];
static uint32_t *d_ctx_text[8];

extern "C"
{
extern bool opt_benchmark;
}

extern "C" void cryptonight_hash(void* output, const void* input, size_t len);

extern "C" int scanhash_cryptonight(int thr_id, uint32_t *pdata, int dlen, const uint32_t *ptarget, uint32_t max_nonce, unsigned long *hashes_done, uint32_t *results)
{
	cudaError_t err;
	int res;
	uint32_t *nonceptr = (uint32_t*)(((char*)pdata) + 39);
	const uint32_t first_nonce = *nonceptr;
	uint32_t nonce = *nonceptr;
	int cn_blocks = device_config[thr_id][0];
	int cn_threads = device_config[thr_id][1];
	if(opt_benchmark)
	{
		((uint32_t*)ptarget)[7] = 0x0000ff;
		pdata[17] = 0;
	}
	const uint32_t Htarg = ptarget[7];
	const uint32_t throughput = cn_threads * cn_blocks;
	if(sizeof(size_t) == 4 && throughput > 0xffffffff / MEMORY)
	{
		applog(LOG_ERR, "GPU %d: THE 32bit VERSION CAN'T ALLOCATE MORE THAN 4GB OF MEMORY!", device_map[thr_id]);
		applog(LOG_ERR, "GPU %d: PLEASE REDUCE THE NUMBER OF THREADS OR BLOCKS", device_map[thr_id]);
		exit(1);
	}
	const size_t alloc = (size_t)MEMORY * throughput;

	static bool init[8] = {false, false, false, false, false, false, false, false};
	if(!init[thr_id])
	{
		err = cudaSetDevice(device_map[thr_id]);
		if(err != cudaSuccess)
		{
			applog(LOG_ERR, "GPU %d: %s", device_map[thr_id], cudaGetErrorString(err));
		}
		cudaDeviceReset();
		cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
		cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
		cudaMalloc(&d_long_state[thr_id], alloc);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_state[thr_id], 50 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_key1[thr_id], 40 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_key2[thr_id], 40 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_text[thr_id], 32 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_a[thr_id], 4 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);
		cudaMalloc(&d_ctx_b[thr_id], 4 * sizeof(uint32_t) * throughput);
		exit_if_cudaerror(thr_id, __FILE__, __LINE__);

		cryptonight_extra_cpu_init(thr_id);

		init[thr_id] = true;
	}

	cryptonight_extra_cpu_setData(thr_id, (const void *)pdata, dlen, (const void *)ptarget);

	do
	{
		uint32_t foundNonce[2];

		cryptonight_extra_cpu_prepare(thr_id, throughput, dlen, nonce, d_ctx_state[thr_id], d_ctx_a[thr_id], d_ctx_b[thr_id], d_ctx_key1[thr_id], d_ctx_key2[thr_id]);
		cryptonight_core_cpu_hash(thr_id, cn_blocks, cn_threads, d_long_state[thr_id], d_ctx_state[thr_id], d_ctx_a[thr_id], d_ctx_b[thr_id], d_ctx_key1[thr_id], d_ctx_key2[thr_id]);
		cryptonight_extra_cpu_final(thr_id, throughput, nonce, foundNonce, d_ctx_state[thr_id]);

		if(foundNonce[0] < 0xffffffff)
		{
			uint32_t vhash64[8] = {0, 0, 0, 0, 0, 0, 0, 0};
			uint32_t tempdata[32];
			uint32_t *tempnonceptr = (uint32_t*)(((char*)tempdata) + 39);
			*tempnonceptr = foundNonce[0];
			memcpy(tempdata, pdata, dlen);
			cryptonight_hash(vhash64, tempdata, dlen);
			if((vhash64[7] <= Htarg) && fulltest(vhash64, ptarget))
			{
				res = 1;
				results[0] = foundNonce[0];
				*hashes_done = nonce - first_nonce + throughput;
				if(foundNonce[1] < 0xffffffff)
				{
					*tempnonceptr = foundNonce[1];
					cryptonight_hash(vhash64, tempdata, dlen);
					if((vhash64[7] <= Htarg) && fulltest(vhash64, ptarget))
					{
						res++;
						results[1] = foundNonce[1];
					}
					else
					{
						applog(LOG_INFO, "GPU #%d: result for nonce $%08X does not validate on CPU!", device_map[thr_id], foundNonce[1]);
					}
				}
				return res;
			}
			else
			{
				applog(LOG_INFO, "GPU #%d: result for nonce $%08X does not validate on CPU!", device_map[thr_id], foundNonce[0]);
			}
		}
		if((nonce & 0x00ffffff) > (0x00ffffff - throughput))
			nonce = max_nonce;
		else
			nonce += throughput;
	} while(nonce < max_nonce && !work_restart[thr_id].restart);

	*hashes_done = nonce - first_nonce;
	return 0;
}
