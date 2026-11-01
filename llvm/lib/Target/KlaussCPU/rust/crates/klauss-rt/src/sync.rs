//! The `__sync_*_N` libcalls behind Rust's atomic read-modify-write and
//! compare-exchange operations — the Rust counterpart of klausscpu-runtime's
//! `src/atomic.c`, for bare-metal programs that do not link the C runtime.
//!
//! The ISA has no atomic RMW, CAS or LL/SC.  The backend uses the "forced
//! atomics" model: an aligned atomic load or store of 1/2/4/8 bytes is a plain
//! access, a fence a compiler barrier, and every RMW and `compare_exchange` a
//! call to one of these functions (the target spec's `max-atomic-width` 64 and
//! `atomic-cas` rely on them).  Each one is a critical section: `DI` (disable
//! interrupts, returning the old enable bit), the plain operation, `EI` with
//! that bit (ISA_ENCODING_V2_MAP.md §8.2; about 3 cycles, and the section
//! nests).  `INT_MASK`, the per-source mask, is not touched.
//! **Atomic within one core only** — against the core's own interrupt
//! handlers, not against the second (AMP) core.
//!
//! Calling convention as the backend uses it: the type legaliser widens a
//! 1/2/4-byte operation to 64 bits first, so the value arguments come in with
//! undefined upper bits and the result must be zero-extended.  Every function
//! therefore takes and returns `u64` and truncates / zero-extends itself.
//! Only volatile accesses are used, so nothing here turns back into an atomic
//! (a call to itself).  Unused functions are removed by `--gc-sections`.
//!
//! A program that also links the C runtime's `atomic.c` must turn off this
//! crate's `sync-libcalls` feature (duplicate symbols otherwise).

use core::ptr::{read_volatile, write_volatile};

// rustc has no `asm!` for this target, so DI/EI are the backend's LLVM
// intrinsics (IntrinsicsKlaussCPU.td; clang's __builtin_klausscpu_di / _ei).
// They are i64 -> i64 / i64 -> void, read and write memory as far as LLVM is
// concerned (so every access stays on its side of the section's edges) and
// are never merged or deleted.
extern "C" {
    #[link_name = "llvm.klausscpu.di"]
    fn llvm_di() -> u64;
    #[link_name = "llvm.klausscpu.ei"]
    fn llvm_ei(key: u64);
}

#[inline(always)]
unsafe fn irq_save() -> u64 {
    llvm_di()
}

#[inline(always)]
unsafe fn irq_restore(key: u64) {
    llvm_ei(key)
}

macro_rules! fetch_and {
    ($name:literal, $n:literal, $t:ty, $old:ident, $val:ident => $new:expr) => {
        const _: () = {
            #[export_name = concat!($name, "_", $n)]
            unsafe extern "C" fn f(ptr: *mut $t, arg: u64) -> u64 {
                let $val = arg as $t;
                let key = irq_save();
                let $old = read_volatile(ptr);
                write_volatile(ptr, $new);
                irq_restore(key);
                $old as u64
            }
        };
    };
}

/// All the functions for one width: `$n` bytes, unsigned `$t`, signed `$s`.
macro_rules! sync_all {
    ($n:literal, $t:ty, $s:ty) => {
        fetch_and!("__sync_fetch_and_add", $n, $t, old, val => old.wrapping_add(val));
        fetch_and!("__sync_fetch_and_sub", $n, $t, old, val => old.wrapping_sub(val));
        fetch_and!("__sync_fetch_and_and", $n, $t, old, val => old & val);
        fetch_and!("__sync_fetch_and_or", $n, $t, old, val => old | val);
        fetch_and!("__sync_fetch_and_xor", $n, $t, old, val => old ^ val);
        fetch_and!("__sync_fetch_and_nand", $n, $t, old, val => !(old & val));
        fetch_and!("__sync_fetch_and_max", $n, $t, old, val => (old as $s).max(val as $s) as $t);
        fetch_and!("__sync_fetch_and_min", $n, $t, old, val => (old as $s).min(val as $s) as $t);
        fetch_and!("__sync_fetch_and_umax", $n, $t, old, val => old.max(val));
        fetch_and!("__sync_fetch_and_umin", $n, $t, old, val => old.min(val));
        // atomicrmw xchg
        fetch_and!("__sync_lock_test_and_set", $n, $t, old, val => val);

        // cmpxchg: returns the old value; the caller compares it.
        const _: () = {
            #[export_name = concat!("__sync_val_compare_and_swap_", $n)]
            unsafe extern "C" fn f(ptr: *mut $t, expected: u64, desired: u64) -> u64 {
                let key = irq_save();
                let old = read_volatile(ptr);
                if old == expected as $t {
                    write_volatile(ptr, desired as $t);
                }
                irq_restore(key);
                old as u64
            }
        };
    };
}

sync_all!(1, u8, i8);
sync_all!(2, u16, i16);
sync_all!(4, u32, i32);
sync_all!(8, u64, i64);
