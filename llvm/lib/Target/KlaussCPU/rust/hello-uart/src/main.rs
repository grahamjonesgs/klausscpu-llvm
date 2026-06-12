//! M2 UART hello: Rust `core::fmt` printing over the KlaussCPU UART, with
//! runtime-valued bit-manipulation ops so the new isel patterns (ROLR/RORR,
//! ABSR, BITREV) and the existing CLZ/CTZ/POPCNT/BSWAP/MULHUR actually
//! execute on silicon (values are seeded from the switches — nothing can
//! const-fold).
//!
//! Board behavior: prints a banner + computed table over the UART
//! (115200, same terminal as the C hellos), then idles with LEDs = 0xFF
//! and the switch value XOR 0xC0DE on the 7-seg.

#![no_std]
#![no_main]

use core::fmt::{self, Write};
use core::ptr::{addr_of_mut, read_volatile, write_volatile};

const REG_SEG_ALL: *mut u32 = 0xF003_0010 as *mut u32;
const REG_LEDS: *mut u32 = 0xF004_0000 as *mut u32;
const REG_SWITCHES: *const u32 = 0xF004_0008 as *const u32;

extern "C" {
    fn uart_putc(c: u8);
    static mut __bss_start: u8;
    static mut __bss_end: u8;
}

struct Uart;

impl Write for Uart {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { uart_putc(b) }
        }
        Ok(())
    }
}

/// crt0-equivalent startup: LED checkpoints, BSS clear, UART-drain delay.
unsafe fn startup() {
    write_volatile(REG_LEDS, 0x1111); // checkpoint A: entered _start

    let mut p = addr_of_mut!(__bss_start);
    let end = addr_of_mut!(__bss_end);
    while p < end {
        p.write_volatile(0);
        p = p.add(1);
    }
    write_volatile(REG_LEDS, 0x2222); // checkpoint B: BSS clear done

    // Startup delay so the serial loader's trailing "Load Complete OK" bytes
    // drain before we print (same reason as crt0.c / Zephyr __start).
    let mut i: u32 = 0;
    while read_volatile(&i) < 200_000 {
        let v = read_volatile(&i);
        write_volatile(&mut i, v + 1);
    }
}

#[no_mangle]
#[link_section = ".text._start"]
pub extern "C" fn _start() -> ! {
    unsafe { startup() }

    let mut out = Uart;
    let _ = writeln!(out, "Hello from Rust on KlaussCPU!");
    let _ = writeln!(out, "built by rustc 1.98.0-dev + klausscpu-llvm (LLVM 23)");
    let _ = writeln!(out);

    // Seed from the switches so every value below is runtime-computed.
    let sw = unsafe { read_volatile(REG_SWITCHES) } as u64;
    let x = 0x0123_4567_89AB_CDEFu64 ^ (sw << 32) ^ sw;
    let n = (sw as u32) & 63;

    let _ = writeln!(out, "switches            = {:#06x}", sw);
    let _ = writeln!(out, "x                   = {:#018x}", x);
    let _ = writeln!(out, "x.rotate_left({:2})   = {:#018x}", n, x.rotate_left(n));
    let _ = writeln!(out, "x.rotate_right({:2})  = {:#018x}", n, x.rotate_right(n));
    let _ = writeln!(out, "x.count_ones()      = {}", x.count_ones());
    let _ = writeln!(out, "x.leading_zeros()   = {}", (x >> n).leading_zeros());
    let _ = writeln!(out, "x.trailing_zeros()  = {}", (x << n).trailing_zeros());
    let _ = writeln!(out, "x.swap_bytes()      = {:#018x}", x.swap_bytes());
    let _ = writeln!(out, "x.reverse_bits()    = {:#018x}", x.reverse_bits());
    let _ = writeln!(out, "(x as i64).abs()    = {:#018x}", (x as i64).wrapping_abs());
    let wide = (x as u128).wrapping_mul(0x1_0000_0001u128);
    let _ = writeln!(out, "u128 mul, high 64   = {:#018x}", (wide >> 64) as u64);
    let _ = writeln!(out, "x as decimal        = {}", x);
    let _ = writeln!(out);
    let _ = writeln!(out, "done. LEDs=0xFF, 7-seg = switches ^ 0xC0DE.");

    unsafe {
        write_volatile(REG_SEG_ALL, (sw as u32) ^ 0xC0DE);
        write_volatile(REG_LEDS, 0x00FF);
    }
    loop {}
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    let mut out = Uart;
    let _ = writeln!(out, "\nrust panic: {}", info);
    unsafe { write_volatile(REG_LEDS, 0xAAAA) }
    loop {}
}
