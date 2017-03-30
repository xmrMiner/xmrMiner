/* Copyright 2010 Jeff Garzik
 * Copyright 2012-2014 pooler
 * Copyright 2017 psychocrypt
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "xmrMiner-config.h"
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#ifndef _WIN64
#include <sys/resource.h>
#endif
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <jansson.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <cuda_runtime.h>
#include "compat.h"
#include "miner.h"

#ifdef WIN32
#include <Mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#define LP_SCANTIME  60
#define JSON_BUF_LEN 345

// from heavy.cu
#ifdef __cplusplus
extern "C" {
#endif
    int cuda_num_devices();
    void cuda_deviceinfo();
    int cuda_finddevice(char *name);
#ifdef __cplusplus
}
#endif

extern void cryptonight_hash(void* output, const void* input, size_t len);
void parse_device_config(int device, char *config, int *blocks, int *threads);

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>

static inline void drop_policy(void) {
    struct sched_param param;
    param.sched_priority = 0;

#ifdef SCHED_IDLE
    if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
        sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu) {
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof (&set), &set);
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>

static inline void drop_policy(void) {
}

static inline void affine_to_cpu(int id, int cpu) {
    cpuset_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof (cpuset_t), &set);
}
#else

static inline void drop_policy(void) {
}

static inline void affine_to_cpu(int id, int cpu) {
}
#endif

enum workio_commands {
    WC_GET_WORK,
    WC_SUBMIT_WORK,
};

struct workio_cmd {
    enum workio_commands cmd;
    struct thr_info *thr;

    union {
        struct work *work;
    } u;
};

typedef enum {
    ALGO_CRYPTONIGHT
} sha256_algos;

static const char *algo_names[] = {
    "cryptonight"
};

bool opt_debug = false;
bool opt_debugDev = false;
bool opt_protocol = false;
bool opt_keepalive2[2] = {false,true};
bool opt_benchmark = false;
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum2[2] = {true,true};
bool have_stratum2[2] = {false,true};
static bool submit_old = false;
bool use_syslog = false;
static bool opt_background = false;
static bool opt_quiet = false;
static int opt_retries = -1;
static int opt_fail_pause = 30;
int opt_timeout = 270;
static int opt_scantime = 5;
static json_t *opt_config;
static const bool opt_time = true;
static sha256_algos opt_algo = ALGO_CRYPTONIGHT;
static int opt_n_threads = 0;
static double opt_difficulty = 1; // CH
bool opt_trust_pool = false;
uint16_t opt_vote = 9999;
static int num_processors;
int device_map[8] = {0, 1, 2, 3, 4, 5, 6, 7}; // CB
char *device_name[8]; // CB
int device_arch[8][2];
int device_mpcount[8];
int device_bfactor[8];
int device_bsleep[8];
int device_config[8][2];
#ifdef WIN32
static int default_bfactor = 6;
static int default_bsleep = 25;
#else
static int default_bfactor = 0;
static int default_bsleep = 0;
#endif
static char *rpc_url2[2];
static char *rpc_userpass2[2];
static char *rpc_user2[2], *rpc_pass2[2];
static double donate = 0.02;
static double weightedDonate = 0.02;
static double pool_difficulty[2] = {-1.0,-1.0};
char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id2[2] = {-1,-1};
struct work_restart *work_restart = NULL;
//static struct stratum_ctx stratum;
static struct stratum_ctx stratumStorage[2];
int opt_cn_threads = 8;
int opt_cn_blocks = 0;

bool jsonrpc_2 = false;
static char rpc2_id2[2][64] = {"",""};
static char *rpc2_blob2[2] = {NULL,NULL};
static int rpc2_bloblen2[2] = {0,0};
static uint32_t rpc2_target2[2] = {0,0};
static char *rpc2_job_id2[2] = {NULL,NULL};
static pthread_mutex_t rpc2_job_lock2[2];
static pthread_mutex_t rpc2_login_lock2[2];

pthread_mutex_t applog_lock;
static pthread_mutex_t stats_lock2[2];
static pthread_mutex_t dev_lock;

static unsigned long accepted_count = 0L;
static unsigned long rejected_count = 0L;
static double *thr_hashrates;
static unsigned long count_dev = 0L;
static unsigned long share_count = 0L;
static const char* devPoolAddress = "43NoJVEXo21hGZ6tDG6Z3g4qimiGdJPE6GRxAmiWwm26gwr62Lqo7zRiCJFSBmbkwTGNuuES9ES5TgaVHceuYc4Y75txCTU";
static int mine_for_dev = 0;

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};
#endif

static char const usage[] = "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
	Options:\n\
	  -d, --devices         takes a comma separated list of CUDA devices to use.\n\
                          Device IDs start counting from 0! Alternatively takes\n\
	                        string names of your cards like gtx780ti or gt640#2\n\
	                        (matching 2nd gt640 in the PC)\n\
    -f, --diff            Divide difficulty by this factor (std is 1) \n\
	  -l, --launch=CONFIG   launch config for the Cryptonight kernel.\n\
                          a comma separated list of values in form of\n\
	                        AxB where A is the number of threads to run in\n\
	                        each thread block and B is the number of thread\n\
	                        blocks to launch. If less values than devices in use\n\
                          are provided, the last value will be used for\n\
                          the remaining devices. If you don't need to vary the\n\
	                        value between devices, you can just enter a single value\n\
	                        and it will be used for all devices. (default: 8x40)\n\
		    --bfactor=X       Enables running the Cryptonight kernel in smaller pieces.\n\
	                        The kernel will be run in 2^X parts according to bfactor,\n\
                          with a small pause between parts, specified by --bsleep.\n\
                          This is a per-device setting like the launch config.\n\
	                        (default: 0 (no splitting) on Linux, 6 (64 parts) on Windows)\n\
	      --bsleep=X        Insert a delay of X microseconds between kernel launches.\n\
                          Use in combination with --bfactor to mitigate the lag\n\
	                        when running on your primary GPU.\n\
                          This is a per-device setting like the launch config.\n\
	  -m, --trust-pool      trust the max block reward vote (maxvote) sent by the pool\n\
	  -o, --url=URL         URL of mining server\n\
    -O, --userpass=U:P    username:password pair for mining server\n\
	  -u, --user=USERNAME   username for mining server\n\
	  -p, --pass=PASSWORD   password for mining server\n\
        --cert=FILE       certificate for mining server using SSL\n\
	  -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
    -k, --keepalive       send keepalive requests to avoid a stratum timeout\n\
    -t, --threads=N       number of miner threads (default: number of nVidia GPUs)\n\
	  -r, --retries=N       number of times to retry if a network call fails\n\
                          (default: retry indefinitely)\n\
	  -R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
        --timeout=N       network timeout, in seconds (default: 270)\n\
	  -s, --scantime=N      upper bound on time spent scanning current work when\n\
                          long polling is unavailable, in seconds (default: 5)\n\
	  -q, --quiet           disable per-thread hashmeter output\n\
	  -D, --debug           enable debug output\n\
	  -P, --protocol-dump   verbose dump of protocol-level activities\n"
#ifdef HAVE_SYSLOG_H
        "\
	  -S, --syslog          use system log for output messages\n"
#endif
#ifndef WIN32
        "\
	  -B, --background      run the miner in the background\n"
#endif
        "\
	      --benchmark       run in offline benchmark mode\n\
	  -c, --config=FILE     load a JSON-format configuration file\n\
      -z, --donate=N     donate N percent of the shares to the developer (default: 2.0)\n\
      -Z, --debugDev     show developer pool actions (debug))\n\
	  -V, --version         display version information and exit\n\
	  -h, --help            display this help text and exit\n\
";

static char const short_options[] =
#ifndef WIN32
        "B"
#endif
#ifdef HAVE_SYSLOG_H
        "S"
#endif
        "a:c:Dhp:Px:kqr:R:s:t:T:o:u:O:Vd:f:ml:z:Z";

static struct option const options[] = {
    {"algo", 1, NULL, 'a'},
#ifndef WIN32
    {"background", 0, NULL, 'B'},
#endif
    {"benchmark", 0, NULL, 1005},
    {"cert", 1, NULL, 1001},
    {"config", 1, NULL, 'c'},
    {"donate", 1, NULL, 'z'},
    {"debugDev", 0, NULL, 'Z'},
    {"debug", 0, NULL, 'D'},
    {"help", 0, NULL, 'h'},
    {"keepalive", 0, NULL, 'k'},
    {"pass", 1, NULL, 'p'},
    {"protocol-dump", 0, NULL, 'P'},
    {"proxy", 1, NULL, 'x'},
    {"quiet", 0, NULL, 'q'},
    {"retries", 1, NULL, 'r'},
    {"retry-pause", 1, NULL, 'R'},
    {"scantime", 1, NULL, 's'},
#ifdef HAVE_SYSLOG_H
    {"syslog", 0, NULL, 'S'},
#endif
    {"threads", 1, NULL, 't'},
    {"trust-pool", 0, NULL, 'm'},
    {"timeout", 1, NULL, 'T'},
    {"url", 1, NULL, 'o'},
    {"user", 1, NULL, 'u'},
    {"userpass", 1, NULL, 'O'},
    {"version", 0, NULL, 'V'},
    {"devices", 1, NULL, 'd'},
    {"diff", 1, NULL, 'f'},
    {"launch", 1, NULL, 'l'},
    {"launch-config", 1, NULL, 'l'},
    {"bfactor", 1, NULL, 1008},
    {"bsleep", 1, NULL, 1009},
    {0, 0, 0, 0}
};

//static struct work g_work;
static struct work g_work2[2];
static time_t g_work_time2[2];
static pthread_mutex_t g_work_lock2[2];

static bool rpc2_login(CURL *curl,int dev);

json_t *json_rpc2_call_recur(CURL *curl, const char *url,
        const char *userpass, json_t *rpc_req,
        int *curl_err, int flags, int recur, int dev) {
    if (recur >= 5) {
        if (opt_debug)
            applog(LOG_DEBUG, "Failed to call rpc command after %i tries", recur);
        return NULL;
    }
    if (!strcmp(rpc2_id2[dev], "")) {
        if (opt_debug)
            applog(LOG_DEBUG, "Tried to call rpc2 command before authentication");
        return NULL;
    }
    json_t *params = json_object_get(rpc_req, "params");
    if (params) {
        json_t *auth_id = json_object_get(params, "id");
        if (auth_id) {
            json_string_set(auth_id, rpc2_id2[dev]);
        }
    }
    json_t *res = json_rpc_call(curl, url, userpass, json_dumps(rpc_req, 0), false, false,
            curl_err,dev);
    if (!res) goto end;
    json_t *error = json_object_get(res, "error");
    if (!error) goto end;
    json_t *message;
    if (json_is_string(error))
        message = error;
    else
        message = json_object_get(error, "message");
    if (!message || !json_is_string(message)) goto end;
    const char *mes = json_string_value(message);
    if (!strcmp(mes, "Unauthenticated")) {
        pthread_mutex_lock(&rpc2_login_lock2[dev]);
        rpc2_login(curl,dev);
        sleep(1);
        pthread_mutex_unlock(&rpc2_login_lock2[dev]);
        return json_rpc2_call_recur(curl, url, userpass, rpc_req,
                curl_err, flags, recur + 1,dev);
    } else if (!strcmp(mes, "Low difficulty share") || !strcmp(mes, "Block expired") || !strcmp(mes, "Invalid job id") || !strcmp(mes, "Duplicate share")) {
        json_t *result = json_object_get(res, "result");
        if (!result) {
            goto end;
        }
        json_object_set(result, "reject-reason", json_string(mes));
    } else {
        applog(LOG_ERR, "json_rpc2.0 error: %s", mes);
        return NULL;
    }
end:
    return res;
}

json_t *json_rpc2_call(CURL *curl, const char *url,
        const char *userpass, const char *rpc_req,
        int *curl_err, int flags,int dev) {
    return json_rpc2_call_recur(curl, url, userpass, JSON_LOADS(rpc_req, NULL),
            curl_err, flags, 0,dev);
}

static bool jobj_binary(const json_t *obj, const char *key,
        void *buf, size_t buflen) {
    const char *hexstr;
    json_t *tmp;

    tmp = json_object_get(obj, key);
    if (unlikely(!tmp)) {
        applog(LOG_ERR, "JSON key '%s' not found", key);
        return false;
    }
    hexstr = json_string_value(tmp);
    if (unlikely(!hexstr)) {
        applog(LOG_ERR, "JSON key '%s' is not a string", key);
        return false;
    }
    if (!hex2bin((unsigned char*) buf, hexstr, buflen))
        return false;

    return true;
}

bool rpc2_job_decode(const json_t *job, struct work *work) {
    if (!jsonrpc_2) {
        applog(LOG_ERR, "Tried to decode job without JSON-RPC 2.0");
        return false;
    }
    int dev = work->dev;
    json_t *tmp;
    tmp = json_object_get(job, "job_id");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval job id");
        goto err_out;
    }
    const char *job_id = json_string_value(tmp);
    tmp = json_object_get(job, "blob");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval blob");
        goto err_out;
    }
    const char *hexblob = json_string_value(tmp);
    int blobLen = (int) strlen(hexblob);
    if (blobLen % 2 != 0 || ((blobLen / 2) < 40 && blobLen != 0) || (blobLen / 2) > 128) {
        applog(LOG_ERR, "JSON invalid blob length");
        goto err_out;
    }
    if (blobLen != 0) {
        pthread_mutex_lock(&rpc2_job_lock2[dev]);
        char *blob = (char *) malloc(blobLen / 2);
        if (!hex2bin((unsigned char *) blob, hexblob, blobLen / 2)) {
            applog(LOG_ERR, "JSON inval blob");
            pthread_mutex_unlock(&rpc2_job_lock2[dev]);
            goto err_out;
        }
        if (rpc2_blob2[dev]) {
            free(rpc2_blob2[dev]);
        }
        rpc2_bloblen2[dev] = blobLen / 2;
        rpc2_blob2[dev] = (char *) malloc(rpc2_bloblen2[dev]);
        memcpy(rpc2_blob2[dev], blob, blobLen / 2);

        free(blob);

        uint32_t target;
        jobj_binary(job, "target", &target, 4);
        if (rpc2_target2[dev] != target) {
            double hashrate = 0.;
            pthread_mutex_lock(&stats_lock2[dev]);
            for (int i = 0; i < opt_n_threads; i++)
                hashrate += thr_hashrates[i];
            pthread_mutex_unlock(&stats_lock2[dev]);
            double difficulty = (((double) 0xffffffff) / target);
            pool_difficulty[dev] = difficulty;
            if(opt_debugDev || dev == 0)
                applog(LOG_INFO, "Pool set diff to %g", difficulty);
            rpc2_target2[dev] = target;
        }

        if (rpc2_job_id2[dev]) {
            free(rpc2_job_id2[dev]);
        }
        rpc2_job_id2[dev] = strdup(job_id);
        pthread_mutex_unlock(&rpc2_job_lock2[dev]);
    }
    if (work) {
        if (!rpc2_blob2[dev]) {
            applog(LOG_ERR, "Requested work before work was received");
            goto err_out;
        }
        memcpy(work->data, rpc2_blob2[dev], rpc2_bloblen2[dev]);
        memset(work->target, 0xff, sizeof (work->target));
        work->target[7] = rpc2_target2[dev];
        strncpy(work->job_id, rpc2_job_id2[dev], 128);
    }
    return true;

err_out:
    return false;
}

static bool work_decode(const json_t *val, struct work *work) {
    int i;

    if (jsonrpc_2) {
        return rpc2_job_decode(val, work);
    }

    if (unlikely(!jobj_binary(val, "data", work->data, sizeof (work->data)))) {
        applog(LOG_ERR, "JSON inval data");
        goto err_out;
    }
    if (unlikely(!jobj_binary(val, "target", work->target, sizeof (work->target)))) {
        applog(LOG_ERR, "JSON inval target");
        goto err_out;
    } else work->maxvote = 0;

    for (i = 0; i < ARRAY_SIZE(work->data); i++)
        work->data[i] = le32dec(work->data + i);
    for (i = 0; i < ARRAY_SIZE(work->target); i++)
        work->target[i] = le32dec(work->target + i);

    return true;

err_out:
    return false;
}

bool rpc2_login_decode(const json_t *val,int dev) {
    const char *id;
    const char *s;

    json_t *res = json_object_get(val, "result");
    if (!res) {
        applog(LOG_ERR, "JSON invalid result");
        goto err_out;
    }

    json_t *tmp;
    tmp = json_object_get(res, "id");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval id");
        goto err_out;
    }
    id = json_string_value(tmp);
    if (!id) {
        applog(LOG_ERR, "JSON id is not a string");
        goto err_out;
    }

    memcpy(&rpc2_id2[dev], id, 64);

    if (opt_debug)
        applog(LOG_DEBUG, "Auth id: %s", id);

    tmp = json_object_get(res, "status");
    if (!tmp) {
        applog(LOG_ERR, "JSON inval status");
        goto err_out;
    }
    s = json_string_value(tmp);
    if (!s) {
        applog(LOG_ERR, "JSON status is not a string");
        goto err_out;
    }
    if (strcmp(s, "OK")) {
        applog(LOG_ERR, "JSON returned status \"%s\"", s);
        return false;
    }

    return true;

err_out:
    return false;
}

static void share_result(int result, const char *reason,int dev) {
    double hashrate;
    int i;

    hashrate = 0.;
    pthread_mutex_lock(&stats_lock2[dev]);
    for (i = 0; i < opt_n_threads; i++)
        hashrate += thr_hashrates[i];
    result ? accepted_count++ : rejected_count++;
    pthread_mutex_unlock(&stats_lock2[dev]);

    applog(LOG_INFO, "accepted: %lu/%lu (%.2f%%), %.2f H/s %s",
            accepted_count, accepted_count + rejected_count,
            100. * accepted_count / (accepted_count + rejected_count), hashrate,
            result ? "(yay!!!)" : "(booooo)");
    if (reason)
        applog(LOG_DEBUG, "reject reason: %s", reason);
}

static void restart_threads(void) {
    int i;

    for (i = 0; i < opt_n_threads; i++)
        work_restart[i].restart = 1;
}

static bool submit_upstream_work(CURL *curl, struct work *work) {
    char *str = NULL;
    json_t *val, *res, *reason;
    char s[345];
    int i;
    bool rc = false;

    int dev = work->dev;


    /* pass if the previous hash is not the current previous hash */
    if (memcmp(work->data + 1, g_work2[dev].data + 1, 32)) {
        if (opt_debug)
            applog(LOG_DEBUG, "DEBUG: stale work detected, discarding");
        snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", }",
                    rpc2_id2[dev], work->job_id);
        if(opt_protocol)
            applog(LOG_DEBUG, "> %s", s);
        return true;
    }

    if (have_stratum2[dev]) {
        uint32_t ntime, nonce;
        uint16_t nvote;
        char *ntimestr, *noncestr, *xnonce2str, *nvotestr;

        if (jsonrpc_2) {
            noncestr = bin2hex(((const unsigned char*) work->data) + 39, 4);
            char hash[32];
            cryptonight_hash((void *) hash, (const void *) work->data, 76);
            char *hashhex = bin2hex((const unsigned char *) hash, 32);
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", \"nonce\": \"%s\", \"result\": \"%s\"}, \"id\":1}",
                    rpc2_id2[dev], work->job_id, noncestr, hashhex);
            free(hashhex);
        } else {
            le32enc(&ntime, work->data[17]);
            le32enc(&nonce, work->data[19]);
            be16enc(&nvote, *((uint16_t*) & work->data[20]));

            ntimestr = bin2hex((const unsigned char *) (&ntime), 4);
            noncestr = bin2hex((const unsigned char *) (&nonce), 4);
            xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);
            nvotestr = bin2hex((const unsigned char *) (&nvote), 2);
            sprintf(s,
                    "{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
                    rpc_user2[dev], work->job_id, xnonce2str, ntimestr, noncestr);
            free(ntimestr);
            free(xnonce2str);
            free(nvotestr);
        }
        free(noncestr);

        if (unlikely(!stratum_send_line(&stratumStorage[dev], s))) {
            applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
            goto out;
        }
    } else {
        /* build JSON-RPC request */
        if (jsonrpc_2) {
            char *noncestr = bin2hex(((const unsigned char*) work->data) + 39, 4);
            char hash[32];
            cryptonight_hash((void *) hash, (const void *) work->data, 76);
            char *hashhex = bin2hex((const unsigned char *) hash, 32);
            snprintf(s, JSON_BUF_LEN,
                    "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", \"nonce\": \"%s\", \"result\": \"%s\"}, \"id\":1}",
                    rpc2_id2[dev], work->job_id, noncestr, hashhex);
            free(noncestr);
            free(hashhex);

            /* issue JSON-RPC request */
            val = json_rpc2_call(curl, rpc_url2[dev], rpc_userpass2[dev], s, NULL, 0,dev);
            if (unlikely(!val)) {
                applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
                goto out;
            }
            res = json_object_get(val, "result");
            json_t *status = json_object_get(res, "status");
            reason = json_object_get(res, "reject-reason");
            share_result(!strcmp(status ? json_string_value(status) : "", "OK"),
                    reason ? json_string_value(reason) : NULL,dev);
        } else {

            /* build hex string */

            for (i = 0; i < ARRAY_SIZE(work->data); i++)
                le32enc(work->data + i, work->data[i]);
            str = bin2hex((unsigned char *) work->data, sizeof (work->data));
            if (unlikely(!str)) {
                applog(LOG_ERR, "submit_upstream_work OOM");
                goto out;
            }

            /* build JSON-RPC request */
            sprintf(s,
                    "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
                    str);

            /* issue JSON-RPC request */
            val = json_rpc_call(curl, rpc_url2[dev], rpc_userpass2[dev], s, false, false, NULL,dev);
            if (unlikely(!val)) {
                applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
                goto out;
            }

            res = json_object_get(val, "result");
            reason = json_object_get(val, "reject-reason");
            share_result(json_is_true(res), reason ? json_string_value(reason) : NULL,dev);
        }

        json_decref(val);
    }

    if(pool_difficulty[0] > 0.0 && pool_difficulty[1] > 0.0)
    {
        double donateFactor = pool_difficulty[0] / pool_difficulty[1];
        weightedDonate = donate * donateFactor;
    }

    pthread_mutex_lock(&dev_lock);
    ++share_count;
    int oldDev = dev;
    if(dev == 1)
    {
        ++count_dev;
    }
    if(share_count > 1)
    {
        double ratio = (double)count_dev / (double)(share_count);
        if(ratio < weightedDonate &&
           donate > 0.0 &&
           stratum_thr_id2[1] != -1 &&
           pool_difficulty[1] > 0.0
        )
            mine_for_dev = 1;
        else
            mine_for_dev = 0;
        if(opt_debugDev)
            applog(LOG_DEBUG, "DEBUG: dev shares %i/%i ratio %lf of %lf: next dev mine? %i\n",
                    count_dev,share_count,ratio,weightedDonate,mine_for_dev);
    }

    if(oldDev!=mine_for_dev)
        restart_threads();
    pthread_mutex_unlock(&dev_lock);

    rc = true;

out:
    free(str);
    return rc;
}

static const char *rpc_req =
        "{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work) {
    json_t *val;
    bool rc;
    struct timeval tv_start, tv_end, diff;

    gettimeofday(&tv_start, NULL);

    pthread_mutex_lock(&dev_lock);
    int dev = mine_for_dev;
    if(stratum_thr_id2[1] == -1)
    {
        mine_for_dev = 0;
        dev = 0;
    }
    pthread_mutex_unlock(&dev_lock);
    work->dev = dev;

    if (jsonrpc_2) {
        char s[128];
        snprintf(s, 128, "{\"method\": \"getjob\", \"params\": {\"id\": \"%s\"}, \"id\":1}\r\n", rpc2_id2[dev]);
        val = json_rpc2_call(curl, rpc_url2[dev], rpc_userpass2[dev], s, NULL, 0,dev);
    }
#if 0
    else {
        val = json_rpc_call(curl, rpc_url2[dev], rpc_userpass2[dev], rpc_req,
                want_longpoll2[dev], false, NULL);
    }
#endif
    gettimeofday(&tv_end, NULL);

    if (have_stratum2[dev]) {
        if (val)
            json_decref(val);
        return true;
    }

    if (!val)
        return false;

    rc = work_decode(json_object_get(val, "result"), work);
    work->dev = dev;

    if (opt_debug && rc) {
        timeval_subtract(&diff, &tv_end, &tv_start);
        applog(LOG_DEBUG, "DEBUG: got new work in %d ms",
                diff.tv_sec * 1000 + diff.tv_usec / 1000);
    }

    json_decref(val);

    return rc;
}

static bool rpc2_login(CURL *curl,int dev) {
    if (!jsonrpc_2) {
        return false;
    }
    json_t *val;
    bool rc;
    struct timeval tv_start, tv_end, diff;
    char s[345];

    snprintf(s, JSON_BUF_LEN, "{\"method\": \"login\", \"params\": {\"login\": \"%s\", \"pass\": \"%s\", \"agent\": \"" USER_AGENT "\"}, \"id\": 1}", rpc_user2[dev], rpc_pass2[dev]);

    gettimeofday(&tv_start, NULL);
    val = json_rpc_call(curl, rpc_url2[dev], rpc_userpass2[dev], s, false, false, NULL,dev);
    gettimeofday(&tv_end, NULL);

    if (!val)
        goto end;

    //    applog(LOG_DEBUG, "JSON value: %s", json_dumps(val, 0));

    rc = rpc2_login_decode(val,dev);

    json_t *result = json_object_get(val, "result");

    if (!result) goto end;

    json_t *job = json_object_get(result, "job");

    if (!rpc2_job_decode(job, &g_work2[dev])) {
        goto end;
    }

    if (opt_debug && rc) {
        timeval_subtract(&diff, &tv_end, &tv_start);
        applog(LOG_DEBUG, "DEBUG: authenticated in %d ms",
                diff.tv_sec * 1000 + diff.tv_usec / 1000);
    }

    json_decref(val);

end:
    return rc;
}

static void workio_cmd_free(struct workio_cmd *wc) {
    if (!wc)
        return;

    switch (wc->cmd) {
        case WC_SUBMIT_WORK:
            free(wc->u.work);
            break;
        default: /* do nothing */
            break;
    }

    memset(wc, 0, sizeof (*wc)); /* poison */
    free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl) {
    struct work *ret_work;
    int failures = 0;

    ret_work = (struct work*) calloc(1, sizeof (*ret_work));
    if (!ret_work)
        return false;

    /* obtain new work from bitcoin via JSON-RPC */
    while (!get_upstream_work(curl, ret_work)) {
        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
            free(ret_work);
            return false;
        }

        /* pause, then restart work-request loop */
        applog(LOG_ERR, "json_rpc_call failed, retry after %d seconds",
                opt_fail_pause);
        sleep(opt_fail_pause);
    }

    /* send work to requesting thread */
    if (!tq_push(wc->thr->q, ret_work))
        free(ret_work);

    return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl) {
    int failures = 0;

    /* submit solution to bitcoin via JSON-RPC */
    while (!submit_upstream_work(curl, wc->u.work)) {
        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "...terminating workio thread");
            return false;
        }

        /* pause, then restart work-request loop */
        applog(LOG_ERR, "...retry after %d seconds",
                opt_fail_pause);
        sleep(opt_fail_pause);
    }

    return true;
}

static bool workio_login(CURL *curl,int dev) {
    int failures = 0;

    /* submit solution to bitcoin via JSON-RPC */
    pthread_mutex_lock(&rpc2_login_lock2[dev]);
    while (!rpc2_login(curl,dev)) {
        if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
            applog(LOG_ERR, "...terminating workio thread");
            pthread_mutex_unlock(&rpc2_login_lock2[dev]);
            return false;
        }

        /* pause, then restart work-request loop */
        applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
        sleep(opt_fail_pause);
        pthread_mutex_unlock(&rpc2_login_lock2[dev]);
        pthread_mutex_lock(&rpc2_login_lock2[dev]);
    }
    pthread_mutex_unlock(&rpc2_login_lock2[dev]);

    return true;
}

static void *workio_thread(void *userdata) {
    struct thr_info *mythr = (struct thr_info*) userdata;
    CURL *curl;
    bool ok = true;

    curl = curl_easy_init();
    if (unlikely(!curl)) {
        applog(LOG_ERR, "CURL initialization failed");
        return NULL;
    }
#if 0
    if (!have_stratum && !opt_benchmark) {
        ok = workio_login(curl);
    }
#endif
    while (ok) {
        struct workio_cmd *wc;

        /* wait for workio_cmd sent to us, on our queue */
        wc = (struct workio_cmd *) tq_pop(mythr->q, NULL);
        if (!wc) {
            ok = false;
            break;
        }

        /* process workio_cmd */
        switch (wc->cmd) {
            case WC_GET_WORK:
                ok = workio_get_work(wc, curl);
                break;
            case WC_SUBMIT_WORK:
                ok = workio_submit_work(wc, curl);
                break;

            default: /* should never happen */
                ok = false;
                break;
        }

        workio_cmd_free(wc);
    }

    tq_freeze(mythr->q);
    curl_easy_cleanup(curl);

    return NULL;
}

static bool get_work(struct thr_info *thr, struct work *work) {
    struct workio_cmd *wc;
    struct work *work_heap;

    if (opt_benchmark) {
        memset(work->data, 0x55, 76);
        work->data[17] = swab32((uint32_t) time(NULL));
        memset(work->data + 19, 0x00, 52);
        work->data[20] = 0x80000000;
        work->data[31] = 0x00000280;
        memset(work->target, 0x00, sizeof (work->target));
        return true;
    }

    /* fill out work request message */
    wc = (struct workio_cmd *) calloc(1, sizeof (*wc));
    if (!wc)
        return false;

    wc->cmd = WC_GET_WORK;
    wc->thr = thr;

    /* send work request to workio thread */
    if (!tq_push(thr_info[work_thr_id].q, wc)) {
        workio_cmd_free(wc);
        return false;
    }

    /* wait for response, a unit of work */
    work_heap = (struct work *) tq_pop(thr->q, NULL);
    if (!work_heap)
        return false;

    /* copy returned work into storage provided by caller */
    memcpy(work, work_heap, sizeof (*work));
    free(work_heap);

    return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in) {
    struct workio_cmd *wc;
    /* fill out work request message */
    wc = (struct workio_cmd *) calloc(1, sizeof (*wc));
    if (!wc)
        return false;

    wc->u.work = (struct work *) malloc(sizeof (*work_in));
    if (!wc->u.work)
        goto err_out;

    wc->cmd = WC_SUBMIT_WORK;
    wc->thr = thr;
    memcpy(wc->u.work, work_in, sizeof (*work_in));

    /* send solution to workio thread */
    if (!tq_push(thr_info[work_thr_id].q, wc))
        goto err_out;

    return true;

err_out:
    workio_cmd_free(wc);
    return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work) {
    unsigned char merkle_root[64];
    int i;

    pthread_mutex_lock(&sctx->work_lock);

    int dev = sctx->work.dev;

    if (jsonrpc_2) {
        memcpy(work, &sctx->work, sizeof (struct work));
        if (sctx->job.job_id) strncpy(work->job_id, sctx->job.job_id, 128);
        pthread_mutex_unlock(&sctx->work_lock);
    } else {
        if (sctx->job.job_id) strncpy(work->job_id, sctx->job.job_id, 128);
        work->xnonce2_len = sctx->xnonce2_size;
        memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

        /* Generate merkle root */
        sha256d(merkle_root, sctx->job.coinbase, (int) sctx->job.coinbase_size);

        for (i = 0; i < sctx->job.merkle_count; i++) {
            memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
            sha256d(merkle_root, merkle_root, 64);
        }

        /* Increment extranonce2 */
        for (i = 0; i < (int) sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

        /* Assemble block header */
        memset(work->data, 0, 128);
        work->data[0] = le32dec(sctx->job.version);
        for (i = 0; i < 8; i++)
            work->data[1 + i] = le32dec((uint32_t *) sctx->job.prevhash + i);
        for (i = 0; i < 8; i++)
            work->data[9 + i] = be32dec((uint32_t *) merkle_root + i);
        work->data[17] = le32dec(sctx->job.ntime);
        work->data[18] = le32dec(sctx->job.nbits);
        work->data[20] = 0x80000000;
        work->data[31] = 0x00000280;

        pthread_mutex_unlock(&sctx->work_lock);

        if (opt_debug) {
            char *xnonce2str = bin2hex(work->xnonce2, sctx->xnonce2_size);
            applog(LOG_DEBUG, "DEBUG: job_id='%s' extranonce2=%s ntime=%08x",
                    work->job_id, xnonce2str, swab32(work->data[17]));
            free(xnonce2str);
        }

        diff_to_target(work->target, sctx->job.diff / opt_difficulty);
    }
}

static void *miner_thread(void *userdata) {
    struct thr_info *mythr = (struct thr_info *) userdata;
    int thr_id = mythr->id;
    struct work work2[2];
    uint32_t max_nonce2[2];
    uint32_t end_nonce;

    unsigned char *scratchbuf = NULL;
    int i;
    static int rounds = 0;
    if (jsonrpc_2)
        end_nonce = 0x00ffffffU / opt_n_threads * (thr_id + 1) - 0x20;
    else
        end_nonce = 0xffffffffU / opt_n_threads * (thr_id + 1) - 0x20;
    for(int d = 0; d < 2; ++d)
    {
        memset(&work2[d], 0, sizeof (struct work)); // prevent work from being used uninitialized
        work2[d].dev = d;
    }
    /* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
     * and if that fails, then SCHED_BATCH. No need for this to be an
     * error if it fails */
    if (!opt_benchmark) {
        setpriority(PRIO_PROCESS, 0, 19);
        drop_policy();
    }

    /* Cpu affinity only makes sense if the number of threads is a multiple
     * of the number of CPUs */
    if (num_processors > 1 && opt_n_threads % num_processors == 0) {
        if (!opt_quiet)
            applog(LOG_INFO, "Binding thread %d to cpu %d",
                thr_id, thr_id % num_processors);
        affine_to_cpu(thr_id, thr_id % num_processors);
    }

    if (device_config[thr_id][0] == 0)
        device_config[thr_id][0] = 4 * device_mpcount[thr_id];

    applog(LOG_INFO, "GPU #%d: %s (%d SMX), using %d blocks of %d threads",
            device_map[thr_id], device_name[thr_id], device_mpcount[thr_id], device_config[thr_id][0], device_config[thr_id][1]);

    if (device_config[thr_id][0] % device_mpcount[thr_id])
        applog(LOG_INFO, "GPU #%d: Warning: block count %d is not a multiple of SMX count %d.",
            device_map[thr_id], device_config[thr_id][0], device_mpcount[thr_id]);

    uint32_t * const nonceptr2[2] ={
        (uint32_t*) (((char*) work2[0].data) + (jsonrpc_2 ? 39 : 76)),
        (uint32_t*) (((char*) work2[1].data) + (jsonrpc_2 ? 39 : 76))
    };

    unsigned long hashes_done2[2]={0lu,0lu};
    while (1) {
        struct timeval tv_start, tv_end, diff;
        int64_t max64;
        int rc;

        pthread_mutex_lock(&dev_lock);
        int dev = mine_for_dev;
        pthread_mutex_unlock(&dev_lock);


        if (have_stratum2[dev]) {
            while (!jsonrpc_2 && time(NULL) >= g_work_time2[dev] + 120)
                sleep(1);
            pthread_mutex_lock(&g_work_lock2[dev]);
            if ((*nonceptr2[dev]) >= end_nonce
                    && !(jsonrpc_2 ? memcmp(work2[dev].data, g_work2[dev].data, 39) ||
                    memcmp(((uint8_t*) work2[dev].data) + 43, ((uint8_t*) g_work2[dev].data) + 43, 33)
                    : memcmp(work2[dev].data, g_work2[dev].data, 76))) {
                stratum_gen_work(&stratumStorage[dev], &g_work2[dev]);
            }
        } else {
            /* obtain new work from internal workio thread */
            pthread_mutex_lock(&g_work_lock2[dev]);
            if (!have_longpoll || time(NULL) >= g_work_time2[dev] + LP_SCANTIME * 3 / 4 || *nonceptr2[dev] >= end_nonce) {
                if (unlikely(!get_work(mythr, &g_work2[dev]))) {
                    applog(LOG_ERR, "work retrieval failed, exiting "
                            "mining thread %d", mythr->id);
                    pthread_mutex_unlock(&g_work_lock2[dev]);
                    goto out;
                }
                g_work_time2[dev] = time(NULL);
            }
        }
        if (jsonrpc_2) {
            if (/*dev!=work2[dev].dev ||*/memcmp(work2[dev].data, g_work2[dev].data, 39) || memcmp(((uint8_t*) work2[dev].data) + 43, ((uint8_t*) g_work2[dev].data) + 43, 33)) {
                memcpy(&work2[dev], &g_work2[dev], sizeof (struct work));
                end_nonce = *nonceptr2[dev] + 0x00ffffffU / opt_n_threads * (thr_id + 1) - 0x20;
                *nonceptr2[dev] += 0x00ffffffU / opt_n_threads * thr_id;
            } else
                *nonceptr2[dev] += hashes_done2[dev];
        } else {
            if (/*dev!=work2[dev].dev ||*/ memcmp(work2[dev].data, g_work2[dev].data, 76)) {
                memcpy(&work2[dev], &g_work2[dev], sizeof (struct work));
                *nonceptr2[dev] = 0xffffffffU / opt_n_threads * thr_id;
            } else
                *nonceptr2[dev] += hashes_done2[dev];
        }

        pthread_mutex_unlock(&g_work_lock2[dev]);
        work_restart[thr_id].restart = 0;

        /* adjust max_nonce to meet target scan time */
        if (have_stratum2[dev])
            max64 = LP_SCANTIME;
        else
            max64 = g_work_time2[dev] + (have_longpoll ? LP_SCANTIME : opt_scantime) - time(NULL);
        max64 *= (int64_t) thr_hashrates[thr_id];
        if (max64 <= 0)
            max64 = 0x200LL;
        if ((int64_t) (*nonceptr2[dev]) + max64 > end_nonce)
            max_nonce2[dev] = end_nonce;
        else
            max_nonce2[dev] = (uint32_t) (*nonceptr2[dev] + max64);

        hashes_done2[dev] = 0;
        gettimeofday(&tv_start, NULL);

        uint32_t results[2];

        /* scan nonces for a proof-of-work hash */
        rc = scanhash_cryptonight(thr_id, work2[dev].data, work2[dev].target, max_nonce2[dev], &hashes_done2[dev], results);

        /* record scanhash elapsed time */
        gettimeofday(&tv_end, NULL);
        timeval_subtract(&diff, &tv_end, &tv_start);
        if (diff.tv_usec || diff.tv_sec) {
            pthread_mutex_lock(&stats_lock2[dev]);
            thr_hashrates[thr_id] = hashes_done2[dev] / (diff.tv_sec + 1e-6 * diff.tv_usec);
            pthread_mutex_unlock(&stats_lock2[dev]);
        }
        if (!opt_quiet)
            applog(LOG_INFO, "GPU #%d: %s, %.2f H/s", device_map[thr_id], device_name[thr_id], thr_hashrates[thr_id]);

        if (opt_benchmark && thr_id == opt_n_threads - 1) {
            double hashrate = 0.;
            for (i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
                hashrate += thr_hashrates[i];
            if (i == opt_n_threads) {
                applog(LOG_INFO, "Total: %.2f H/s", hashrate);
            }
        }

        /* if nonce found, submit work */
        if (rc && !opt_benchmark) {
            uint32_t backup = *nonceptr2[dev];
            *nonceptr2[dev] = results[0];
            submit_work(mythr, &work2[dev]);
            if (rc > 1) {
                *nonceptr2[dev] = results[1];
                submit_work(mythr, &work2[dev]);
            }
            *nonceptr2[dev] = backup;
        }
    }

out:
    tq_freeze(mythr->q);

    return NULL;
}



#if 0
static void *longpoll_thread(void *userdata) {
    struct thr_info *mythr = (struct thr_info *) userdata;
    CURL *curl = NULL;
    char *copy_start, *hdr_path = NULL, *lp_url = NULL;
    bool need_slash = false;

    curl = curl_easy_init();
    if (unlikely(!curl)) {
        applog(LOG_ERR, "CURL initialization failed");
        goto out;
    }

start:
    hdr_path = (char*) tq_pop(mythr->q, NULL);
    if (!hdr_path)
        goto out;

    /* full URL */
    if (strstr(hdr_path, "://")) {
        lp_url = hdr_path;
        hdr_path = NULL;
    }
        /* absolute path, on current server */
    else {
        copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
        if (rpc_url2[dev][strlen(rpc_url) - 1] != '/')
            need_slash = true;

        lp_url = (char*) malloc(strlen(rpc_url2[dev]) + strlen(copy_start) + 2);
        if (!lp_url)
            goto out;

        sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
    }

    applog(LOG_INFO, "Long-polling activated for %s", lp_url);

    while (1) {
        json_t *val, *soval;
        int err;

        if (jsonrpc_2) {
            pthread_mutex_lock(&rpc2_login_lock);
            if (!strcmp(rpc2_id2[dev], "")) {
                sleep(1);
                continue;
            }
            char s[128];
            snprintf(s, 128, "{\"method\": \"getjob\", \"params\": {\"id\": \"%s\"}, \"id\":1}\r\n", rpc2_id);
            pthread_mutex_unlock(&rpc2_login_lock);
            val = json_rpc2_call(curl, rpc_url, rpc_userpass, s, &err, JSON_RPC_LONGPOLL,dev);
        } else {
            val = json_rpc_call(curl, lp_url, rpc_userpass, rpc_req,
                    false, true, &err);
        }
        if (have_stratum2) {
            if (val)
                json_decref(val);
            goto out;
        }
        if (likely(val)) {

            if (!jsonrpc_2) {
                soval = json_object_get(json_object_get(val, "result"), "submitold");
                submit_old = soval ? json_is_true(soval) : false;
            }
            pthread_mutex_lock(&g_work_lock);
            char *start_job_id = strdup(g_work.job_id);
            if (work_decode(json_object_get(val, "result"), &g_work)) {
                if (strcmp(start_job_id, g_work.job_id)) {
                    if (!opt_quiet) applog(LOG_INFO, "LONGPOLL detected new block");
                    if (opt_debug)
                        applog(LOG_DEBUG, "DEBUG: got new work");
                    time(&g_work_time);
                    restart_threads();
                }
            }
            pthread_mutex_unlock(&g_work_lock);
            json_decref(val);
        } else {
            pthread_mutex_lock(&g_work_lock);
            g_work_time -= LP_SCANTIME;
            pthread_mutex_unlock(&g_work_lock);
            if (err == CURLE_OPERATION_TIMEDOUT) {
                restart_threads();
            } else {
                have_longpoll = false;
                restart_threads();
                free(hdr_path);
                free(lp_url);
                lp_url = NULL;
                sleep(opt_fail_pause);
                goto start;
            }
        }
    }

out:
    free(hdr_path);
    free(lp_url);
    tq_freeze(mythr->q);
    if (curl)
        curl_easy_cleanup(curl);

    return NULL;
}
#endif

static bool stratum_handle_response(char *buf,int dev) {
    json_t *val, *err_val, *res_val, *id_val;
    json_error_t err;
    bool ret = false;
    bool valid = false;

    val = JSON_LOADS(buf, &err);
    if (!val) {
        applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
        goto out;
    }

    res_val = json_object_get(val, "result");
    err_val = json_object_get(val, "error");
    id_val = json_object_get(val, "id");

    if (!id_val || json_is_null(id_val) || (!res_val && !err_val))
        goto out;

    if (jsonrpc_2) {
        char *s;
        json_t *status = json_object_get(res_val, "status");
        if (status != NULL)
            s = (char*) json_string_value(status);

        // Keepalive response handling.
        if (status != NULL && strcmp(s, "KEEPALIVED") == 0) {
            goto out;
        }

        if (status != NULL) {
            valid = !strcmp(s, "OK") && json_is_null(err_val);
        } else {
            valid = json_is_null(err_val);
        }
    } else {
        valid = json_is_true(res_val);
    }

    if (err_val) {
        /*	if(jsonrpc_2)
                    share_result(valid, json_string_value(err_val));
                else */
        share_result(valid, json_string_value(json_object_get(err_val, "message")),dev);
    } else
        share_result(valid, NULL,dev);

    ret = true;
out:
    if (val)
        json_decref(val);

    return ret;
}

void disableDonation()
{
    donate = 0.0;
    weightedDonate = 0.0;
    pthread_mutex_lock(&dev_lock);
    mine_for_dev = 0;
    pthread_mutex_unlock(&dev_lock);
}

static void *stratum_thread(void *userdata) {

    char *s;
    int ff = 0;
    struct thr_info *mythr = (struct thr_info *) userdata;

    //seach my dev id
    for(int d = 0; d < 2; ++d)
    {
        if(mythr->id == stratum_thr_id2[d])
        {
            ff=d;
            break;
        }
    }

    stratumStorage[ff].url = (char*) tq_pop(mythr->q, NULL);

    if(ff == 1)
    {
        // max loop time 2 min
        for(int l = 0; l < 20; ++l)
        {
            //wait for pool diff
            sleep(6);
            if(pool_difficulty[0] > 0.0)
            {
                int poolDiff = (int)pool_difficulty[0];
                char* tmpDiff = (char*) malloc(20);
                memset(tmpDiff,0,20);
                sprintf(tmpDiff,"%i",poolDiff);
                char* tmp = (char*) malloc(strlen(devPoolAddress) + strlen(tmpDiff) + 2);
                if (!tmp)
                {
                    disableDonation();
                    applog(LOG_ERR, "Stratum username generation error, mining without dev donation");
                    return NULL;
                }
                sprintf(tmp, "%s+%s", devPoolAddress, tmpDiff);
                free(rpc_user2[1]);
                rpc_user2[1] = tmp;

                // create user:password
                free(rpc_userpass2[1]);
                rpc_userpass2[1] = (char*) malloc(strlen(rpc_user2[1]) + strlen(rpc_pass2[1]) + 2);
                if (!rpc_userpass2[1])
                {
                    disableDonation();
                    applog(LOG_ERR, "Stratum username generation error, mining without dev donation");
                    return NULL;
                }
                sprintf(rpc_userpass2[1], "%s:%s", rpc_user2[1], rpc_pass2[1]);
                break;
            }
        }
    }

    if (!stratumStorage[ff].url)
        goto out;
    if(opt_debugDev || ff == 0)
        applog(LOG_INFO, "Starting Stratum on %s", stratumStorage[ff].url);


    pthread_mutex_lock(&dev_lock);
    int dev = mine_for_dev;
    pthread_mutex_unlock(&dev_lock);

    while (1) {
        int failures = 0;

        pthread_mutex_lock(&dev_lock);
        dev = mine_for_dev;
        pthread_mutex_unlock(&dev_lock);

        while (!stratumStorage[ff].curl) {
            pthread_mutex_lock(&g_work_lock2[ff]);
            g_work_time2[ff] = 0;
            pthread_mutex_unlock(&g_work_lock2[ff]);
            if(dev == ff) restart_threads();

            if (!stratum_connect(&stratumStorage[ff], stratumStorage[ff].url) ||
                    !stratum_subscribe(&stratumStorage[ff]) ||
                    !stratum_authorize(&stratumStorage[ff], rpc_user2[ff], rpc_pass2[ff])) {
                stratum_disconnect(&stratumStorage[ff]);
                if (opt_retries >= 0 && ++failures > opt_retries) {
                    if(opt_debugDev || dev==ff)
                        applog(LOG_ERR, "...terminating workio thread");
                    if(ff == 1)
                    {
                        disableDonation();
                        applog(LOG_ERR, "Stratum dev pool connection failed, mining without dev donation");
                    }
                    else
                    {
                        applog(LOG_ERR, "...terminating workio thread");
                        tq_push(thr_info[work_thr_id].q, NULL);
                        //pthread_kill(stratum_thr_id2[1],-9); //terminate dev pool stratum thread
                    }
                    goto out;
                }
                if(opt_debugDev || dev==ff)
                {
                    applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
                    if(ff == 1)
                    {
                        pthread_mutex_lock(&dev_lock);
                        mine_for_dev = 0;
                        pthread_mutex_unlock(&dev_lock);
                    }
                }
                sleep(opt_fail_pause);
            }
        }

        if (jsonrpc_2) {
            if (stratumStorage[ff].work.job_id
                    && (!g_work_time2[ff]
                    || strcmp(stratumStorage[ff].work.job_id, g_work2[ff].job_id))) {
                pthread_mutex_lock(&g_work_lock2[ff]);
                stratum_gen_work(&stratumStorage[ff], &g_work2[ff]);
                time(&g_work_time2[ff]);
                pthread_mutex_unlock(&g_work_lock2[ff]);
                if(opt_debugDev || dev==ff)
                    applog(LOG_INFO, "Stratum detected new block");
                if(dev==ff) restart_threads();
            }
        } else {
            if (stratumStorage[ff].job.job_id &&
                    (strcmp(stratumStorage[ff].job.job_id, g_work2[ff].job_id) || !g_work_time2[ff])) {
                pthread_mutex_lock(&g_work_lock2[ff]);
                stratum_gen_work(&stratumStorage[ff], &g_work2[ff]);
                time(&g_work_time2[ff]);
                pthread_mutex_unlock(&g_work_lock2[ff]);
                if (stratumStorage[ff].job.clean) {
                    if (!opt_quiet && (dev == ff || opt_debugDev)) applog(LOG_INFO, "Stratum detected new block");
                    if(dev==ff) restart_threads();
                }
            }
        }

        // Should we send a keepalive?
        if (opt_keepalive2[ff] && !stratum_socket_full(&stratumStorage[ff], 90)) {
            if(opt_debugDev || dev==ff)
                applog(LOG_INFO, "Keepalive send...");
            stratum_keepalived(&stratumStorage[ff], rpc2_id2[ff]);
        }

        if (!stratum_socket_full(&stratumStorage[ff], 120)) {
            if(opt_debugDev || dev==ff)
                applog(LOG_ERR, "Stratum connection timed out");
            s = NULL;
        } else
            s = stratum_recv_line(&stratumStorage[ff]);
        if (!s) {
            stratum_disconnect(&stratumStorage[ff]);
            if(opt_debugDev || dev==ff)
                applog(LOG_ERR, "Stratum connection interrupted");
            continue;
        }
        if (!stratum_handle_method(&stratumStorage[ff], s))
            stratum_handle_response(s,ff);
        free(s);
    }

out:
    stratum_thr_id2[ff] = -1;
    return NULL;
}

static void show_version_and_exit(void) {
    printf("%s\n%s\n", PACKAGE_STRING, curl_version());
    exit(0);
}

static void show_usage_and_exit(int status) {
    if (status)
        fprintf(stderr, "Try `" PROGRAM_NAME " --help' for more information.\n");
    else
        printf(usage);
    exit(status);
}

void parse_device_config(int device, char *config, int *blocks, int *threads) {
    char *p;
    int tmp_blocks, tmp_threads;

    if (config == NULL) goto usedefault;


    p = strtok(config, "x");
    if (!p)
        goto usedefault;

    tmp_threads = atoi(p);
    if (tmp_threads < 4 || tmp_threads > 1024)
        goto usedefault;

    p = strtok(NULL, "x");
    if (!p)
        goto usedefault;

    tmp_blocks = atoi(p);
    if (tmp_blocks < 1)
        goto usedefault;

    *blocks = tmp_blocks;
    *threads = tmp_threads;
    return;

usedefault:
    *blocks = 4 * device_mpcount[device];
    *threads = opt_cn_threads;
    return;

}

static void parse_arg(int key, char *arg) {
    char *p;
    int v, i;
    double d;

    switch (key) {
        case 'a':
            applog(LOG_INFO, "Ignoring algo switch, this program does only cryptonight now.");
            break;
        case 'B':
            opt_background = true;
            break;
        case 'c':
        {
            json_error_t err;
            if (opt_config)
                json_decref(opt_config);
#if JANSSON_VERSION_HEX >= 0x020000
            opt_config = json_load_file(arg, 0, &err);
#else
            opt_config = json_load_file(arg, &err);
#endif
            if (!json_is_object(opt_config)) {
                applog(LOG_ERR, "JSON decode of %s failed", arg);
                exit(1);
            }
            break;
        }
        case 'k':
            opt_keepalive2[0] = true;
            applog(LOG_INFO, "Keepalive actived");
            break;
        case 'q':
            opt_quiet = true;
            break;
        case 'D':
            opt_debug = true;
            break;
        case 'p':
            free(rpc_pass2[0]);
            rpc_pass2[0] = strdup(arg);
            break;
        case 'P':
            opt_protocol = true;
            break;
        case 'r':
            v = atoi(arg);
            if (v < -1 || v > 9999) /* sanity check */
                show_usage_and_exit(1);
            opt_retries = v;
            break;
        case 'R':
            v = atoi(arg);
            if (v < 1 || v > 9999) /* sanity check */
                show_usage_and_exit(1);
            opt_fail_pause = v;
            break;
        case 's':
            v = atoi(arg);
            if (v < 1 || v > 9999) /* sanity check */
                show_usage_and_exit(1);
            opt_scantime = v;
            break;
        case 'T':
            v = atoi(arg);
            if (v < 1 || v > 99999) /* sanity check */
                show_usage_and_exit(1);
            opt_timeout = v;
            break;
        case 't':
            v = atoi(arg);
            if (v < 1 || v > 9999) /* sanity check */
                show_usage_and_exit(1);
            opt_n_threads = v;
            break;
        case 'v':
            break;
        case 'm':
            opt_trust_pool = true;
            break;
        case 'u':
            free(rpc_user2[0]);
            rpc_user2[0] = strdup(arg);
            break;
        case 'z':
            donate = strtod(arg, NULL) / 100.0;
            if(donate > 1.0)
            {
                applog(LOG_ERR, "Invalid donation value '%s', set donation to 100 percent. (valid range [0.0;100.0])", arg);
                donate = 1.0;
            }
            else if(donate < 0.0)
            {
                applog(LOG_ERR, "Invalid donation value '%s', set donation to default. (valid range [0.0;100.0])", arg);
                donate = 0.02;
            }
            weightedDonate = donate;
            break;
        case 'Z':
            opt_debugDev = true;
            break;
        case 'o': /* --url */
            p = strstr(arg, "://");
            if (p) {
                if (strncasecmp(arg, "http://", 7)
                        && strncasecmp(arg, "https://", 8)
                        && strncasecmp(arg, "stratum+tcp://", 14))
                    show_usage_and_exit(1);
                free(rpc_url2[0]);
                rpc_url2[0] = strdup(arg);
            } else {
                if (!strlen(arg) || *arg == '/')
                    show_usage_and_exit(1);
                free(rpc_url2[0]);
                rpc_url2[0] = (char*) malloc(strlen(arg) + 8);
                sprintf(rpc_url2[0], "http://%s", arg);
            }
            p = strrchr(rpc_url2[0], '@');
            if (p) {
                char *sp, *ap;
                *p = '\0';
                ap = strstr(rpc_url2[0], "://") + 3;
                sp = strchr(ap, ':');
                if (sp) {
                    free(rpc_userpass2[0]);
                    rpc_userpass2[0] = strdup(ap);
                    free(rpc_user2[0]);
                    rpc_user2[0] = (char*) calloc(sp - ap + 1, 1);
                    strncpy(rpc_user2[0], ap, sp - ap);
                    free(rpc_pass2[0]);
                    rpc_pass2[0] = strdup(sp + 1);
                } else {
                    free(rpc_user2[0]);
                    rpc_user2[0] = strdup(ap);
                }
                memmove(ap, p + 1, strlen(p + 1) + 1);
            }
            have_stratum2[0] = !opt_benchmark && !strncasecmp(rpc_url2[0], "stratum", 7);
            break;
        case 'O': /* --userpass */
            p = strchr(arg, ':');
            if (!p)
                show_usage_and_exit(1);
            free(rpc_userpass2[0]);
            rpc_userpass2[0] = strdup(arg);
            free(rpc_user2[0]);
            rpc_user2[0] = (char*) calloc(p - arg + 1, 1);
            strncpy(rpc_user2[0], arg, p - arg);
            free(rpc_pass2[0]);
            rpc_pass2[0] = strdup(p + 1);
            break;
        case 'x': /* --proxy */
            if (!strncasecmp(arg, "socks4://", 9))
                opt_proxy_type = CURLPROXY_SOCKS4;
            else if (!strncasecmp(arg, "socks5://", 9))
                opt_proxy_type = CURLPROXY_SOCKS5;
#if LIBCURL_VERSION_NUM >= 0x071200
            else if (!strncasecmp(arg, "socks4a://", 10))
                opt_proxy_type = CURLPROXY_SOCKS4A;
            else if (!strncasecmp(arg, "socks5h://", 10))
                opt_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
#endif
            else
                opt_proxy_type = CURLPROXY_HTTP;
            free(opt_proxy);
            opt_proxy = strdup(arg);
            break;
        case 1001:
            free(opt_cert);
            opt_cert = strdup(arg);
            break;
        case 1005:
            opt_benchmark = true;
            want_longpoll = false;
            want_stratum2[0] = false;
            have_stratum2[0] = false;
            break;
        case 1003:
            want_longpoll = false;
            break;
        case 1007:
            want_stratum2[0] = false;
            break;
        case 'S':
            use_syslog = true;
            break;
        case 'd': // CB
        {
            char * pch = strtok(arg, ",");
            opt_n_threads = 0;
            while (pch != NULL) {
                if (pch[0] >= '0' && pch[0] <= '9' && pch[1] == '\0') {
                    if (atoi(pch) < num_processors)
                        device_map[opt_n_threads++] = atoi(pch);
                    else {
                        applog(LOG_ERR, "Non-existant CUDA device #%d specified in -d option", atoi(pch));
                        exit(1);
                    }
                } else {
                    int device = cuda_finddevice(pch);
                    if (device >= 0 && device < num_processors)
                        device_map[opt_n_threads++] = device;
                    else {
                        applog(LOG_ERR, "Non-existant CUDA device '%s' specified in -d option", pch);
                        exit(1);
                    }
                }
                pch = strtok(NULL, ",");
            }
        }
            break;
        case 'f': // CH - Divisor for Difficulty
            d = atof(arg);
            if (d == 0) /* sanity check */
                show_usage_and_exit(1);
            opt_difficulty = d;
            break;
        case 'l': /* cryptonight launch config */
        {
            char *tmp_config[8];
            int tmp_blocks = opt_cn_blocks, tmp_threads = opt_cn_threads;
            for (i = 0; i < 8; i++) tmp_config[i] = NULL;
            p = strtok(arg, ",");
            if (p == NULL) show_usage_and_exit(1);
            i = 0;
            while (p != NULL && i < 8) {
                tmp_config[i++] = strdup(p);
                p = strtok(NULL, ",");
            }
            while (i < 8) {
                tmp_config[i] = strdup(tmp_config[i - 1]);
                i++;
            }

            for (i = 0; i < 8; i++) {
                parse_device_config(i, tmp_config[i], &tmp_blocks, &tmp_threads);
                device_config[i][0] = tmp_blocks;
                device_config[i][1] = tmp_threads;
            }
        }
            break;
        case 1008:
        {
            p = strtok(arg, ",");
            if (p == NULL) show_usage_and_exit(1);
            int last;
            i = 0;
            while (p != NULL && i < 8) {
                device_bfactor[i++] = last = atoi(p);
                if (last < 0 || last > 10) {
                    applog(LOG_ERR, "Valid range for --bfactor is 0-10");
                    exit(1);
                }
                p = strtok(NULL, ",");
            }
            while (i < 8) {
                device_bfactor[i++] = last;
            }
        }
            break;
        case 1009:
            p = strtok(arg, ",");
            if (p == NULL) show_usage_and_exit(1);
            int last;
            i = 0;
            while (p != NULL && i < 8) {
                device_bsleep[i++] = last = atoi(p);
                if (last < 0 || last > 1000000) {
                    applog(LOG_ERR, "Valid range for --bsleep is 0-1000000");
                    exit(1);
                }
                p = strtok(NULL, ",");
            }
            while (i < 8) {
                device_bsleep[i++] = last;
            }
            break;

        case 'V':
            show_version_and_exit();
        case 'h':
            show_usage_and_exit(0);
        default:
            show_usage_and_exit(1);
    }
}

static void parse_config(void) {
    int i;
    json_t *val;

    if (!json_is_object(opt_config))
        return;

    for (i = 0; i < ARRAY_SIZE(options); i++) {
        if (!options[i].name)
            break;
        if (!strcmp(options[i].name, "config"))
            continue;

        val = json_object_get(opt_config, options[i].name);
        if (!val)
            continue;

        if (options[i].has_arg && json_is_string(val)) {
            char *s = strdup(json_string_value(val));
            if (!s)
                break;
            parse_arg(options[i].val, s);
            free(s);
        } else if (!options[i].has_arg && json_is_true(val))
            parse_arg(options[i].val, "");
        else
            applog(LOG_ERR, "JSON option %s invalid",
                options[i].name);
    }
}

static void parse_cmdline(int argc, char *argv[]) {
    int key;

    while (1) {
#if HAVE_GETOPT_LONG
        key = getopt_long(argc, argv, short_options, options, NULL);
#else
        key = getopt(argc, argv, short_options);
#endif
        if (key < 0)
            break;

        parse_arg(key, optarg);
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unsupported non-option argument '%s'\n",
                argv[0], argv[optind]);
        show_usage_and_exit(1);
    }
    parse_config();
}

#ifndef WIN32

static void signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            applog(LOG_INFO, "SIGHUP received");
            break;
        case SIGINT:
            applog(LOG_INFO, "SIGINT received, exiting");
            exit(0);
            break;
        case SIGTERM:
            applog(LOG_INFO, "SIGTERM received, exiting");
            exit(0);
            break;
    }
}
#endif

static int msver(void) {
    int version;
#ifdef _MSC_VER
    switch (_MSC_VER) {
        case 1500: version = 2008;
            break;
        case 1600: version = 2010;
            break;
        case 1700: version = 2012;
            break;
        case 1800: version = 2013;
            break;
        case 1900: version = 2015;
            break;
        default: version = _MSC_VER / 100;
    }
#else
    version = 0;
#endif
    return version;
}

#define PROGRAM_VERSION "1.03"

int main(int argc, char *argv[]) {

    rpc_user2[0] = strdup("");
    rpc_pass2[0] = strdup("");
    rpc_url2[0] = NULL;
    rpc_url2[1] = strdup("stratum+tcp://xmr.crypto-pool.fr:80");
    rpc_userpass2[0] = NULL;
    rpc_userpass2[1] = NULL;
    rpc_user2[1] = strdup(devPoolAddress);
    rpc_pass2[1] = strdup("x");

    struct thr_info *thr;
    long flags;
    int i;
    /*
#ifdef WIN32
    SYSTEM_INFO sysinfo;
#endif
     */
#if defined _WIN64 || defined _LP64
    int bits = 64;
#else
    int bits = 32;
#endif
    printf("    *** xmrMiner %s (%d bit) for NVIDIA GPUs by psychocrypt \n", PACKAGE_VERSION, bits);
#ifdef _MSC_VER
    printf("    *** Built with Visual Studio %d ", msver());
#else
#ifdef __clang__
    printf("    *** Built with Clang %s ", __clang_version__);
#else
#ifdef __GNUC__
    printf("    *** Built with GCC %d.%d ", __GNUC__, __GNUC_MINOR__);
#else
    printf("    *** Built with an unusual compiler ");
#endif
#endif
#endif
    printf(" using the Nvidia CUDA Toolkit %d.%d\n\n", CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
    printf(" psychocrypt's XMR donation address:\n   43NoJVEXo21hGZ6tDG6Z3g4qimiGdJPE6GRxAmiWwm26gwr62Lqo7zRiCJFSBmbkwTGNuuES9ES5TgaVHceuYc4Y75txCTU\n");
    printf(" for more donation addresses please read the README.md\n");
    printf("-----------------------------------------------------------------\n");


    pthread_mutex_init(&dev_lock, NULL);
    pthread_mutex_init(&applog_lock, NULL);
    num_processors = cuda_num_devices();

    for (i = 0; i < 8; i++) {
        device_config[i][0] = opt_cn_blocks;
        device_config[i][1] = opt_cn_threads;
        device_bfactor[i] = default_bfactor;
        device_bsleep[i] = default_bsleep;
    }

    /* parse command line */
    parse_cmdline(argc, argv);

    // create user:password
    rpc_userpass2[1] = (char*) malloc(strlen(rpc_user2[1]) + strlen(rpc_pass2[1]) + 2);
    if (!rpc_userpass2[1])
        return 1;
    sprintf(rpc_userpass2[1], "%s:%s", rpc_user2[1], rpc_pass2[1]);

    cuda_deviceinfo();

    jsonrpc_2 = true;
    applog(LOG_INFO, "Using JSON-RPC 2.0");

    if (!opt_benchmark && !rpc_url2[0]) {
        fprintf(stderr, "%s: no URL supplied\n", argv[0]);
        show_usage_and_exit(1);
    }

    if (!rpc_userpass2[0]) {
        rpc_userpass2[0] = (char*) malloc(strlen(rpc_user2[0]) + strlen(rpc_pass2[0]) + 2);
        if (!rpc_userpass2[0])
            return 1;
        sprintf(rpc_userpass2[0], "%s:%s", rpc_user2[0], rpc_pass2[0]);
    }

    for(int d = 0; d < 2; ++d)
    {
        pthread_mutex_init(&stats_lock2[d], NULL);
        pthread_mutex_init(&g_work_lock2[d], NULL);
        pthread_mutex_init(&rpc2_job_lock2[d], NULL);
        pthread_mutex_init(&stratumStorage[d].sock_lock, NULL);
        pthread_mutex_init(&stratumStorage[d].work_lock, NULL);
        stratumStorage[d].work.dev = d;
        memset(&g_work2[d],0,sizeof(struct work));
        g_work2[d].dev = d;
        g_work_time2[d] = time(NULL);
    }


    flags = CURL_GLOBAL_ALL;
    if (curl_global_init(flags)) {
        applog(LOG_ERR, "CURL initialization failed");
        return 1;
    }

#ifndef WIN32
    if (opt_background) {
        i = fork();
        if (i < 0) exit(1);
        if (i > 0) exit(0);
        i = setsid();
        if (i < 0)
            applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
        i = chdir("/");
        if (i < 0)
            applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
        signal(SIGHUP, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
    }
#endif

    if (num_processors == 0) {
        applog(LOG_ERR, "No CUDA devices found! terminating.");
        exit(1);
    }
    if (!opt_n_threads)
        opt_n_threads = num_processors;

#ifdef HAVE_SYSLOG_H
    if (use_syslog)
        openlog("xmrMiner", LOG_PID, LOG_USER);
#endif

    work_restart = (struct work_restart *) calloc(opt_n_threads, sizeof (*work_restart));
    if (!work_restart)
        return 1;

    thr_info = (struct thr_info *) calloc(opt_n_threads + 3, sizeof (*thr));
    if (!thr_info)
        return 1;

    thr_hashrates = (double *) calloc(opt_n_threads, sizeof (double));
    if (!thr_hashrates)
        return 1;

    /* init workio thread info */
    work_thr_id = opt_n_threads;
    thr = &thr_info[work_thr_id];
    thr->id = work_thr_id;
    thr->q = tq_new();
    if (!thr->q)
        return 1;

    /* start work I/O thread */
    if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
        applog(LOG_ERR, "workio thread create failed");
        return 1;
    }

#if 0
    if (want_longpoll && !have_stratum) {
        /* init longpoll thread info */
        longpoll_thr_id = opt_n_threads + 1;
        thr = &thr_info[longpoll_thr_id];
        thr->id = longpoll_thr_id;
        thr->q = tq_new();
        if (!thr->q)
            return 1;

        /* start longpoll thread */
        if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
            applog(LOG_ERR, "longpoll thread create failed");
            return 1;
        }
    }
#endif
    for(int d = 0; d < 2; ++d){
        if (want_stratum2[d] && (d==0 || !opt_benchmark)) {
            /* init stratum thread info */
            stratum_thr_id2[d] = opt_n_threads + d + 1;
            thr = &thr_info[stratum_thr_id2[d]];
            thr->id = stratum_thr_id2[d];
            thr->q = tq_new();
            if (!thr->q)
                return 1;

            /* start stratum thread */
            if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
                applog(LOG_ERR, "stratum thread create failed");
                return 1;
            }

            // this call is useless, please remove first read in the thread
            if (have_stratum2[d])
                tq_push(thr_info[stratum_thr_id2[d]].q, strdup(rpc_url2[d]));
        }
    }

    // wait that the stratum thread is initialized
    sleep(2);

    /* start mining threads */
    for (i = 0; i < opt_n_threads; i++) {
        thr = &thr_info[i];

        thr->id = i;
        thr->q = tq_new();
        if (!thr->q)
            return 1;

        if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
            applog(LOG_ERR, "thread %d create failed", i);
            return 1;
        }
    }

    applog(LOG_INFO, "%d miner threads started, "
            "using '%s' algorithm.",
            opt_n_threads,
            algo_names[opt_algo]);

#ifdef WIN32
    timeBeginPeriod(1); // enable high timer precision (similar to Google Chrome Trick)
#endif

    /* main loop - simply wait for workio thread to exit */
    pthread_join(thr_info[work_thr_id].pth, NULL);

#ifdef WIN32
    timeEndPeriod(1); // be nice and forego high timer precision
#endif

    applog(LOG_INFO, "workio thread dead, exiting.");

    return 0;
}

