// Compiles the UART C shim with the fork's clang. The TXCHARMEMR transmit
// path is only reachable via __builtin_klausscpu_* (opcodes, not MMIO), and
// Rust has no asm!/builtin access on this target yet — see RUST_PLAN.md §5.3.

use std::env;
use std::process::Command;

fn main() {
    let out = env::var("OUT_DIR").unwrap();
    let bin = env::var("KLAUSSCPU_LLVM_BIN").unwrap_or_else(|_| {
        "/Users/gjonesblackcyton/Documents/src/klausscpu-llvm/build/bin".into()
    });

    let status = Command::new(format!("{bin}/clang"))
        .args([
            "-target", "klausscpu-unknown-elf",
            "-O1", "-nostdlib", "-nostdinc", "-ffreestanding",
            "-c", "src/uart_shim.c", "-o",
        ])
        .arg(format!("{out}/uart_shim.o"))
        .status()
        .expect("failed to run fork clang (set KLAUSSCPU_LLVM_BIN)");
    assert!(status.success(), "uart_shim.c failed to compile");

    let status = Command::new(format!("{bin}/llvm-ar"))
        .arg("rcs")
        .arg(format!("{out}/libuart_shim.a"))
        .arg(format!("{out}/uart_shim.o"))
        .status()
        .expect("failed to run llvm-ar");
    assert!(status.success(), "llvm-ar failed");

    println!("cargo:rustc-link-search=native={out}");
    println!("cargo:rustc-link-lib=static=uart_shim");
    println!("cargo:rerun-if-changed=src/uart_shim.c");
    println!("cargo:rerun-if-env-changed=KLAUSSCPU_LLVM_BIN");
}
