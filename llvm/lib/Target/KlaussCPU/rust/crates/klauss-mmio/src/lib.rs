//! Typed, volatile access to KlaussCPU memory-mapped peripherals.
//!
//! Rust port of the runtime's `mmio.h`. The MMIO window is the top nibble
//! `0xF`: `addr[27:16]` selects the device, `addr[15:0]` the register.
//! Plain `read_volatile`/`write_volatile` lower to `LDIDX*`/`STIDX*` — no
//! intrinsics or `asm!` needed.
//!
//! Only the peripherals exercised so far are wrapped with typed helpers;
//! [`read32`]/[`write32`]/[`read64`] cover anything else (e.g. perf counters
//! at `0xF00D_xxxx`, cache stats at `0xF005_xxxx`).

#![no_std]

use core::ptr::{read_volatile, write_volatile};

pub const MMIO_BASE: usize = 0xF000_0000;
pub const RGB_BASE: usize = MMIO_BASE + 0x0002_0000;
pub const SEG_BASE: usize = MMIO_BASE + 0x0003_0000;
pub const IO_BASE: usize = MMIO_BASE + 0x0004_0000;
pub const INTC_BASE: usize = MMIO_BASE + 0x000F_0000;

// ---- generic accessors ----

/// Read a 32-bit MMIO register.
///
/// # Safety
/// `addr` must be a valid, correctly-aligned MMIO register address.
#[inline]
pub unsafe fn read32(addr: usize) -> u32 {
    read_volatile(addr as *const u32)
}

/// Write a 32-bit MMIO register.
///
/// # Safety
/// `addr` must be a valid, correctly-aligned MMIO register address.
#[inline]
pub unsafe fn write32(addr: usize, val: u32) {
    write_volatile(addr as *mut u32, val)
}

/// Read a 64-bit MMIO register (e.g. perf counters, `CLOCK_MS`).
///
/// # Safety
/// `addr` must be a valid, 8-byte-aligned MMIO register address.
#[inline]
pub unsafe fn read64(addr: usize) -> u64 {
    read_volatile(addr as *const u64)
}

// ---- LEDs / switches (0xF004_xxxx) ----

const REG_LEDS: usize = IO_BASE + 0x0000;
const REG_SWITCHES: usize = IO_BASE + 0x0008;

/// Set the 16 board LEDs to `bits`.
#[inline]
pub fn leds(bits: u32) {
    unsafe { write32(REG_LEDS, bits) }
}

/// Read the 16 board slide switches.
#[inline]
pub fn switches() -> u32 {
    unsafe { read32(REG_SWITCHES) }
}

// ---- 7-segment display (0xF003_xxxx) ----

const REG_SEG_LOW: usize = SEG_BASE + 0x0000;
const REG_SEG_HIGH: usize = SEG_BASE + 0x0008;
const REG_SEG_ALL: usize = SEG_BASE + 0x0010;
const REG_SEG_BLANK: usize = SEG_BASE + 0x0018;

/// Display the low 32 bits of `val` as 8 packed hex digits.
#[inline]
pub fn seg_all(val: u32) {
    unsafe { write32(REG_SEG_ALL, val) }
}

/// Set only the lower four hex digits.
#[inline]
pub fn seg_low(val: u32) {
    unsafe { write32(REG_SEG_LOW, val) }
}

/// Set only the upper four hex digits.
#[inline]
pub fn seg_high(val: u32) {
    unsafe { write32(REG_SEG_HIGH, val) }
}

/// Blank the display (write any value; the register is write-to-blank).
#[inline]
pub fn seg_blank() {
    unsafe { write32(REG_SEG_BLANK, 0) }
}

// ---- RGB LEDs (0xF002_xxxx) ----

const REG_RGB1: usize = RGB_BASE + 0x0000;
const REG_RGB2: usize = RGB_BASE + 0x0008;

/// Pack `r`, `g`, `b` (each 0..=15) into the 12-bit RGB register value.
#[inline]
pub const fn rgb12(r: u8, g: u8, b: u8) -> u32 {
    (((r & 0xF) as u32) << 8) | (((g & 0xF) as u32) << 4) | ((b & 0xF) as u32)
}

/// Set tricolour LED 1 (use [`rgb12`] to build the value).
#[inline]
pub fn rgb1(val: u32) {
    unsafe { write32(REG_RGB1, val) }
}

/// Set tricolour LED 2.
#[inline]
pub fn rgb2(val: u32) {
    unsafe { write32(REG_RGB2, val) }
}

// ---- timer / wall clock (0xF00F_xxxx) ----

const REG_CLOCK_MS: usize = INTC_BASE + 0x0040;

/// Free-running milliseconds since FPGA configuration (atomic 64-bit read).
///
/// Note: the 32-bit `TIMER_CNT` at `+0x38` is a *period* counter that wraps
/// each rollover — use this for elapsed-time measurement instead.
#[inline]
pub fn clock_ms() -> u64 {
    unsafe { read64(REG_CLOCK_MS) }
}
