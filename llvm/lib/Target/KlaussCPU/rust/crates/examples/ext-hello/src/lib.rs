//! LLEXT extension built from the klauss-* workspace crates.
//!
//! Compare with ../../../hello-ext: the global allocator, the Console/Write
//! impl, the panic handler and the `extern "C" fn main` wrapper are all
//! provided by klauss-llext-rt / klauss-io. This file is just the program.
//!
//! Run over SSH:  run ext_hello.llext

#![no_std]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;
use klauss_io::println;

klauss_llext_rt::entry!(run);

fn run() -> i32 {
    println!("Hello from the klauss-* crate workspace, as a Zephyr .llext!");
    println!();

    // Vec on the kernel heap.
    let mut fib: Vec<u64> = Vec::new();
    let (mut a, mut b) = (0u64, 1u64);
    for _ in 0..12 {
        fib.push(a);
        let n = a + b;
        a = b;
        b = n;
    }
    println!("fib (Vec)        = {:?}", fib);

    // BTreeMap: word-frequency count, exercising heap-heavy alloc.
    let text = "the quick brown fox the lazy dog the end";
    let mut counts: BTreeMap<&str, u32> = BTreeMap::new();
    for w in text.split_whitespace() {
        *counts.entry(w).or_insert(0) += 1;
    }
    let mut line = String::new();
    for (w, c) in &counts {
        let _ = write!(line, "{}:{} ", w, c);
    }
    println!("word counts      = {}", line.trim_end());
    println!("sum(fib)         = {}", fib.iter().sum::<u64>());

    println!();
    println!("rust crate-workspace extension done.");
    0
}
