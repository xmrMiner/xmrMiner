# Command Line Interface

 -d, --devices         gives a comma separated list of CUDA device IDs
                       to operate on. Device IDs start counting from 0!
                       Alternatively give string names of your card like
                       gtx780ti or gt640#2 (matching 2nd gt640 in the PC).
 -l, --launch=CONFIG   launch config for the Cryptonight kernel.
                       a comma separated list of values in form of
                       AxB where A is the number of threads to run in
                       each thread block and B is the number of thread
                       blocks to launch. If less values than devices in use
                       are provided, the last value will be used for
                       the remaining devices. If you don't need to vary the
                       value between devices, you can just enter a single
		       value and it will be used for all devices.
		       (default: 8x40)
     --bfactor=X       Enables running the Cryptonight kernel in smaller pieces.\n\
                       The kernel will be run in 2^X parts according to bfactor,\n\
                       with a small pause between parts, specified by --bsleep.\n\
                       This is a per-device setting like the launch config.\n\
                       (default: 0 (no splitting) on Linux, 6 (64 parts) on Windows)\n\
     --bsleep=X        Insert a delay of X microseconds between kernel launches.\n\
                       Use in combination with --bfactor to mitigate the lag\n\
                       when running on your primary GPU.\n\
                       This is a per-device setting like the launch config.\n\
 -f, --diff            Divide difficulty by this factor (std is 1)
 -o, --url=URL         URL of mining server (default: " DEF_RPC_URL ")
 -O, --userpass=U:P    username:password pair for mining server
 -u, --user=USERNAME   username for mining server
 -p, --pass=PASSWORD   password for mining server
     --cert=FILE       certificate for mining server using SSL
 -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy
 -t, --threads=N       number of miner threads
                       (default: number of nVidia GPUs in your system)
 -r, --retries=N       number of times to retry if a network call fails
                         (default: retry indefinitely)
 -R, --retry-pause=N   time to pause between retries, in seconds (default: 15)
 -T, --timeout=N       network timeout, in seconds (default: 270)
 -k, --keepalive       send keepalive requests to avoid a stratum timeout
 -s, --scantime=N      upper bound on time spent scanning current work when
                       long polling is unavailable, in seconds (default: 5)
     --no-longpoll     disable X-Long-Polling support
     --no-stratum      disable X-Stratum support
 -q, --quiet           disable per-thread hashmeter output
 -D, --debug           enable debug output
 -P, --protocol-dump   verbose dump of protocol-level activities
 -B, --background      run the miner in the background
     --benchmark       run in offline benchmark mode
 -c, --config=FILE     load a JSON-format configuration file
 -V, --version         display version information and exit
 -h, --help            display this help text and exit


>>> AUTHORS <<<

Notable contributors to this application are:
KlausT:
- various fixes and optimizations

tsiv:
- CUDA implementation for the Cryptonight algorithm.

Christian Buchner, Christian H. (Germany):
- modifying the original pooler-cpuminer for use with CUDA.

Jeff Garzik, pooler + contributors:
- The original pooler-cpuminer project

LucasJones:
 - JSON-RPC 2.0 handling and the Cryptonight C-code comes
   from his cpuminer fork, cpuminer-multi

Source code is included to satisfy GNU GPL V3 requirements.
