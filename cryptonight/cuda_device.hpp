
#pragma once

#include <cuda_runtime.h>

extern "C"
{
    extern int device_map[8];
}

static inline void exit_if_cudaerror(int thr_id, const char *file, int line)
{
	cudaError_t err = cudaGetLastError();
	if(err != cudaSuccess)
	{
		printf("\nGPU %d: %s\n%s line %d\n", device_map[thr_id], cudaGetErrorString(err), file, line);
		exit(1);
	}
}
