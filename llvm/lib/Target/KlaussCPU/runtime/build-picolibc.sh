#!/bin/bash
# build-picolibc.sh — clone, configure, build and install picolibc for KlaussCPU.
#
# Run this once from the runtime/ directory.  After it completes, set
# PICOLIBC in the Makefile to $INSTALL_DIR.
#
# Requirements: meson, ninja, git, clang (our build/bin/clang), llvm-ar.

set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="$(git -C ../../../.. rev-parse --show-toplevel)/build"
LLVM_BIN="$BUILD_DIR/bin"

# Add our build/bin to PATH so meson finds clang, llvm-ar, etc.
export PATH="$LLVM_BIN:$PATH"

PICOLIBC_SRC="$(pwd)/picolibc-src"
PICOLIBC_BUILD="$(pwd)/picolibc-build"
PICOLIBC_INSTALL="$(pwd)/picolibc-install"

# ── 1. Clone picolibc ─────────────────────────────────────────────────────────
if [ ! -d "$PICOLIBC_SRC" ]; then
    echo "==> Cloning picolibc..."
    git clone --depth 1 https://github.com/picolibc/picolibc.git "$PICOLIBC_SRC"
else
    echo "==> picolibc source already present, skipping clone"
fi

# ── 2. Generate cross file with absolute tool paths ───────────────────────────
# Meson looks up tools by name in PATH. Using full paths avoids PATH confusion.
echo "==> Writing picolibc-cross-abs.ini with absolute tool paths..."
cat > "$(pwd)/picolibc-cross-abs.ini" <<CROSSEOF
[binaries]
c       = '$LLVM_BIN/clang'
ar      = '$LLVM_BIN/llvm-ar'
strip   = '$LLVM_BIN/llvm-strip'
nm      = '$LLVM_BIN/llvm-nm'
objcopy = '$LLVM_BIN/llvm-objcopy'

[built-in options]
c_args      = ['-target', 'klausscpu-unknown-elf', '-O2', '-fPIC', '-ffreestanding', '-fno-common', '-ffunction-sections', '-fdata-sections', '-D__IEEE_LITTLE_ENDIAN']
c_link_args = ['-target', 'klausscpu-unknown-elf', '-nostdlib']

[host_machine]
system     = 'none'
cpu_family = 'none'
cpu        = 'klausscpu'
endian     = 'little'
CROSSEOF

# ── 3. Add KlaussCPU machine directory to picolibc ───────────────────────────
# picolibc requires libc/machine/<cpu_family>/meson.build.
# We use cpu_family='none' in the cross file; create a minimal machine dir.
MACHINE_DIR="$PICOLIBC_SRC/libc/machine/none"
if [ ! -d "$MACHINE_DIR" ]; then
    echo "==> Adding picolibc machine/none (KlaussCPU generic port)..."
    mkdir -p "$MACHINE_DIR"
    # Empty machine: use all-C generic implementations from picolibc.
    # setjmp/longjmp are provided by our own setjmp.S linked alongside.
    cat > "$MACHINE_DIR/meson.build" <<'MBEOF'
# KlaussCPU machine port — all-C generic implementations.
# Architecture-specific code (setjmp/longjmp) is in the application's
# setjmp.S, not compiled into picolibc itself.
src_machine = files()
MBEOF
    # Declare little-endian for picolibc's ieeefp.h, which requires this.
    # Also define _LDBL_EQ_DBL: on KlaussCPU long double == double (both 64-bit).
    cat > "$MACHINE_DIR/ieeefp.h" <<'IEOF'
/* KlaussCPU: little-endian IEEE 754 */
#ifndef __IEEE_LITTLE_ENDIAN
#define __IEEE_LITTLE_ENDIAN
#endif

/* long double == double on KlaussCPU; avoids conflicting declarations in
 * math_config.h when __SIZEOF_LONG_DOUBLE__ == 8 and _NEED_FLOAT64 is set. */
#ifndef _LDBL_EQ_DBL
#define _LDBL_EQ_DBL
#endif
IEOF
    # machine/ieeefp.h + machine/setjmp.h — placed in machine/none/machine/ so
    # they shadow libc/include/machine/ when picolibc searches include paths.
    mkdir -p "$MACHINE_DIR/machine"

    # machine/ieeefp.h: little-endian + _LDBL_EQ_DBL (long double == double).
    # Without _LDBL_EQ_DBL, math_config.h creates conflicting declarations for
    # _cosl/_sinl/_powl when __SIZEOF_LONG_DOUBLE__==8 && _NEED_FLOAT64.
    cat > "$MACHINE_DIR/machine/ieeefp.h" <<'IEOF2'
#ifndef _MACHINE_IEEEFP_H
#define _MACHINE_IEEEFP_H
#ifndef __IEEE_LITTLE_ENDIAN
#define __IEEE_LITTLE_ENDIAN
#endif
/* long double == double on KlaussCPU */
#ifndef _LDBL_EQ_DBL
#define _LDBL_EQ_DBL
#endif
#endif /* _MACHINE_IEEEFP_H */
IEOF2

    # machine/setjmp.h — defines jmp_buf for KlaussCPU.
    # Placed in machine/none/machine/ so it is found before the generic
    # libc/include/machine/setjmp.h (which has no __klausscpu__ block).
    cat > "$MACHINE_DIR/machine/setjmp.h" <<'SJEOF'
/*
 * KlaussCPU machine/setjmp.h
 *
 * jmp_buf layout matches setjmp.S (7 × 8-byte slots):
 *   [0] R4  [1] R5  [2] R6  [3] R7   (callee-saved GPRs)
 *   [4] caller's R15 (frame pointer)
 *   [5] caller's SP
 *   [6] return address
 */
#ifndef _MACHINE_SETJMP_H
#define _MACHINE_SETJMP_H

#define _JBLEN  7
#define _JBTYPE long   /* 64-bit on KlaussCPU */

typedef _JBTYPE jmp_buf[_JBLEN];

#endif /* _MACHINE_SETJMP_H */
SJEOF
fi

# ── 3. Configure with meson ───────────────────────────────────────────────────
echo "==> Configuring picolibc..."
rm -rf "$PICOLIBC_BUILD"
meson setup "$PICOLIBC_BUILD" "$PICOLIBC_SRC" \
    --cross-file "$(pwd)/picolibc-cross-abs.ini" \
    --prefix "$PICOLIBC_INSTALL" \
    -Dmultilib=false \
    -Dpicocrt=false \
    -Dtests=false \
    -Dspecsdir=none \
    -Dsingle-thread=true \
    -Dthread-local-storage=false \
    -Dfreestanding=true

# ── 4. Build and install ──────────────────────────────────────────────────────
echo "==> Building picolibc..."
ninja -C "$PICOLIBC_BUILD"
echo "==> Installing to $PICOLIBC_INSTALL..."
ninja -C "$PICOLIBC_BUILD" install

echo ""
echo "==> Done.  Set in Makefile:"
echo "    PICOLIBC = $PICOLIBC_INSTALL"
