//! Raw `extern "C"` surface to the KlaussCPU runtime.
//!
//! Two provenances are declared here; a given program references only the
//! subset it links against, so unused declarations cost nothing:
//!
//! * **Bare-metal IO shim** (`uart_putc`) — compiled per-binary from the C
//!   shim, since the UART TX path is an opcode (`__builtin_klausscpu_*`) that
//!   Rust cannot emit without `asm!`/builtins (RUST_PLAN.md §5.3).
//! * **Zephyr LLEXT kernel exports** (`putchar`, `malloc`, …) — resolved by
//!   the loader against `ssh/llext_exports.c`.
//!
//! These are the low-level primitives; prefer the safe wrappers in
//! `klauss-io`, `klauss-alloc`, and `klauss-mmio`.

#![no_std]

use core::ffi::{c_char, c_int, c_void};

extern "C" {
    // ---- bare-metal UART shim (uart_shim.c) ----
    /// Transmit one byte over the UART (the shim expands `\n` to CR+LF).
    pub fn uart_putc(c: u8);

    // ---- Zephyr LLEXT kernel exports (ssh/llext_exports.c) ----
    pub fn putchar(c: c_int) -> c_int;
    pub fn getchar() -> c_int;
    pub fn puts(s: *const c_char) -> c_int;

    pub fn malloc(size: usize) -> *mut c_void;
    pub fn calloc(nmemb: usize, size: usize) -> *mut c_void;
    pub fn realloc(ptr: *mut c_void, size: usize) -> *mut c_void;
    pub fn free(ptr: *mut c_void);
}
