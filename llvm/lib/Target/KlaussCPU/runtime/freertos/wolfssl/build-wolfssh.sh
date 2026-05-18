#!/usr/bin/env bash
# build-wolfssh.sh — Clone and build wolfSSH for KlaussCPU FreeRTOS.
#
# Run from runtime/freertos/wolfssl/ (or any directory — uses absolute paths).
# Run AFTER build-wolfssl.sh completes.
#
# Produces: wolfssh-install/lib/libwolfssh.a
#           wolfssh-install/include/wolfssh/...

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

FREERTOS_DIR="$(realpath "$SCRIPT_DIR/..")"
RUNTIME_DIR="$(realpath "$SCRIPT_DIR/../..")"
REPO_ROOT="$(realpath "$SCRIPT_DIR/../../../../../../..")"

LLVM_BUILD="$REPO_ROOT/build"
PICOLIBC="$RUNTIME_DIR/picolibc-install"
WOLFSSL_INSTALL="$SCRIPT_DIR/wolfssl-install"
INSTALL_DIR="$SCRIPT_DIR/wolfssh-install"
WOLFSSH_SRC="$SCRIPT_DIR/wolfssh-src"
BUILD_DIR="$SCRIPT_DIR/wolfssh-build"

WOLFSSH_TAG="v1.4.17-stable"

if [ ! -f "$WOLFSSL_INSTALL/lib/libwolfssl.a" ]; then
    echo "ERROR: wolfssl-install/lib/libwolfssl.a not found."
    echo "Run build-wolfssl.sh first."
    exit 1
fi
if [ ! -f "$LLVM_BUILD/bin/clang" ]; then
    echo "ERROR: clang not found at $LLVM_BUILD/bin/clang"
    exit 1
fi

echo "==> wolfSSH build for KlaussCPU"
echo "    LLVM build: $LLVM_BUILD"
echo "    wolfSSL:    $WOLFSSL_INSTALL"
echo "    install:    $INSTALL_DIR"

if [ ! -d "$WOLFSSH_SRC" ]; then
    echo "==> Cloning wolfSSH $WOLFSSH_TAG..."
    git clone --depth 1 --branch "$WOLFSSH_TAG" \
        https://github.com/wolfSSL/wolfssh.git "$WOLFSSH_SRC"
fi

TRIPLE="klausscpu-unknown-elf"

# Compiler flags — must match the wolfSSL build exactly.
CF="-target $TRIPLE -O1 -ffreestanding -ffunction-sections -fdata-sections -fPIC"
CF="$CF -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL"
CF="$CF -isystem $PICOLIBC/include"
CF="$CF -DWOLFSSL_USER_SETTINGS"
CF="$CF -I$SCRIPT_DIR"                          # user_settings.h
CF="$CF -I$FREERTOS_DIR"                        # FreeRTOS headers
CF="$CF -I$FREERTOS_DIR/FreeRTOS-Kernel/include"
CF="$CF -I$FREERTOS_DIR/portable/KlaussCPU"
CF="$CF -I$RUNTIME_DIR"                         # mmio.h (needed by portmacro.h)
CF="$CF -I$WOLFSSL_INSTALL/include"
CF="$CF -Wno-error -Wno-int-conversion -Wno-implicit-int-conversion"
CF="$CF -DWOLFSSH_NO_SFTP -DWOLFSSH_NO_SCP"
CF="$CF -DWOLFSSH_USER_IO"          # skip default socket I/O (no sys/socket.h)
CF="$CF -Wno-undef"                  # suppress ESP_IDF_VERSION_MAJOR warning

mkdir -p "$BUILD_DIR" "$INSTALL_DIR"

cd "$WOLFSSH_SRC"
if [ ! -f configure ]; then
    ./autogen.sh
fi

cd "$BUILD_DIR"

# config.sub only recognises standard arch names; arm-none-eabi is accepted.
# The actual compiler and flags target KlaussCPU.
HOST_ALIAS="arm-none-eabi"

# AC_CHECK_LIB tries to link a native test binary against our cross-compiled
# libwolfssl — that always fails.  Pre-supply the cache variables so autoconf
# skips the link test and trusts us that the library is present.
export ac_cv_header_wolfssl_ssl_h=yes
export ac_cv_lib_wolfssl_wolfCrypt_Init=yes

"$WOLFSSH_SRC/configure" \
    CC="$LLVM_BUILD/bin/clang" \
    CFLAGS="$CF" \
    LDFLAGS="-L$WOLFSSL_INSTALL/lib" \
    LIBS="-lwolfssl" \
    AR="$LLVM_BUILD/bin/llvm-ar" \
    RANLIB="$LLVM_BUILD/bin/llvm-ranlib" \
    --host="$HOST_ALIAS" \
    --prefix="$INSTALL_DIR" \
    --with-wolfssl="$WOLFSSL_INSTALL" \
    --disable-shared \
    --enable-static \
    --disable-examples \
    --disable-sftp \
    --disable-scp \
    --disable-term

# Build with -k so that test/example failures don't abort the library build.
# src/ is the first SUBDIRS entry so libwolfssh.a is always built before any
# test/example failure.
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
make -j"$NCPU" -k || true

# Verify the library was actually produced.
if [ ! -f "src/.libs/libwolfssh.a" ]; then
    echo "ERROR: src/.libs/libwolfssh.a not found — library build failed."
    exit 1
fi

# Install manually (avoids 'make install' which also installs tests/examples).
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include/wolfssh"
cp src/.libs/libwolfssh.a "$INSTALL_DIR/lib/"
cp "$WOLFSSH_SRC/wolfssh/"*.h "$INSTALL_DIR/include/wolfssh/"

echo ""
echo "==> wolfSSH built successfully"
echo "    Library:  $INSTALL_DIR/lib/libwolfssh.a"
echo "    Headers:  $INSTALL_DIR/include/wolfssh/"
