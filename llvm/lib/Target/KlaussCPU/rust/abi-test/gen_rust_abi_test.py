#!/usr/bin/env python3
"""Generate a Rust <-> C ABI conformance test.

Writes, for one seed, a C and a Rust version of each side:

  support.c   main(), report(), the shared globals (always C)
  caller.c    run_tests(): calls every test function, reports a hash per call
  callee.c    the test functions
  caller.rs   run_tests() in Rust (a no_std staticlib)
  callee.rs   the test functions in Rust (a no_std staticlib)

The functions take and return random mixes of signed and unsigned 8-64-bit
integers, bool, pointers, f32/f64 (soft float), i128, and #[repr(C)] structs
and unions of 1 to 32 bytes (fields of every integer width up to 128 bits,
bool, f32/f64), some variadic.  The two versions of each side
compute the same thing: host gcc runs support.c + caller.c + callee.c for the
expected output, and the target build pairs a C caller with a Rust callee, a
Rust caller with a C callee, and Rust with Rust (all calls are extern "C").

As in klausscpu-runtime's abi-test/gen_abi_test.py (the C-only test): every
argument comes from volatile data, so a value reaches the call with whatever
upper bits its producer left; a callee folds every argument into a hash with
operations that depend on the whole register (widening, signed compares,
shifts), overwrites each by-value record parameter (it must be its own copy),
and returns a value derived from the hash; the caller folds the result and
then its own by-value records.  Records are compared field by field (struct
padding has no defined value in Rust), unions through a byte-array member.

Limits of the Rust side: unnamed variadic arguments are int, unsigned, long,
unsigned long, double or a pointer (core::ffi::VaArgSafe), and a variadic
function has at least one named parameter.

Usage: gen_rust_abi_test.py SEED OUTDIR [NFUNCS]
"""
import os
import random
import sys
import zlib

INT_TYPES = [
    ('signed char', 1, True), ('unsigned char', 1, False),
    ('short', 2, True), ('unsigned short', 2, False),
    ('int', 4, True), ('unsigned', 4, False),
    ('long', 8, True), ('unsigned long', 8, False),
    ('_Bool', 1, False),
]
VA_INT_TYPES = [('int', 4, True), ('unsigned', 4, False),
                ('long', 8, True), ('unsigned long', 8, False)]
# A 128-bit field makes the record 16-byte aligned (an i128 in registers when
# it is 16 bytes, by reference beyond); _Bool fields hold 0 or 1.
FIELD_TYPES = ['signed char', 'unsigned char', 'short', 'unsigned short',
               'int', 'unsigned', 'long', 'unsigned long', 'float', 'double',
               '_Bool', '__int128', 'unsigned __int128']
SIZES = {'signed char': 1, 'unsigned char': 1, 'short': 2,
         'unsigned short': 2, 'int': 4, 'unsigned': 4, 'long': 8,
         'unsigned long': 8, 'float': 4, 'double': 8, '_Bool': 1,
         '__int128': 16, 'unsigned __int128': 16}
RUST = {'signed char': 'i8', 'unsigned char': 'u8', 'short': 'i16',
        'unsigned short': 'u16', 'int': 'i32', 'unsigned': 'u32',
        'long': 'i64', 'unsigned long': 'u64', 'float': 'f32',
        'double': 'f64', '_Bool': 'bool', '__int128': 'i128',
        'unsigned __int128': 'u128'}
SIGNED = {'signed char', 'short', 'int', 'long'}
WIDE = {'__int128', 'unsigned __int128'}


def layout(fields):
    off, align = 0, 1
    for t, n in fields:
        s = SIZES[t]
        off = (off + s - 1) // s * s + s * n
        align = max(align, s)
    return (off + align - 1) // align * align


def union_size(fields):
    size = max(SIZES[t] * n for t, n in fields)
    align = max(SIZES[t] for t, _ in fields)
    return (size + align - 1) // align * align


C_PRELUDE = r'''#include <stdarg.h>
#include <string.h>
#include <stdint.h>
typedef unsigned long u64;
static inline u64 mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15UL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdUL; }
static inline u64 mixs(u64 h, long v) { return mix(mix(h, (u64)v), v < 0 ? 0x55 : 0xaa); }
static inline u64 mixbytes(u64 h, const void *p, unsigned n) { const unsigned char *b = p; for (unsigned i = 0; i < n; i++) h = mix(h, b[i]); return h; }
extern const char g_base[64];
extern volatile u64 g_sink;
u64 next(void);
void report(int i, u64 h);
'''

RUST_PRELUDE = r'''#![no_std]
#![allow(non_snake_case, non_upper_case_globals, unused_mut, unused_variables,
         unused_assignments, unused_unsafe, improper_ctypes,
         improper_ctypes_definitions, clippy::all)]
#![feature(c_variadic, c_variadic_experimental_arch)]
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};

extern "C" {
    static g_base: [u8; 64];
    static mut g_sink: u64;
    fn next() -> u64;
    fn report(i: i32, h: u64);
}

#[inline(always)]
fn mix(mut h: u64, v: u64) -> u64 {
    h ^= v.wrapping_add(0x9e3779b97f4a7c15).wrapping_add(h << 6).wrapping_add(h >> 2);
    h.wrapping_mul(0xff51afd7ed558ccd)
}
#[inline(always)]
fn mixs(h: u64, v: i64) -> u64 { mix(mix(h, v as u64), if v < 0 { 0x55 } else { 0xaa }) }
#[inline(always)]
fn mixbytes(mut h: u64, b: &[u8]) -> u64 {
    for &x in b { h = mix(h, x as u64); }
    h
}
#[inline(always)]
fn base() -> usize { unsafe { addr_of!(g_base) as usize } }
#[inline(always)]
fn ptr_at(off: u64) -> *const u8 { (base() + off as usize) as *const u8 }
#[inline(always)]
fn sink_get() -> u64 { unsafe { read_volatile(addr_of!(g_sink)) } }
#[inline(always)]
fn sink_set(v: u64) { unsafe { write_volatile(addr_of_mut!(g_sink), v) } }
'''


class Gen:
    def __init__(self, seed, nfuncs):
        self.r = random.Random(seed)
        self.seed = seed
        self.nfuncs = nfuncs
        self.records = {}   # name -> (kind, fields)
        self.funcs = []

    # -- types -------------------------------------------------------------

    def make_record(self):
        r = self.r
        for _ in range(100):
            fields = []
            for _ in range(r.randint(1, 5)):
                t = r.choice(FIELD_TYPES)
                fields.append((t, 1 if r.random() < 0.8 else r.randint(2, 5)))
            if 1 <= layout(fields) <= 32:
                break
        kind = 'union' if r.random() < 0.12 else 'struct'
        name = 'R%d' % len(self.records)
        self.records[name] = (kind, fields)
        return name

    def rand_type(self):
        r = self.r
        x = r.random()
        if x < 0.45:
            return ('int',) + r.choice(INT_TYPES)
        if x < 0.55:
            return ('ptr',)
        if x < 0.62:
            return ('fp', r.choice(['float', 'double']))
        if x < 0.67:
            return ('i128',)
        if self.records and r.random() < 0.5:
            return ('rec', r.choice(sorted(self.records)))
        return ('rec', self.make_record())

    def va_type(self):
        x = self.r.random()
        if x < 0.7:
            return ('int',) + self.r.choice(VA_INT_TYPES)
        if x < 0.85:
            return ('ptr',)
        return ('fp', 'double')

    def gen(self):
        for i in range(self.nfuncs):
            r = self.r
            variadic = r.random() < 0.25
            nparams = r.randint(1 if variadic else 0, 9)
            params = [self.rand_type() for _ in range(nparams)]
            vtypes = [self.va_type() for _ in range(r.randint(0, 7))] if variadic else []
            ret = self.rand_type() if r.random() < 0.85 else ('void',)
            self.funcs.append(('f%d' % i, ret, params, variadic, vtypes))

    def size(self, name):
        kind, fields = self.records[name]
        return union_size(fields) if kind == 'union' else layout(fields)

    def cty(self, t):
        return {'int': lambda: t[1], 'ptr': lambda: 'const char *',
                'fp': lambda: t[1], 'i128': lambda: '__int128',
                'rec': lambda: '%s %s' % (self.records[t[1]][0], t[1]),
                'void': lambda: 'void'}[t[0]]()

    def rty(self, t):
        return {'int': lambda: RUST[t[1]], 'ptr': lambda: '*const u8',
                'fp': lambda: RUST[t[1]], 'i128': lambda: 'i128',
                'rec': lambda: t[1]}[t[0]]()

    # -- C -----------------------------------------------------------------

    def c_decls(self):
        out = [C_PRELUDE]
        for name, (kind, fields) in sorted(self.records.items()):
            fs = ' '.join('%s m%d%s;' % (t, j, '' if n == 1 else '[%d]' % n)
                          for j, (t, n) in enumerate(fields))
            if kind == 'union':
                fs += ' unsigned char b[%d];' % self.size(name)
            out.append('%s %s { %s };' % (kind, name, fs))
        for f in self.funcs:
            out.append(self.c_proto(*f[:4]) + ';')
        return out

    def c_proto(self, name, ret, params, variadic):
        ps = ['%s a%d' % (self.cty(t), j) for j, t in enumerate(params)]
        if variadic:
            ps.append('...')
        return '%s %s(%s)' % (self.cty(ret), name, ', '.join(ps) or 'void')

    def c_fold(self, expr, t):
        if t[0] == 'int':
            if t[3]:
                return ['h = mixs(h, (long)%s);' % expr]
            return ['{ %s v_ = %s; h = mix(h, (u64)v_); h = mix(h, (u64)(v_ >> 1)); }'
                    % (t[1], expr)]
        if t[0] == 'ptr':
            return ['h = mix(h, (u64)(%s - g_base));' % expr]
        if t[0] == 'fp':
            return ['{ %s v_ = %s; h = mixbytes(h, &v_, sizeof v_); }' % (t[1], expr)]
        if t[0] == 'i128':
            return ['{ __int128 v_ = %s; h = mix(h, (u64)v_); h = mix(h, (u64)(v_ >> 64)); h = mix(h, v_ < 0); }'
                    % expr]
        kind, fields = self.records[t[1]]
        out = ['{ %s r_ = %s;' % (self.cty(t), expr)]
        if kind == 'union':
            out.append('  h = mixbytes(h, r_.b, sizeof r_.b); }')
            return out
        for j, (ft, n) in enumerate(fields):
            for k in range(n):
                fe = 'r_.m%d%s' % (j, '' if n == 1 else '[%d]' % k)
                if ft in ('float', 'double'):
                    out.append('  { %s v_ = %s; h = mixbytes(h, &v_, sizeof v_); }' % (ft, fe))
                elif ft in WIDE:
                    out.append('  h = mix(h, (u64)%s); h = mix(h, (u64)(%s >> 64));' % (fe, fe))
                elif ft in SIGNED:
                    out.append('  h = mixs(h, (long)%s);' % fe)
                else:
                    out.append('  h = mix(h, (u64)%s);' % fe)
        out.append('}')
        return out

    def c_value(self, t, s):
        """An expression of scalar type t from the u64 variable s."""
        if t[0] == 'int':
            if t[1] == '_Bool':
                return '(_Bool)(%s & 4)' % s
            return '(%s)%s' % (t[1], s)
        if t[0] == 'ptr':
            return '(g_base + (%s & 63))' % s
        if t[0] == 'fp':
            return '(%s)(long)(int)%s' % (t[1], s)
        return '(((__int128)(long)%s << 64) | (__int128)(u64)(%s * 3))' % (s, s)

    def c_fill(self, var, t, src):
        """Statements filling record var; src is evaluated once per field
        (it may be next())."""
        kind, fields = self.records[t[1]]
        out = ['memset(&%s, 0, sizeof %s);' % (var, var)]
        if kind == 'union':
            out.append('{ u64 s_ = %s; for (unsigned i_ = 0; i_ < sizeof %s.b; i_++) { %s.b[i_] = (unsigned char)(s_ >> (8 * (i_ & 7))); if ((i_ & 7) == 7) s_ = mix(s_, i_); } }'
                       % (src, var, var))
            return out
        idx = 0
        for j, (ft, n) in enumerate(fields):
            for k in range(n):
                fe = '%s.m%d%s' % (var, j, '' if n == 1 else '[%d]' % k)
                v = '(%s) * %d + %d' % (src, 2 * idx + 3, idx)
                if ft in ('float', 'double'):
                    out.append('%s = (%s)(long)(int)(%s);' % (fe, ft, v))
                elif ft == '_Bool':
                    out.append('%s = ((%s) & 4) != 0;' % (fe, v))
                elif ft in WIDE:
                    out.append('{ u64 t_ = %s; %s = (%s)(((__int128)(long)t_ << 64) | (__int128)(t_ * 3)); }'
                               % (v, fe, ft))
                else:
                    out.append('%s = (%s)(%s);' % (fe, ft, v))
                idx += 1
        return out

    def c_callee(self):
        out = self.c_decls()
        for name, ret, params, variadic, vtypes in self.funcs:
            out.append(self.c_proto(name, ret, params, variadic) + ' {')
            out.append('  u64 h = 0x%x;' % zlib.crc32(name.encode()))
            for j, t in enumerate(params):
                out += ['  ' + s for s in self.c_fold('a%d' % j, t)]
                if t[0] == 'rec':
                    out.append('  memset(&a%d, 0x5a, sizeof a%d);' % (j, j))
            if variadic:
                out.append('  va_list ap;')
                out.append('  va_start(ap, a%d);' % (len(params) - 1))
                for t in vtypes:
                    out += ['  ' + s for s in self.c_fold('va_arg(ap, %s)' % self.cty(t), t)]
                out.append('  va_end(ap);')
            if ret[0] == 'void':
                out.append('  g_sink = h;')
            elif ret[0] == 'rec':
                out.append('  %s r;' % self.cty(ret))
                out += ['  ' + s for s in self.c_fill('r', ret, 'h')]
                out.append('  return r;')
            else:
                out.append('  return %s;' % self.c_value(ret, 'h'))
            out.append('}')
        return '\n'.join(out) + '\n'

    def c_caller(self):
        out = self.c_decls()
        out.append('void run_tests(void) {')
        for i, (name, ret, params, variadic, vtypes) in enumerate(self.funcs):
            out.append('  { /* %s */' % name)
            out.append('    u64 h = 0;')
            args, recs = [], []
            for j, t in enumerate(params + vtypes):
                v = 'v%d' % j
                if t[0] == 'rec':
                    out.append('    %s %s;' % (self.cty(t), v))
                    out += ['    ' + s for s in self.c_fill(v, t, 'next()')]
                    recs.append((v, t))
                else:
                    out.append('    u64 s%d = next();' % j)
                    out.append('    %s %s = %s;' % (self.cty(t), v, self.c_value(t, 's%d' % j)))
                args.append(v)
            call = '%s(%s)' % (name, ', '.join(args))
            if ret[0] == 'void':
                out.append('    %s;' % call)
                out.append('    h = mix(h, g_sink);')
            else:
                out += ['    ' + s for s in self.c_fold(call, ret)]
            for v, t in recs:
                out += ['    ' + s for s in self.c_fold(v, t)]
            out.append('    report(%d, h);' % i)
            out.append('  }')
        out.append('}')
        return '\n'.join(out) + '\n'

    def c_support(self):
        return r'''#include <stdio.h>
typedef unsigned long u64;
const char g_base[64] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";
volatile u64 g_sink;
volatile u64 g_seed = 0x%xUL;
u64 next(void) { u64 s = g_seed; s ^= s << 13; s ^= s >> 7; s ^= s << 17; g_seed = s; return s; }
void report(int i, u64 h) { printf("T%%d %%016lx\n", i, h); }
void run_tests(void);
int main(void) { run_tests(); printf("ABI-DONE\n"); return 0; }
''' % random.Random(self.seed * 7919 + 1).getrandbits(64)

    # -- Rust --------------------------------------------------------------

    def rs_decls(self):
        out = [RUST_PRELUDE]
        for name, (kind, fields) in sorted(self.records.items()):
            fs = ['m%d: %s' % (j, RUST[t] if n == 1 else '[%s; %d]' % (RUST[t], n))
                  for j, (t, n) in enumerate(fields)]
            if kind == 'union':
                fs.append('b: [u8; %d]' % self.size(name))
            out.append('#[repr(C)]\n#[derive(Clone, Copy)]\npub %s %s { %s }'
                       % (kind, name, ', '.join('pub ' + f for f in fs)))
        return out

    def rs_sig(self, name, ret, params, variadic, extern):
        ps = ['%sa%d: %s' % ('' if extern else 'mut ', j, self.rty(t))
              for j, t in enumerate(params)]
        if variadic:
            ps.append('...' if extern else 'mut ap: ...')
        rt = '' if ret[0] == 'void' else ' -> %s' % self.rty(ret)
        return 'fn %s(%s)%s' % (name, ', '.join(ps), rt)

    def rs_fold(self, expr, t):
        if t[0] == 'int':
            if t[3]:
                return ['h = mixs(h, (%s) as i64);' % expr]
            if t[1] == '_Bool':
                return ['{ let v_: bool = %s; h = mix(h, v_ as u64); h = mix(h, (v_ as u64) >> 1); }'
                        % expr]
            return ['{ let v_: %s = %s; h = mix(h, v_ as u64); h = mix(h, (v_ >> 1) as u64); }'
                    % (RUST[t[1]], expr)]
        if t[0] == 'ptr':
            return ['h = mix(h, ((%s) as usize).wrapping_sub(base()) as u64);' % expr]
        if t[0] == 'fp':
            return ['{ let v_: %s = %s; h = mixbytes(h, &v_.to_ne_bytes()); }' % (RUST[t[1]], expr)]
        if t[0] == 'i128':
            return ['{ let v_: i128 = %s; h = mix(h, v_ as u64); h = mix(h, (v_ >> 64) as u64); h = mix(h, (v_ < 0) as u64); }'
                    % expr]
        kind, fields = self.records[t[1]]
        out = ['{ let r_: %s = %s;' % (t[1], expr)]
        if kind == 'union':
            out.append('  h = mixbytes(h, unsafe { &r_.b }); }')
            return out
        for j, (ft, n) in enumerate(fields):
            for k in range(n):
                fe = 'r_.m%d%s' % (j, '' if n == 1 else '[%d]' % k)
                if ft in ('float', 'double'):
                    out.append('  h = mixbytes(h, &%s.to_ne_bytes());' % fe)
                elif ft in WIDE:
                    out.append('  h = mix(h, %s as u64); h = mix(h, (%s >> 64) as u64);' % (fe, fe))
                elif ft in SIGNED:
                    out.append('  h = mixs(h, %s as i64);' % fe)
                else:
                    out.append('  h = mix(h, %s as u64);' % fe)
        out.append('}')
        return out

    def rs_value(self, t, s):
        if t[0] == 'int':
            if t[1] == '_Bool':
                return '(%s & 4) != 0' % s
            return '%s as %s' % (s, RUST[t[1]])
        if t[0] == 'ptr':
            return 'ptr_at(%s & 63)' % s
        if t[0] == 'fp':
            return '(%s as i32 as i64) as %s' % (s, RUST[t[1]])
        return '((%s as i64 as i128) << 64) | (%s.wrapping_mul(3) as i128)' % (s, s)

    def rs_fill(self, var, t, src):
        """Statements filling the zeroed record var, src once per field."""
        kind, fields = self.records[t[1]]
        if kind == 'union':
            return ['{ let mut s_: u64 = %s; let mut b_ = [0u8; %d];' % (src, self.size(t[1])),
                    '  for i_ in 0..b_.len() { b_[i_] = (s_ >> (8 * (i_ & 7))) as u8; if (i_ & 7) == 7 { s_ = mix(s_, i_ as u64); } }',
                    '  %s.b = b_; }' % var]
        out = []
        idx = 0
        for j, (ft, n) in enumerate(fields):
            for k in range(n):
                fe = '%s.m%d%s' % (var, j, '' if n == 1 else '[%d]' % k)
                v = '(%s).wrapping_mul(%d).wrapping_add(%d)' % (src, 2 * idx + 3, idx)
                if ft in ('float', 'double'):
                    out.append('%s = (%s as i32 as i64) as %s;' % (fe, v, RUST[ft]))
                elif ft == '_Bool':
                    out.append('%s = (%s & 4) != 0;' % (fe, v))
                elif ft in WIDE:
                    out.append('{ let t_: u64 = %s; %s = (((t_ as i64 as i128) << 64) | (t_.wrapping_mul(3) as i128)) as %s; }'
                               % (v, fe, RUST[ft]))
                else:
                    out.append('%s = %s as %s;' % (fe, v, RUST[ft]))
                idx += 1
        return out

    def rs_callee(self):
        out = self.rs_decls()
        for name, ret, params, variadic, vtypes in self.funcs:
            out.append('#[no_mangle]')
            out.append('pub unsafe extern "C" %s {' % self.rs_sig(name, ret, params, variadic, False))
            out.append('  let mut h: u64 = 0x%x;' % zlib.crc32(name.encode()))
            for j, t in enumerate(params):
                out += ['  ' + s for s in self.rs_fold('a%d' % j, t)]
                if t[0] == 'rec':
                    # The parameter is this function's copy.
                    out.append('  core::ptr::write_bytes(addr_of_mut!(a%d) as *mut u8, 0x5a, core::mem::size_of::<%s>());'
                               % (j, t[1]))
            for t in vtypes:
                out += ['  ' + s for s in self.rs_fold('ap.next_arg::<%s>()' % self.rty(t), t)]
            if ret[0] == 'void':
                out.append('  sink_set(h);')
            elif ret[0] == 'rec':
                out.append('  let mut r: %s = core::mem::zeroed();' % ret[1])
                out += ['  ' + s for s in self.rs_fill('r', ret, 'h')]
                out.append('  r')
            else:
                out.append('  %s' % self.rs_value(ret, 'h'))
            out.append('}')
        return '\n'.join(out) + '\n'

    def rs_caller(self):
        out = self.rs_decls()
        out.append('extern "C" {')
        for name, ret, params, variadic, vtypes in self.funcs:
            out.append('  %s;' % self.rs_sig(name, ret, params, variadic, True))
        out.append('}')
        out.append('#[no_mangle]')
        out.append('pub unsafe extern "C" fn run_tests() {')
        for i, (name, ret, params, variadic, vtypes) in enumerate(self.funcs):
            out.append('  { // %s' % name)
            out.append('    let mut h: u64 = 0;')
            args, recs = [], []
            for j, t in enumerate(params + vtypes):
                v = 'v%d' % j
                if t[0] == 'rec':
                    out.append('    let mut %s: %s = core::mem::zeroed();' % (v, t[1]))
                    out += ['    ' + s for s in self.rs_fill(v, t, 'next()')]
                    recs.append((v, t))
                else:
                    out.append('    let s%d: u64 = next();' % j)
                    out.append('    let %s: %s = %s;' % (v, self.rty(t), self.rs_value(t, 's%d' % j)))
                args.append(v)
            call = '%s(%s)' % (name, ', '.join(args))
            if ret[0] == 'void':
                out.append('    %s;' % call)
                out.append('    h = mix(h, sink_get());')
            else:
                out += ['    ' + s for s in self.rs_fold(call, ret)]
            for v, t in recs:
                out += ['    ' + s for s in self.rs_fold(v, t)]
            out.append('    report(%d, h);' % i)
            out.append('  }')
        out.append('}')
        return '\n'.join(out) + '\n'


def main():
    seed = int(sys.argv[1])
    outdir = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    g = Gen(seed, n)
    g.gen()
    os.makedirs(outdir, exist_ok=True)
    for fname, text in [('support.c', g.c_support()), ('caller.c', g.c_caller()),
                        ('callee.c', g.c_callee()), ('caller.rs', g.rs_caller()),
                        ('callee.rs', g.rs_callee())]:
        with open(os.path.join(outdir, fname), 'w') as f:
            f.write(text)


if __name__ == '__main__':
    main()
