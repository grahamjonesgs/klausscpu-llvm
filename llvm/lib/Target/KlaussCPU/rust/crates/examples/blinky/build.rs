// Compiles the UART C shim (the TX path is an opcode reachable only via
// __builtin_klausscpu_*; Rust has no asm!/builtin access on this target yet).
// Shared with the standalone hello-uart example.

use std::env;
use std::process::Command;

fn main() {
    let out = env::var("OUT_DIR").unwrap();
    let bin = env::var("KLAUSSCPU_LLVM_BIN").unwrap_or_else(|_| {
        "/Users/gjonesblackcyton/Documents/src/llvm-project/build/bin".into()
    });

    let ok = Command::new(format!("{bin}/clang"))
        .args([
            "-target", "klausscpu-unknown-elf",
            "-O1", "-nostdlib", "-nostdinc", "-ffreestanding",
            "-c", "src/uart_shim.c", "-o",
        ])
        .arg(format!("{out}/uart_shim.o"))
        .status()
        .expect("run fork clang (set KLAUSSCPU_LLVM_BIN)")
        .success();
    assert!(ok, "uart_shim.c failed to compile");

    let ok = Command::new(format!("{bin}/llvm-ar"))
        .arg("rcs")
        .arg(format!("{out}/libuart_shim.a"))
        .arg(format!("{out}/uart_shim.o"))
        .status()
        .expect("run llvm-ar")
        .success();
    assert!(ok, "llvm-ar failed");

    println!("cargo:rustc-link-search=native={out}");
    println!("cargo:rustc-link-lib=static=uart_shim");
    println!("cargo:rerun-if-changed=src/uart_shim.c");
    println!("cargo:rerun-if-env-changed=KLAUSSCPU_LLVM_BIN");
}
