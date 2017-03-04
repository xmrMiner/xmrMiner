## Requirements

### Hardware

xmrMiner supports all NVIDIA GPUs with a compute capability >=2.0
You can check the compute capability on [this](https://developer.nvidia.com/cuda-gpus) side.

### Software
- NVIDIA [CUDA](https://developer.nvidia.com/cuda-downloads) >=6.0
  - *Debian/Ubuntu:* `sudo apt-get install nvidia-cuda-dev nvidia-cuda-toolkit`
- host compiler
  - clang >=3.9 (support compile of the host and device code)
  - gcc >=4.6 (depends on your current CUDA version)
    - by default CUDA8 is not supporting gcc 6.x, never the less to compile with the
      newest gcc version you need follow this steps
    - open the file `$CUDAINSTALLDIR/include/host_config.h` and change(remove) the following line
    ```C++
    #if __GNUC__ > 5
    #error -- unsupported GNU version! gcc versions later than 5 are not supported!
    #endif /* __GNUC__ > 5 */
    ```
    to
    ```C++
    #if __GNUC__ > 5

    #endif /* __GNUC__ > 5 */
    ```

- SSL support
  - *Debian/Ubuntu:* `sudo apt-get install libssl-dev`
- CMake >=3.3.0
  - *Debian/Ubuntu:* `sudo apt-get install cmake cmake-curses-gui`
- Curl
  - *Debian/Ubuntu:* `sudo apt-get install libcurl4-gnutls-dev`
- git
  - *Debian/Ubuntu:* `sudo apt-get install git`
- Jansson
  - *Debian/Ubuntu:* `sudo apt-get install libjansson-dev`
  - From Source:
    - `git clone https://github.com/akheron/jansson.git`
    - `cd jansson`
    - `mkdir build`
    - `cd build`
    - `cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/usr/jansson`
    - `make install`
    - set the path to jansson before you compile *xmrMiner*
      `export CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH:$HOME/usr/jansson`

## Install

If you have compiled a dependency by hand please add the path to the install folder to `CMAKE_PREFIX_PATH` e.g.,
`export CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH:$JANSSON_ROOT`

1. create and enter the project folder:
  - `mkdir -p xmrMinerProject`
  - `cd xmrMinerProject`
2. download the `xmrMiner` source code
  - `git clone ....`
3. create a temporary build folder and enter
  - `mkdir -p build`
  - `cd build`
4. configure `xmrMiner` (search for all dependencies) and add the install path (creates a folder `xmrMiner` within the home)
  - `cmake -DCMAKE_INSTALL_PREFIX=$HOME/xmrMiner ../xmrMiner`
  - **optional** you can change all compile time options with a ncurses gui
    - `ccmake .` inside the build folder
    - after a option in ccmake is changed you need to end ccmake with the key `c` and than `g`
5. compile
  - `make -j install`
  - **optional** to speedup the compile you can change the CMake option `CUDA_ARCH` to the [compute capability]((https://developer.nvidia.com/cuda-gpus)) of your NVIDIA GPU
    - `ccmake` or `cmake -DCUDA_ARCH=61`+ options from step `4` (for Pascal)
6. start xmrMiner
  - `cd $HOME/xmrMiner`
  - `./xmrMiner --help`
