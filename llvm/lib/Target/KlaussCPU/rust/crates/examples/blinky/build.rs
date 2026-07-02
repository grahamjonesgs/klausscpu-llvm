// Compiles the UART C shim. UART is memory-mapped (MMIO at 0xF001_0000), so the
// shim is plain volatile loads/stores — no CPU I/O opcodes. Shared with the
// standalone hello-uart example.

use std::env;
use std::process::Command;

fn main() {
    let out = env::var("OUT_DIR").unwrap();
    // klauss-build exports KLAUSSCPU_LLVM_BIN; otherwise default to the fork's
    // build/bin found relative to this crate (…/llvm/lib/Target/KlaussCPU/rust/
    // crates/examples/blinky → 8 levels up to the repo root).
    let bin = env::var("KLAUSSCPU_LLVM_BIN").unwrap_or_else(|_| {
        let manifest = env::var("CARGO_MANIFEST_DIR").unwrap();
        let repo = std::path::Path::new(&manifest)
            .ancestors()
            .nth(8)
            .expect("repo root above blinky crate");
        repo.join("build/bin").to_string_lossy().into_owned()
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
