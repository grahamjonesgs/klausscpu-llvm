//! M1 smoke test: first Rust on KlaussCPU.
//!
//! Board behavior: 7-seg shows the low 32 bits of a computed value,
//! LEDs mirror the switches XOR 0x55. No UART, no allocator, no C runtime —
//! pure core + MMIO. Loaded like any bare-metal .bin (entry at 0x20, SP set
//! by hardware reset).

#![no_std]
#![no_main]

use core::ptr::{read_volatile, write_volatile};

const REG_SEG_ALL: *mut u32 = 0xF003_0010 as *mut u32;
const REG_LEDS: *mut u32 = 0xF004_0000 as *mut u32;
const REG_SWITCHES: *const u32 = 0xF004_0008 as *const u32;

/// Kept out-of-line to exercise the call ABI (args in R0/R1, return in R12).
#[inline(never)]
fn mix(a: u64, b: u64) -> u64 {
    a.wrapping_mul(b) ^ (a >> 7) ^ b.rotate_left(13)
}

#[no_mangle]
#[link_section = ".text._start"]
pub extern "C" fn _start() -> ! {
    let x = mix(0x1234_5678_9ABC_DEF0, 0xDEAD_BEEF_CAFE_F00D);
    unsafe {
        write_volatile(REG_SEG_ALL, x as u32);
        loop {
            let sw = read_volatile(REG_SWITCHES);
            write_volatile(REG_LEDS, sw ^ 0x55);
        }
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
