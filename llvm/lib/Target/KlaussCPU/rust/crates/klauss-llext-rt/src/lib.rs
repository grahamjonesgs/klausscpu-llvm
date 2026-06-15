//! Runtime glue for a Rust program built as a Zephyr LLEXT extension.
//!
//! Pulls in the kernel-heap global allocator (so `alloc` works) and the
//! extension `#[panic_handler]`, and provides [`entry!`] to declare the
//! `main` symbol the loader resolves and calls on the SSH connection thread.
//!
//! ```ignore
//! #![no_std]
//! extern crate alloc;
//! klauss_llext_rt::entry!(run);
//! fn run() -> i32 { /* ... */ 0 }
//! ```
//!
//! Imports the program will carry (all in `ssh/llext_exports.c`):
//! `putchar` (stdio), `malloc`/`free` (heap), `memcpy` (if not built with
//! `compiler-builtins-mem`).

#![no_std]

// Install the kernel-heap allocator for the whole extension.
klauss_alloc::global_allocator!();

/// Declare the extension entry point.
///
/// `$main` must be `fn() -> i32` (the process-style exit code; the loader
/// prints it). Expands to the `extern "C" fn main(argc, argv)` symbol the
/// LLEXT loader looks up by name.
#[macro_export]
macro_rules! entry {
    ($main:path) => {
        #[no_mangle]
        pub extern "C" fn main(
            _argc: ::core::ffi::c_int,
            _argv: *mut *mut ::core::ffi::c_char,
        ) -> ::core::ffi::c_int {
            let f: fn() -> i32 = $main;
            f() as ::core::ffi::c_int
        }
    };
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write;
    // Print to the SSH session via the kernel console route, then park.
    // (A clean per-extension exit needs the `llext_exit` kernel export —
    //  RUST_PLAN.md §6.5 — which parks the connection thread until then.)
    let _ = writeln!(klauss_io::Console, "\nrust .llext panic: {}", info);
    loop {}
}
