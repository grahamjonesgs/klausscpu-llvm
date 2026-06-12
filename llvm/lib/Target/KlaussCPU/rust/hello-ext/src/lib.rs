//! M3: first Rust LLEXT extension for the Zephyr SSH `run` command.
//!
//! Built as ELFCLASS32 ET_REL via staticlib + fat LTO + `ld.lld -r`
//! (see build-llext.sh). All imports resolve against the kernel's
//! llext_exports.c table: putchar (stdio, routed per-session by the
//! loader), malloc/free (shared kernel heap, backing Rust `alloc`).
//!
//! Exercises in one run: .text relocation, .rodata strings, a mutable
//! static (.data), Vec/String on the kernel heap, core::fmt, and the
//! C calling convention across the loader boundary.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};

extern "C" {
    fn putchar(c: c_int) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
}

// ---------------------------------------------------------------------------
// Rust `alloc` over the kernel heap (CONFIG_COMMON_LIBC_MALLOC arena).
// Kernel malloc returns 8-byte-aligned blocks; that covers every type this
// demo allocates. (A real klauss-alloc crate should over-allocate + align
// for Layouts with align > 8.)
// ---------------------------------------------------------------------------
struct KernelHeap;

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() > 8 {
            return core::ptr::null_mut();
        }
        malloc(layout.size()) as *mut u8
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr as *mut c_void)
    }
}

#[global_allocator]
static HEAP: KernelHeap = KernelHeap;

// ---------------------------------------------------------------------------
// Console: the loader routes putchar to the calling SSH session and does
// its own \n handling.
// ---------------------------------------------------------------------------
struct Console;

impl Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { putchar(b as c_int) };
        }
        Ok(())
    }
}

// Mutable static: proves the .data section is placed + relocated and
// persists across calls within one load.
static mut RUN_STAMP: u64 = 0xC0FFEE;

#[no_mangle]
pub extern "C" fn main(argc: c_int, _argv: *mut *mut c_char) -> c_int {
    let mut out = Console;

    let _ = writeln!(out, "Hello from a RUST .llext on Zephyr!");
    let _ = writeln!(out, "(rustc 1.98.0-dev / klausscpu-llvm LLVM 23, ld.lld -r)");
    let _ = writeln!(out);

    let stamp = unsafe {
        RUN_STAMP = RUN_STAMP.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        RUN_STAMP
    };
    let _ = writeln!(out, "argc          = {}", argc);
    let _ = writeln!(out, ".data stamp   = {:#018x}", stamp);

    // Vec on the kernel heap: first 12 Fibonacci numbers.
    let mut fib: Vec<u64> = Vec::new();
    let (mut a, mut b) = (0u64, 1u64);
    for _ in 0..12 {
        fib.push(a);
        let n = a + b;
        a = b;
        b = n;
    }
    let _ = writeln!(out, "fib (Vec)     = {:?}", fib);

    // String formatting on the heap.
    let mut s = String::new();
    let _ = write!(s, "heap String: sum(fib) = {}", fib.iter().sum::<u64>());
    let _ = writeln!(out, "{}", s);

    let _ = writeln!(out);
    let _ = writeln!(out, "rust extension done.");
    0
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    let mut out = Console;
    let _ = writeln!(out, "\nrust .llext panic: {}", info);
    // No kernel-assisted exit yet (RUST_PLAN.md §6.5: llext_exit export).
    // Parking the connection thread is the least-bad option for the demo.
    loop {}
}
