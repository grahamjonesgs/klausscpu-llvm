//! The `__sync_*_N` libcalls behind Rust's atomic read-modify-write and
//! compare-exchange operations — the Rust counterpart of klausscpu-runtime's
//! `src/atomic.c`, for bare-metal programs that do not link the C runtime.
//!
//! The ISA has no atomic RMW, CAS or LL/SC.  The backend uses the "forced
//! atomics" model: an aligned atomic load or store of 1/2/4/8 bytes is a plain
//! access, a fence a compiler barrier, and every RMW and `compare_exchange` a
//! call to one of these functions (the target spec's `max-atomic-width` 64 and
//! `atomic-cas` rely on them).  Each one is a critical section: save
//! `INT_MASK`, write 0, do the plain operation, restore the saved mask.
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
use core::sync::atomic::{compiler_fence, Ordering};

/// The interrupt controller's mask register (`REG_INT_MASK` in `mmio.h`).
const INT_MASK: *mut u32 = 0xF00F_0000 as *mut u32;

#[inline(always)]
unsafe fn irq_save() -> u32 {
    let mask = read_volatile(INT_MASK);
    write_volatile(INT_MASK, 0);
    compiler_fence(Ordering::SeqCst);
    mask
}

#[inline(always)]
unsafe fn irq_restore(mask: u32) {
    compiler_fence(Ordering::SeqCst);
    write_volatile(INT_MASK, mask);
}

macro_rules! fetch_and {
    ($name:literal, $n:literal, $t:ty, $old:ident, $val:ident => $new:expr) => {
        const _: () = {
            #[export_name = concat!($name, "_", $n)]
            unsafe extern "C" fn f(ptr: *mut $t, arg: u64) -> u64 {
                let $val = arg as $t;
                let mask = irq_save();
                let $old = read_volatile(ptr);
                write_volatile(ptr, $new);
                irq_restore(mask);
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
                let mask = irq_save();
                let old = read_volatile(ptr);
                if old == expected as $t {
                    write_volatile(ptr, desired as $t);
                }
                irq_restore(mask);
                old as u64
            }
        };
    };
}

sync_all!(1, u8, i8);
sync_all!(2, u16, i16);
sync_all!(4, u32, i32);
sync_all!(8, u64, i64);
