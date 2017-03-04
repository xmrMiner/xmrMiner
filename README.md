# xmrMiner - A CUDA based miner for Monero

This project is forked from [KlausT's](https://github.com/KlausT/ccminer-cryptonight) ccminer version.
ccminer is developed by Christian Buchner's &amp; Christian H.'s and modified by tsiv for Cryptonight mining.

## Performance Overview

gpu | launch param | xmrMiner [hash/s] | KlausT ccminer [hash/s] | speedup [%] | clock [MHz] | watt
:---|:------------:|:------------------|:------------------------|:------------|:------------|----
k80* | 24x39 | 482 | 395 | 22 | 875 | 115
K80* Amazon AWS | 24x39 | 469 | 399 | 25 | 875 | 128
k20 | 24x39 | 397 | 314 | 26 | 758 |  99
P100 | 72x56 | 1640 | 1630 | 0 | 1328 | 92
GTX TITAN X | 16x48 | 633 | 579 | 9 | 1151 | 132

* used one of two GPU sockets

## Bugs

If you find any bugs don't be afraid to open an issue.


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
- Jansson
  - *Debian/Ubuntu:* `sudo apt-get install libjansson-dev`
- git
  - *Debian/Ubuntu:* `sudo apt-get install git`

## Install

If you have compiled a dependency by hand please add the path to the install folder to `CMAKE_PREFIX_PATH` e.g.,
`export  CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH:$JANSSON_ROOT`

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

## Performance

The optimal parameter for `--launch=TxB` depend on your GPU.
For all GPU's with a compute capability `>=3.0` and `<6.0` there is a restriction of the amount of RAM that can be used for the mining algorithm.
The maximum RAM that can be used must be less than 2GB (e.g. GTX TITAN) or 1GB (e.g. GTX 750-TI).
The amount of RAM used for mining can be changed with `--launch=TxB`.
  - `T` = threads used
  - `B` = CUDA blocks started (must be a multiple of the multiprocessors `M` on the GPU)

For the 2GB limit the equations must be full filled: `T * B * 2 <= 2000` and ` B mod M == 0`.
The GTX Titan X has 24 multiprocessors `M`, this means a valid and good starting configuration is `--launch=16x48`
and full fill all restrictions `16 * 48 * 2 = 1536` and `48 mod 24 = 0`.

Pascal GPU's should have no memory limit if the newest CUDA driver is used.

## Donation

By default xmrMiner will donate 2% of the shares to my Monero address.
If you want to change that, use the runtime option `--donate` to de/increase the donation.
If you find this tool useful and like to support its continued development, then consider a donation.
Do not forget the original authors.

- **psychocrypt**'s XMR address:
`43NoJVEXo21hGZ6tDG6Z3g4qimiGdJPE6GRxAmiWwm26gwr62Lqo7zRiCJFSBmbkwTGNuuES9ES5TgaVHceuYc4Y75txCTU`

- **KlausT**'s BTC address: `1QHH2dibyYL5iyMDk3UN4PVvFVtrWD8QKp`
- **tsiv**'s XMR address:
`42uasNqYPnSaG3TwRtTeVbQ4aRY3n9jY6VXX3mfgerWt4ohDQLVaBPv3cYGKDXasTUVuLvhxetcuS16ynt85czQ48mbSrWX`
- **tsiv**'s BTC address: `1JHDKp59t1RhHFXsTw2UQpR3F9BBz3R3cs`
- **Christian Buchner** and **Christian H.** BTC adress: `16hJF5mceSojnTD3ZTUDqdRhDyPJzoRakM`
