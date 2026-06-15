//! Bare-metal runtime glue for KlaussCPU — the Rust counterpart of `crt0.c`.
//!
//! Use the [`entry!`] macro to declare the program entry point; it emits the
//! `_start` symbol into `.text._start` (so the linker places it at `0x20`),
//! zeroes `.bss`, runs the UART-drain startup delay, then calls your `main`.
//!
//! ```ignore
//! klauss_rt::entry!(main);
//! fn main() -> ! { /* ... */ loop {} }
//! ```
//!
//! With the default `panic-handler` feature this crate also supplies the
//! program's `#[panic_handler]` (sets LEDs = `0xAAAA` and parks). Disable the
//! feature to provide your own.

#![no_std]

extern "C" {
    static mut __bss_start: u8;
    static mut __bss_end: u8;
}

/// Zero the `.bss` section. Called by [`entry!`] before `main`.
///
/// # Safety
/// Must run exactly once at startup, before any static is read.
#[doc(hidden)]
pub unsafe fn __bss_init() {
    let mut p = core::ptr::addr_of_mut!(__bss_start);
    let end = core::ptr::addr_of_mut!(__bss_end);
    while p < end {
        p.write_volatile(0);
        p = p.add(1);
    }
}

/// Volatile busy-loop, used to let the serial loader's trailing bytes drain
/// before the program's first UART output (mirrors crt0.c / Zephyr `__start`).
#[doc(hidden)]
pub fn __startup_delay(iters: u32) {
    let mut i: u32 = 0;
    while unsafe { core::ptr::read_volatile(&i) } < iters {
        let v = unsafe { core::ptr::read_volatile(&i) };
        unsafe { core::ptr::write_volatile(&mut i, v + 1) };
    }
}

/// Declare the bare-metal entry point.
///
/// `$main` must be `fn() -> !`. Expands to the `_start` symbol (placed at
/// `0x20` via `.text._start`) that initialises `.bss`, delays, then calls it.
#[macro_export]
macro_rules! entry {
    ($main:path) => {
        #[no_mangle]
        #[link_section = ".text._start"]
        pub extern "C" fn _start() -> ! {
            // Type-check the signature at the call site.
            let f: fn() -> ! = $main;
            unsafe { $crate::__bss_init() };
            $crate::__startup_delay(200_000);
            f()
        }
    };
}

#[cfg(feature = "panic-handler")]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // LEDs = 0xAAAA marks "panicked"; park (no console assumption here).
    unsafe { core::ptr::write_volatile(0xF004_0000 as *mut u32, 0xAAAA) };
    loop {}
}
