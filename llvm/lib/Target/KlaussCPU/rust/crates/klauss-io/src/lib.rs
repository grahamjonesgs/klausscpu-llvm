//! Console output (and input) plus `print!`/`println!` for KlaussCPU `no_std`
//! programs, built on `core::fmt` so formatting never depends on C `printf`.
//!
//! Select the byte sink with exactly one Cargo feature:
//!
//! * `uart`   — bare-metal: bytes go through the C `uart_putc` shim.
//! * `kernel` — LLEXT: bytes go through the kernel's exported `putchar`,
//!   which the loader routes to the calling SSH session.
//!
//! ```ignore
//! use klauss_io::println;
//! println!("answer = {}", 42);
//! ```

#![no_std]

use core::fmt::{self, Write};

#[cfg(all(feature = "uart", feature = "kernel"))]
compile_error!("klauss-io: select only one of the `uart` / `kernel` features");

#[cfg(not(any(feature = "uart", feature = "kernel")))]
compile_error!("klauss-io: select a sink feature — `uart` (bare-metal) or `kernel` (llext)");

/// A zero-sized `core::fmt::Write` sink over the selected byte output.
pub struct Console;

impl Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { put_byte(b) };
        }
        Ok(())
    }
}

#[cfg(feature = "uart")]
#[inline]
unsafe fn put_byte(b: u8) {
    klauss_sys::uart_putc(b);
}

#[cfg(feature = "kernel")]
#[inline]
unsafe fn put_byte(b: u8) {
    klauss_sys::putchar(b as core::ffi::c_int);
}

/// Read one byte of console input, or `None` at EOF (`kernel` feature only;
/// the loader pulls from the SSH session).
#[cfg(feature = "kernel")]
#[inline]
pub fn read_byte() -> Option<u8> {
    let c = unsafe { klauss_sys::getchar() };
    if c < 0 {
        None
    } else {
        Some(c as u8)
    }
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    let _ = Console.write_fmt(args);
}

/// Print to the console with no trailing newline.
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::_print(::core::format_args!($($arg)*)) };
}

/// Print to the console followed by a newline (the sink expands `\n` for the
/// terminal as needed).
#[macro_export]
macro_rules! println {
    () => { $crate::_print(::core::format_args!("\n")) };
    ($($arg:tt)*) => {
        $crate::_print(::core::format_args!("{}\n", ::core::format_args!($($arg)*)))
    };
}
