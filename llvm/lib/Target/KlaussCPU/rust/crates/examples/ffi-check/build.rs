// Compiles the C half of the check (src/ffi.c) and the UART shim shared with
// blinky, with the fork's clang (as blinky's build.rs does).

use std::env;
use std::process::Command;

fn main() {
    let out = env::var("OUT_DIR").unwrap();
    let bin = env::var("KLAUSSCPU_LLVM_BIN").unwrap_or_else(|_| {
        let manifest = env::var("CARGO_MANIFEST_DIR").unwrap();
        let repo = std::path::Path::new(&manifest)
            .ancestors()
            .nth(8)
            .expect("repo root above the crate");
        repo.join("build/bin").to_string_lossy().into_owned()
    });
    // FFI_CHECK_COPT picks the C optimisation level (default -O1).
    let copt = env::var("FFI_CHECK_COPT").unwrap_or_else(|_| "-O1".into());

    let mut objs = Vec::new();
    for (src, obj) in [
        ("src/ffi.c", "ffi.o"),
        ("../blinky/src/uart_shim.c", "uart_shim.o"),
    ] {
        let o = format!("{out}/{obj}");
        let ok = Command::new(format!("{bin}/clang"))
            .args([
                "-target",
                "klausscpu-unknown-elf",
                &copt,
                "-ffreestanding",
                "-c",
                src,
                "-o",
                &o,
            ])
            .status()
            .expect("run the fork's clang (set KLAUSSCPU_LLVM_BIN)")
            .success();
        assert!(ok, "{src} failed to compile");
        objs.push(o);
    }
    let ok = Command::new(format!("{bin}/llvm-ar"))
        .arg("rcs")
        .arg(format!("{out}/libffi_check_c.a"))
        .args(&objs)
        .status()
        .expect("run llvm-ar")
        .success();
    assert!(ok, "llvm-ar failed");

    println!("cargo:rustc-link-search=native={out}");
    println!("cargo:rustc-link-lib=static=ffi_check_c");
    println!("cargo:rerun-if-changed=src/ffi.c");
    println!("cargo:rerun-if-changed=../blinky/src/uart_shim.c");
    println!("cargo:rerun-if-env-changed=KLAUSSCPU_LLVM_BIN");
    println!("cargo:rerun-if-env-changed=FFI_CHECK_COPT");
}
