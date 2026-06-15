//! A `#[global_allocator]` backing Rust's `alloc` crate with the KlaussCPU
//! runtime heap (`malloc`/`free` from `klauss-sys`).
//!
//! In an LLEXT extension this shares the kernel's heap arena, so Rust `Box`,
//! `Vec`, `String`, `BTreeMap`, etc. allocate from the same pool as the C
//! side. The C `malloc` only guarantees 8-byte alignment, so this wrapper
//! over-allocates and stores the raw pointer just below the aligned block to
//! satisfy arbitrary `Layout::align()` values.

#![no_std]

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::mem::size_of;

pub struct KernelHeap;

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align().max(size_of::<usize>());
        // Room for the requested block, worst-case alignment padding, and one
        // word to stash the raw malloc pointer for dealloc.
        let total = layout.size() + align + size_of::<usize>();
        let raw = klauss_sys::malloc(total) as usize;
        if raw == 0 {
            return core::ptr::null_mut();
        }
        // First aligned address at least one word past `raw`.
        let aligned = (raw + size_of::<usize>() + align - 1) & !(align - 1);
        // Stash the raw pointer immediately below the returned block.
        *((aligned - size_of::<usize>()) as *mut usize) = raw;
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        let raw = *((ptr as usize - size_of::<usize>()) as *const usize);
        klauss_sys::free(raw as *mut c_void);
    }
}

/// Install [`KernelHeap`] as the program's global allocator.
///
/// Invoke once at the top of the binary/staticlib crate (a `#[global_allocator]`
/// may be defined only once per artifact). `klauss-llext-rt`'s `entry!` macro
/// already does this for extensions.
#[macro_export]
macro_rules! global_allocator {
    () => {
        #[global_allocator]
        static __KLAUSS_GLOBAL_HEAP: $crate::KernelHeap = $crate::KernelHeap;
    };
}
