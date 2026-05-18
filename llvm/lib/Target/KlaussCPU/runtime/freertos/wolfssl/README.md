# wolfSSL + wolfSSH for KlaussCPU / FreeRTOS

This directory holds the build scripts and configuration for cross-compiling
wolfSSL 5.7.x and wolfSSH 1.4.x for the KlaussCPU bare-metal / FreeRTOS target.

The cloned source trees, build directories, and install trees are **not** committed
to git (see `.gitignore`). Run the scripts below once after cloning the repo.

---

## Prerequisites

- KlaussCPU LLVM/Clang toolchain built in `llvm-project/build/`
- picolibc built for KlaussCPU (run `../../../build-picolibc.sh` first)
- CMake ≥ 3.16, autoconf, automake, libtool (for wolfSSH configure)

---

## Build wolfSSL

```bash
cd freertos/wolfssl
bash build-wolfssl.sh
```

This will:
1. Clone wolfSSL 5.7.x into `wolfssl-src/`
2. Configure via CMake using `wolfssl-toolchain.cmake` and `user_settings.h`
3. Build and install into `wolfssl-install/` (headers + `libwolfssl.a`)

---

## Build wolfSSH

```bash
cd freertos/wolfssl
bash build-wolfssh.sh
```

This will:
1. Clone wolfSSH 1.4.x into `wolfssh-src/`
2. Configure via autoconf (`--host=arm-none-eabi` as alias for klausscpu-unknown-elf)
3. Build and install into `wolfssh-install/` (headers + `libwolfssh.a`)

**Note:** wolfSSH must be built *after* wolfSSL since it links against `libwolfssl.a`.

---

## Configuration

| File | Purpose |
|---|---|
| `user_settings.h` | wolfSSL compile-time feature flags (no filesystem, no sockets, HW crypto callbacks, SP_WORD_SIZE 32, etc.) |
| `wolfssl-toolchain.cmake` | CMake cross-compile toolchain — points at KlaussCPU clang/lld |
| `build-wolfssl.sh` | Clone + CMake build script for wolfSSL |
| `build-wolfssh.sh` | Clone + autoconf build script for wolfSSH |

---

## Key build options

- `WOLFSSL_USER_SETTINGS=yes` — uses `user_settings.h` instead of autoconf feature detection
- `SP_WORD_SIZE 32` — forces 32-bit bignum digits; avoids i128 division (now fixed in backend, can be changed to 64 for performance)
- `WOLF_CRYPTO_CB` — enables hardware crypto callbacks wired in `../wolfssl_hw.c`
- `NO_FILESYSTEM`, `WOLFSSL_NO_SOCK`, `WOLFSSL_USER_IO` — bare-metal / FreeRTOS mode
- `--host=arm-none-eabi` — autoconf alias; actual compiler is `klausscpu-unknown-elf-clang`
