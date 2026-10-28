//! LLEXT extension: `core::sync::atomic` read-modify-writes.
//!
//! KlaussCPU has no atomic instructions: every atomic read-modify-write,
//! swap and compare-exchange of 1, 2, 4 and 8 bytes is a call to a
//! `__sync_*_N` function (forced atomics).  An extension does not carry
//! them: the kernel exports its own (klausscpu-runtime's src/atomic.c,
//! `EXPORT_SYNC_ALL` in ssh/llext_exports.c), so they resolve at load time.
//! Each check prints nothing unless it fails; the summary line and the exit
//! code say whether all passed.
//!
//! Run over SSH:  run ext_atomics.llext

#![no_std]

use core::hint::black_box;
use core::sync::atomic::Ordering::SeqCst;
use core::sync::atomic::*;
use klauss_io::println;

klauss_llext_rt::entry!(run);

struct Checks {
    pass: u32,
    fail: u32,
}

impl Checks {
    fn check(&mut self, name: &str, ok: bool) {
        if ok {
            self.pass += 1;
        } else {
            self.fail += 1;
            println!("FAIL {}", name);
        }
    }
}

// Every RMW at one width; `$top` is the top bit (unsigned: largest; signed
// twin: most negative).
macro_rules! width {
    ($c:ident, $name:literal, $A:ty, $S:ty, $T:ty, $ST:ty) => {{
        static X: $A = <$A>::new(0);
        static S: $S = <$S>::new(0);
        let max: $T = <$T>::MAX;
        let top: $T = max ^ (max >> 1);
        X.store(black_box(max - 1), SeqCst);
        $c.check(
            concat!($name, " fetch_add wraps"),
            X.fetch_add(black_box(3), SeqCst) == max - 1 && X.load(SeqCst) == 1,
        );
        $c.check(
            concat!($name, " fetch_sub wraps"),
            X.fetch_sub(black_box(2), SeqCst) == 1 && X.load(SeqCst) == max,
        );
        $c.check(
            concat!($name, " fetch_and"),
            X.fetch_and(black_box(top | 0x0F), SeqCst) == max && X.load(SeqCst) == top | 0x0F,
        );
        $c.check(
            concat!($name, " fetch_or"),
            X.fetch_or(black_box(0x30), SeqCst) == top | 0x0F && X.load(SeqCst) == top | 0x3F,
        );
        $c.check(
            concat!($name, " fetch_xor"),
            X.fetch_xor(black_box(top | 0x11), SeqCst) == top | 0x3F && X.load(SeqCst) == 0x2E,
        );
        $c.check(
            concat!($name, " fetch_nand"),
            X.fetch_nand(black_box(0x0C), SeqCst) == 0x2E && X.load(SeqCst) == !(0x0C as $T),
        );
        $c.check(
            concat!($name, " swap"),
            X.swap(black_box(top), SeqCst) == !(0x0C as $T) && X.load(SeqCst) == top,
        );
        $c.check(
            concat!($name, " fetch_max (unsigned) keeps"),
            X.fetch_max(black_box(1), SeqCst) == top && X.load(SeqCst) == top,
        );
        $c.check(
            concat!($name, " fetch_min (unsigned) takes"),
            X.fetch_min(black_box(1), SeqCst) == top && X.load(SeqCst) == 1,
        );
        $c.check(
            concat!($name, " compare_exchange succeeds"),
            X.compare_exchange(1, black_box(top | 2), SeqCst, SeqCst) == Ok(1)
                && X.load(SeqCst) == top | 2,
        );
        $c.check(
            concat!($name, " compare_exchange fails"),
            X.compare_exchange(1, black_box(5), SeqCst, SeqCst) == Err(top | 2)
                && X.load(SeqCst) == top | 2,
        );
        let mut seen = 0;
        let r = X.fetch_update(SeqCst, SeqCst, |v| {
            seen = v;
            Some(v.wrapping_mul(3))
        });
        $c.check(
            concat!($name, " fetch_update"),
            r == Ok(top | 2) && seen == top | 2 && X.load(SeqCst) == (top | 2).wrapping_mul(3),
        );
        S.store(black_box(top as $ST), SeqCst);
        $c.check(
            concat!($name, " fetch_max (signed) takes"),
            S.fetch_max(black_box(1), SeqCst) == top as $ST && S.load(SeqCst) == 1,
        );
        $c.check(
            concat!($name, " fetch_min (signed) takes"),
            S.fetch_min(black_box(-7), SeqCst) == 1 && S.load(SeqCst) == -7,
        );
        $c.check(
            concat!($name, " fetch_min (signed) keeps"),
            S.fetch_min(black_box(3), SeqCst) == -7 && S.load(SeqCst) == -7,
        );
    }};
}

fn run() -> i32 {
    let mut c = Checks { pass: 0, fail: 0 };
    width!(c, "u8", AtomicU8, AtomicI8, u8, i8);
    width!(c, "u16", AtomicU16, AtomicI16, u16, i16);
    width!(c, "u32", AtomicU32, AtomicI32, u32, i32);
    width!(c, "u64", AtomicU64, AtomicI64, u64, i64);

    static FLAG: AtomicBool = AtomicBool::new(false);
    c.check(
        "bool swap",
        !FLAG.swap(black_box(true), SeqCst) && FLAG.load(SeqCst),
    );
    c.check(
        "bool fetch_xor",
        FLAG.fetch_xor(black_box(true), SeqCst) && !FLAG.load(SeqCst),
    );
    static COUNT: AtomicUsize = AtomicUsize::new(0);
    for _ in 0..1000 {
        COUNT.fetch_add(black_box(1), Ordering::Relaxed);
    }
    c.check("usize counter", COUNT.load(SeqCst) == 1000);

    println!(
        "rust atomics in an LLEXT extension: {} pass, {} fail",
        c.pass, c.fail
    );
    if c.fail == 0 {
        0
    } else {
        1
    }
}
