#!/usr/bin/env python3
"""Check Rust's C primitive types (core::ffi, used by libc) for klausscpu
against clang's C types for the target: size, alignment and signedness of
char, short, int, long, long long, float, double, pointers, and the
alignment of 64-bit integers and doubles.

C values come from a C file compiled to assembly; the Rust side is a no_std
crate of const assertions built with the stage1 rustc for the JSON target
(through ../crate/rustc-expand-wrapper.sh, which supplies core). A failed
assertion is a compile error naming the type.

Usage: prims.py workdir clang sysroot wrapper
"""
import re
import subprocess
import sys
from pathlib import Path

work, clang, sysroot, wrapper = sys.argv[1:5]
work = Path(work)
work.mkdir(parents=True, exist_ok=True)

# (rust type, C type, signed-check?)
TYPES = [
    ("c_char", "char", True), ("c_schar", "signed char", True),
    ("c_uchar", "unsigned char", True), ("c_short", "short", True),
    ("c_ushort", "unsigned short", True), ("c_int", "int", True),
    ("c_uint", "unsigned int", True), ("c_long", "long", True),
    ("c_ulong", "unsigned long", True), ("c_longlong", "long long", True),
    ("c_ulonglong", "unsigned long long", True), ("c_float", "float", False),
    ("c_double", "double", False), ("*const c_void", "void *", False),
    ("usize", "size_t", True), ("isize", "ptrdiff_t", True),
    ("u64", "uint64_t", True), ("i64", "int64_t", True), ("f64", "double", False),
    ("u16", "uint16_t", True), ("u32", "uint32_t", True), ("u8", "uint8_t", True),
]
c = ["#include <stddef.h>", "#include <stdint.h>"]
for i, (_, ct, sg) in enumerate(TYPES):
    signed = "((%s)-1 < (%s)0)" % (ct, ct) if sg else "0"
    c.append("const unsigned long ks_prim_%d[3] = { sizeof(%s), _Alignof(%s), %s };"
             % (i, ct, ct, signed))
(work / "prims.c").write_text("\n".join(c) + "\n")
r = subprocess.run([clang, "--target=klausscpu-unknown-linux-musl", "--sysroot=" + sysroot,
                    "-S", "-O1", "-o", str(work / "prims.s"), str(work / "prims.c")],
                   capture_output=True, text=True)
if r.returncode:
    sys.exit(r.stderr)
vals, cur = {}, None
for line in (work / "prims.s").read_text().splitlines():
    m = re.match(r"^ks_prim_(\d+):", line)
    if m:
        cur = int(m.group(1))
        vals[cur] = []
        continue
    m = re.match(r"^\s*\.(?:quad|8byte|xword)\s+(\d+)", line)
    if cur is not None and m:
        vals[cur].append(int(m.group(1)))

rs = ["#![no_std]", "#![allow(non_camel_case_types)]",
      "use core::ffi::*;", "use core::mem::{size_of, align_of};"]
for i, (rt, ct, sg) in enumerate(TYPES):
    size, align, signed = vals[i]
    rs.append('const _: () = assert!(size_of::<%s>() == %d, "size of %s (C %s)");'
              % (rt, size, rt, ct))
    rs.append('const _: () = assert!(align_of::<%s>() == %d, "align of %s (C %s)");'
              % (rt, align, rt, ct))
    if sg:
        rs.append('const _: () = assert!(((0 as %s).wrapping_sub(1) < (0 as %s)) == %s, '
                  '"signedness of %s (C %s)");' % (rt, rt, "true" if signed else "false", rt, ct))
(work / "prims.rs").write_text("\n".join(rs) + "\n")
r = subprocess.run([wrapper, "--crate-type", "lib", "--edition", "2021", "--emit", "metadata",
                    "--target", "klausscpu-unknown-linux-musl", "-o", str(work / "prims.rmeta"),
                    str(work / "prims.rs")], capture_output=True, text=True,
                   env=dict(__import__("os").environ, RUSTC_BOOTSTRAP="1"))
if r.returncode:
    print(r.stderr)
    print("prims: FAILED (%d types)" % len(TYPES))
    sys.exit(1)
print("prims: %d types, size/align/signedness match C" % len(TYPES))
