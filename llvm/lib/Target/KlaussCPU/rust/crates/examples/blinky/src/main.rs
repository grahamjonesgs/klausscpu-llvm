//! Bare-metal example built from the klauss-* workspace crates.
//!
//! Compare with ../../../hello-uart: the MMIO addresses, the Console/Write
//! impl, _start, BSS-clear and the panic handler are all gone — provided by
//! klauss-mmio / klauss-io / klauss-rt. This file is just the program.
//!
//! Behavior: prints a banner over UART, then loops mirroring the switches to
//! the LEDs and showing a free-running millisecond counter on the 7-seg.

#![no_std]
#![no_main]

use klauss_io::println;
use klauss_mmio as mmio;

klauss_rt::entry!(main);

fn main() -> ! {
    println!("blinky: Rust on KlaussCPU via the klauss-* crates");
    println!("switches -> LEDs, clock_ms -> 7-seg. flip switches to see them mirror.");

    let mut last = u32::MAX;
    loop {
        let sw = mmio::switches();
        if sw != last {
            mmio::leds(sw);
            println!("switches = {:#06x}", sw);
            last = sw;
        }
        // Low 32 bits of the wall clock on the display (changes ~every ms).
        mmio::seg_all(mmio::clock_ms() as u32);
    }
}
