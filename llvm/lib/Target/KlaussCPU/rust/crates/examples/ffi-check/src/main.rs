//! Bare-metal self-check of Rust on KlaussCPU: atomics, and calls between
//! Rust and C with structs by value (C ABI version 2, `ABI.md`).
//!
//! * Rust calls the C functions in `ffi.c` and compares each result with a
//!   Rust twin (the `rs_*` functions, which C calls in turn: `c_calls_rust`).
//!   The mixes pass 3-, 8-, 12-, 16- and 24-byte structs, a 16-byte-aligned
//!   one and a union, partly on the stack, a sign-extended `u32`, and check
//!   that a struct passed by reference is the callee's own copy.
//! * Atomics of every width: loads and stores are plain accesses, every
//!   read-modify-write and compare-exchange calls a `__sync_*_N` function
//!   (klauss-rt's `sync` module); C's `__atomic_*` builtins on the same
//!   variables go through the same functions.
//!
//! Prints `PASS <check>` / `FAIL <check> ...` lines and `Result: PASS` or
//! `Result: FAIL`, then halts with the number of failed checks in R12.

#![no_std]
#![no_main]

use core::hint::black_box;
use core::sync::atomic::{
    AtomicBool, AtomicI16, AtomicI32, AtomicI64, AtomicI8, AtomicPtr, AtomicU16, AtomicU32,
    AtomicU64, AtomicU8, AtomicUsize, Ordering::SeqCst,
};
use klauss_io::println;

klauss_rt::entry!(main);

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Tri {
    x: u8,
    y: u8,
    z: u8,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Pair {
    a: i32,
    b: u32,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Mid {
    a: u64,
    b: u32,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Wide {
    v: i128,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Big {
    a: u64,
    b: u64,
    c: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union U {
    w: u32,
    b: [u8; 6],
}

#[allow(improper_ctypes)]
extern "C" {
    fn c_combine(p: Pair, t: Tri, u: u32, s: i8, b: Big, w: Wide, un: U) -> Mid;
    fn c_big(k: u64, m: Mid) -> Big;
    fn c_sext(x: u32) -> i64;
    fn c_u32_ret(x: u32) -> u32;
    fn c_tri(t: Tri) -> Tri;
    fn c_wide(w: Wide, u: u32) -> Wide;
    fn c_calls_rust() -> u32;

    fn c_fetch_add32(p: *mut u32, v: u32) -> u32;
    fn c_cas64(p: *mut u64, expected: *mut u64, desired: u64) -> bool;
    fn c_xchg8(p: *mut u8, v: u8) -> u8;
    fn c_fetch_max16(p: *mut i16, v: i16) -> i16;
    fn c_halt(rc: i64) -> !;
}

fn mix(mut h: u64, v: u64) -> u64 {
    h ^= v
        .wrapping_add(0x9e3779b97f4a7c15)
        .wrapping_add(h << 6)
        .wrapping_add(h >> 2);
    h.wrapping_mul(0xff51afd7ed558ccd)
}

fn hash_args(p: Pair, t: Tri, u: u32, s: i8, b: Big, w: Wide, un: U) -> u64 {
    let mut h = mix(0, p.a as i64 as u64);
    h = mix(h, p.b as u64);
    h = mix(h, t.x as u64);
    h = mix(h, t.y as u64);
    h = mix(h, t.z as u64);
    h = mix(h, u as u64);
    h = mix(h, u as i32 as i64 as u64);
    h = mix(h, s as i64 as u64);
    h = mix(h, b.a);
    h = mix(h, b.b);
    h = mix(h, b.c);
    h = mix(h, w.v as u64);
    h = mix(h, (w.v >> 64) as u64);
    unsafe {
        h = mix(h, un.w as u64);
        h = mix(h, un.b[4] as u64);
        h = mix(h, un.b[5] as u64);
    }
    h
}

// ---- the Rust twins, called from C ----

#[no_mangle]
#[allow(improper_ctypes_definitions)]
pub extern "C" fn rs_combine(p: Pair, t: Tri, u: u32, s: i8, mut b: Big, w: Wide, un: U) -> Mid {
    let h = hash_args(p, t, u, s, b, w, un);
    // Our own copy: the caller's must not change.
    b.a = 0xdeadbeef;
    b.c = 0;
    black_box(&mut b);
    Mid {
        a: h,
        b: (h as u32) ^ u,
    }
}

#[no_mangle]
pub extern "C" fn rs_big(k: u64, m: Mid) -> Big {
    Big {
        a: k ^ m.a,
        b: m.b as u64,
        c: k.wrapping_add(m.b as u64),
    }
}

#[no_mangle]
pub extern "C" fn rs_sext(x: u32) -> i64 {
    x as i32 as i64
}

#[no_mangle]
pub extern "C" fn rs_u32_ret(x: u32) -> u32 {
    x.wrapping_mul(3)
}

#[no_mangle]
pub extern "C" fn rs_tri(t: Tri) -> Tri {
    Tri {
        x: t.z,
        y: t.x,
        z: t.y.wrapping_add(1),
    }
}

#[no_mangle]
#[allow(improper_ctypes_definitions)]
pub extern "C" fn rs_wide(w: Wide, u: u32) -> Wide {
    Wide {
        v: w.v.wrapping_mul(3).wrapping_add(u as i128),
    }
}

// ---- checking ----

static mut FAILS: u32 = 0;
static mut CHECKS: u32 = 0;

fn check<T: PartialEq + core::fmt::Debug>(name: &str, got: T, want: T) {
    unsafe { CHECKS += 1 };
    if got == want {
        println!("PASS {}", name);
    } else {
        unsafe { FAILS += 1 };
        println!("FAIL {}: got {:?}, want {:?}", name, got, want);
    }
}

fn calls() {
    let u = black_box(0x8000_0007u32);
    let k = black_box(0x0123_4567_89ab_cdefu64);
    let p = Pair {
        a: black_box(-12345),
        b: u ^ 5,
    };
    let t = Tri {
        x: 1,
        y: 0xfe,
        z: u as u8,
    };
    let b = Big {
        a: k,
        b: !k,
        c: k.wrapping_mul(7),
    };
    let w = Wide {
        v: ((k as i64 as i128) << 64) | u as i128,
    };
    let mut un = U { b: [0; 6] };
    unsafe {
        un.w = u.wrapping_mul(9);
        un.b[4] = 0x44;
        un.b[5] = 0x55;
    }

    let m = unsafe { c_combine(p, t, u, -3, b, w, un) };
    let h = hash_args(p, t, u, -3, b, w, un);
    check(
        "c_combine",
        m,
        Mid {
            a: h,
            b: (h as u32) ^ u,
        },
    );
    check(
        "c_combine keeps the caller's struct",
        b,
        Big {
            a: k,
            b: !k,
            c: k.wrapping_mul(7),
        },
    );
    check("c_big (sret)", unsafe { c_big(k, m) }, rs_big(k, m));
    check(
        "c_sext (u32 argument)",
        unsafe { c_sext(u) },
        u as i32 as i64,
    );
    let x = unsafe { c_u32_ret(u) };
    check(
        "c_u32_ret (u32 result)",
        x as i32 as i64,
        u.wrapping_mul(3) as i32 as i64,
    );
    check("c_u32_ret value", x, u.wrapping_mul(3));
    check("c_tri (3-byte struct)", unsafe { c_tri(t) }, rs_tri(t));
    check(
        "c_wide (16-byte aligned)",
        unsafe { c_wide(w, u) },
        rs_wide(w, u),
    );
    check(
        "C calls the Rust twins (failed-check mask)",
        unsafe { c_calls_rust() },
        0,
    );
}

static A8: AtomicU8 = AtomicU8::new(250);
static I8: AtomicI8 = AtomicI8::new(-100);
static A16: AtomicU16 = AtomicU16::new(0xffff);
static I16: AtomicI16 = AtomicI16::new(-7);
static A32: AtomicU32 = AtomicU32::new(5);
static I32: AtomicI32 = AtomicI32::new(-5);
static A64: AtomicU64 = AtomicU64::new(0xffff_ffff_ffff_fff0);
static I64: AtomicI64 = AtomicI64::new(i64::MIN + 1);
static FLAG: AtomicBool = AtomicBool::new(false);
static SZ: AtomicUsize = AtomicUsize::new(0);
static mut TARGETS: [u32; 2] = [11, 22];
static PTR: AtomicPtr<u32> = AtomicPtr::new(core::ptr::null_mut());

fn atomics() {
    // 8 bits: wrap-around, zero-extended results.
    check("u8 fetch_add", A8.fetch_add(black_box(10), SeqCst), 250);
    check("u8 after fetch_add", A8.load(SeqCst), 4);
    check("u8 fetch_sub", A8.fetch_sub(5, SeqCst), 4);
    check("u8 fetch_nand", A8.fetch_nand(0x0f, SeqCst), 0xff);
    check("u8 after fetch_nand", A8.load(SeqCst), 0xf0);
    check("i8 fetch_min", I8.fetch_min(-101, SeqCst), -100);
    check("i8 fetch_max", I8.fetch_max(black_box(3), SeqCst), -101);
    check("i8 after fetch_max", I8.load(SeqCst), 3);
    check("u8 swap", A8.swap(0x80, SeqCst), 0xf0);
    check(
        "u8 C __atomic_exchange_n",
        unsafe { c_xchg8(A8.as_ptr(), 0x7f) },
        0x80,
    );
    check("u8 after C exchange", A8.load(SeqCst), 0x7f);

    // 16 bits.
    check("u16 fetch_add wraps", A16.fetch_add(1, SeqCst), 0xffff);
    check("u16 after fetch_add", A16.load(SeqCst), 0);
    check("u16 fetch_or", A16.fetch_or(0x8001, SeqCst), 0);
    check("u16 fetch_xor", A16.fetch_xor(0xffff, SeqCst), 0x8001);
    check("u16 after fetch_xor", A16.load(SeqCst), 0x7ffe);
    check(
        "i16 C __atomic_fetch_max",
        unsafe { c_fetch_max16(I16.as_ptr(), -9) },
        -7,
    );
    check("i16 after C max", I16.load(SeqCst), -7);
    check(
        "i16 C __atomic_fetch_max raises",
        unsafe { c_fetch_max16(I16.as_ptr(), 300) },
        -7,
    );
    check("i16 after C max raises", I16.load(SeqCst), 300);

    // 32 bits: signed vs unsigned min/max on values with the top bit set.
    check(
        "u32 fetch_max (unsigned)",
        A32.fetch_max(black_box(0x8000_0000), SeqCst),
        5,
    );
    check("u32 after fetch_max", A32.load(SeqCst), 0x8000_0000);
    check(
        "u32 fetch_min (unsigned)",
        A32.fetch_min(7, SeqCst),
        0x8000_0000,
    );
    check("u32 after fetch_min", A32.load(SeqCst), 7);
    check(
        "i32 fetch_max (signed)",
        I32.fetch_max(i32::MIN, SeqCst),
        -5,
    );
    check("i32 after fetch_max", I32.load(SeqCst), -5);
    check(
        "i32 fetch_min (signed)",
        I32.fetch_min(i32::MIN, SeqCst),
        -5,
    );
    check("u32 fetch_and", A32.fetch_and(!1, SeqCst), 7);
    check(
        "u32 C __atomic_fetch_add",
        unsafe { c_fetch_add32(A32.as_ptr(), 0xffff_fff0) },
        6,
    );
    check("u32 after C add", A32.load(SeqCst), 0xffff_fff6);
    check(
        "u32 compare_exchange ok",
        A32.compare_exchange(0xffff_fff6, 0x8000_0001, SeqCst, SeqCst),
        Ok(0xffff_fff6),
    );
    check(
        "u32 compare_exchange fails",
        A32.compare_exchange(0xffff_fff6, 1, SeqCst, SeqCst),
        Err(0x8000_0001),
    );
    check(
        "u32 fetch_update",
        A32.fetch_update(SeqCst, SeqCst, |v| Some(v.rotate_left(1))),
        Ok(0x8000_0001),
    );
    check("u32 after fetch_update", A32.load(SeqCst), 3);

    // 64 bits.
    check(
        "u64 fetch_add carries",
        A64.fetch_add(0x20, SeqCst),
        0xffff_ffff_ffff_fff0,
    );
    check("u64 after fetch_add", A64.load(SeqCst), 0x10);
    check("u64 fetch_sub", A64.fetch_sub(0x11, SeqCst), 0x10);
    check("u64 after fetch_sub", A64.load(SeqCst), u64::MAX);
    check("i64 fetch_sub", I64.fetch_sub(1, SeqCst), i64::MIN + 1);
    check("i64 fetch_min", I64.fetch_min(0, SeqCst), i64::MIN);
    check("i64 fetch_max", I64.fetch_max(-1, SeqCst), i64::MIN);
    let mut expected = u64::MAX;
    check(
        "u64 C __atomic_compare_exchange_n",
        unsafe { c_cas64(A64.as_ptr(), &mut expected, 42) },
        true,
    );
    check("u64 after C cas", A64.load(SeqCst), 42);
    check(
        "u64 C cas fails",
        unsafe { c_cas64(A64.as_ptr(), &mut expected, 43) },
        false,
    );
    check("u64 C cas reports the value", expected, 42);
    check(
        "u64 compare_exchange_weak",
        A64.compare_exchange_weak(42, 1 << 40, SeqCst, SeqCst),
        Ok(42),
    );

    // bool, usize, pointers.
    check("bool fetch_or", FLAG.fetch_or(true, SeqCst), false);
    check("bool fetch_xor", FLAG.fetch_xor(true, SeqCst), true);
    check(
        "bool compare_exchange",
        FLAG.compare_exchange(false, true, SeqCst, SeqCst),
        Ok(false),
    );
    check("bool fetch_and", FLAG.fetch_and(false, SeqCst), true);
    check("usize fetch_add", SZ.fetch_add(usize::MAX, SeqCst), 0);
    check("usize after fetch_add", SZ.load(SeqCst), usize::MAX);
    let (t0, t1) = unsafe {
        let t = &raw mut TARGETS;
        (&raw mut (*t)[0], &raw mut (*t)[1])
    };
    check("ptr swap", PTR.swap(t0, SeqCst), core::ptr::null_mut());
    check(
        "ptr compare_exchange",
        PTR.compare_exchange(t0, t1, SeqCst, SeqCst),
        Ok(t0),
    );
    check("ptr load", unsafe { *PTR.load(SeqCst) }, 22);
}

fn int_mask() -> u32 {
    unsafe { core::ptr::read_volatile(0xF00F_0000 as *const u32) }
}

fn main() -> ! {
    println!("ffi-check: Rust atomics and Rust <-> C calls");
    let mask = int_mask();
    calls();
    atomics();
    check("INT_MASK restored", int_mask(), mask);
    let (checks, fails) = unsafe { (CHECKS, FAILS) };
    println!("ffi-check: {} checks, {} failed", checks, fails);
    println!("Result: {}", if fails == 0 { "PASS" } else { "FAIL" });
    unsafe { c_halt(fails as i64) }
}
