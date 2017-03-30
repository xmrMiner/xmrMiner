# xmrMiner - A CUDA based miner for Monero

This project is forked from [KlausT's](https://github.com/KlausT/ccminer-cryptonight) ccminer version.
ccminer is developed by Christian Buchner's &amp; Christian H.'s and modified by tsiv for Cryptonight mining.

## Performance Overview

gpu | launch param | xmrMiner [hash/s] | KlausT ccminer [hash/s] | speedup [%] | clock [MHz] | watt | system
:---|:------------:|:------------------|:------------------------|:------------|:------------|:----|:-----
k80* | 24x39 | 482 | 395 | 22 | 875 | 115 | ubuntu 14.04
K80* Amazon AWS | 24x39 | 469 | 399 | 25 | 875 | 128 | ubuntu 14.04
k20 | 24x39 | 397 | 314 | 26 | 758 |  99 | ubuntu 14.04
P100 | 72x56 | 1640 | 1630** | 0 | 1328 | 92 | ubuntu 14.04
GTX TITAN X | 16x48 | 633 | 579 | 9 | 1151 | 132 | ubuntu 14.04 
GT 555M | 64x6 | 102 | 68 | 50 | default | x | windows 7

\* used one of two GPU sockets  

\** patched version to support memory >4GiB

## Bugs

If you find any bugs don't be afraid to open an issue.


## Requirements/ Install

- see [INSTALL.md](INSTALL.md)


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
