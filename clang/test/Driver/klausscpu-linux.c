// klausscpu-*-linux-musl: user programs for the KlaussCPU Linux port.  The
// KlaussCPU toolchain adds the sysroot's headers and links statically by
// default with crt1.o, -lc and compiler-rt's builtins, 64 KiB pages, no PIE;
// -shared and -pie link for musl's dynamic linker (ABI.md "Driver").

// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -c %s 2>&1 | FileCheck %s --check-prefix=CC1
// CC1: "-cc1" "-triple" "klausscpu-unknown-linux-musl"
// CC1-NOT: "-pic-is-pie"
// CC1: "-internal-externc-isystem" "{{.*}}klausscpu_linux_tree{{/|\\\\}}include"

// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=LINK
// LINK: ld.lld{{(.exe)?}}" "-m" "elf32klausscpu" "-o" "{{.*}}.elf" "-static" "-z" "max-page-size=0x10000"
// LINK-SAME: "-L{{.*}}klausscpu_linux_tree{{/|\\\\}}lib"
// LINK-SAME: "{{.*}}klausscpu_linux_tree{{/|\\\\}}lib{{/|\\\\}}crt1.o"
// LINK-SAME: "--start-group" "-lc" "{{.*}}libclang_rt.builtins.a" "--end-group"
// LINK-NOT: "-pie"
// LINK-NOT: "-shared"
// LINK-NOT: "-dynamic-linker"
// LINK-NOT: "now"
// LINK-NOT: "--hash-style
// LINK-NOT: "--eh-frame-hdr"
// LINK-NOT: "common-page-size

// An explicit -static is the same link, and wins over -pie; -rdynamic does
// nothing in a static executable.
// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -static -pie -rdynamic %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=STATIC
// STATIC-NOT: warning
// STATIC: ld.lld{{(.exe)?}}" "-m" "elf32klausscpu" "-o" "{{.*}}.elf" "-static" "-z" "max-page-size=0x10000" "-L
// STATIC-NOT: "-static"
// STATIC-NOT: "-pie"
// STATIC-NOT: "--export-dynamic"
// STATIC-SAME: crt1.o"
// STATIC-NOT: Scrt1.o

// -pie: a PIE for /lib/ld-musl-klausscpu.so.1 with Scrt1.o, bound at load,
// both hash tables, .eh_frame_hdr, 64 KiB pages.  The compiler still needs
// -fPIE: -pie is a link flag.
// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -pie -rdynamic %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=PIE
// PIE: "-cc1"
// PIE-NOT: "-pic-is-pie"
// PIE: ld.lld{{(.exe)?}}" "-m" "elf32klausscpu" "-o" "{{.*}}.elf" "-pie"
// PIE-SAME: "-dynamic-linker" "/lib/ld-musl-klausscpu.so.1" "-z" "now" "--hash-style=both" "--eh-frame-hdr" "--export-dynamic"
// PIE-SAME: "-z" "max-page-size=0x10000" "-z" "common-page-size=0x10000"
// PIE-SAME: "-L{{.*}}klausscpu_linux_tree{{/|\\\\}}lib"
// PIE-SAME: "{{.*}}klausscpu_linux_tree{{/|\\\\}}lib{{/|\\\\}}Scrt1.o"
// PIE-SAME: "--start-group" "-lc" "{{.*}}klausscpu-unknown-linux-musl{{/|\\\\}}libclang_rt.builtins.a" "--end-group"
// PIE-NOT: "-static"
// PIE-NOT: "{{[^"]*}}{{/|\\\\}}crt1.o"

// -fPIE -pie: the compiler side.
// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -fPIE -pie %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=FPIE
// FPIE: "-cc1" {{.*}}"-pic-level" "2" "-pic-is-pie"
// FPIE: "-pie"

// -shared: a shared library, no start file, no interpreter.
// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -fPIC -shared %s -o %t.so 2>&1 | FileCheck %s --check-prefix=SHARED
// SHARED: ld.lld{{(.exe)?}}" "-m" "elf32klausscpu" "-o" "{{.*}}.so" "-shared"
// SHARED-SAME: "-z" "now" "--hash-style=both" "--eh-frame-hdr"
// SHARED-SAME: "-z" "max-page-size=0x10000" "-z" "common-page-size=0x10000"
// SHARED-SAME: "--start-group" "-lc" "{{.*}}klausscpu-unknown-linux-musl{{/|\\\\}}libclang_rt.builtins.a" "--end-group"
// SHARED-NOT: "-static"
// SHARED-NOT: "-dynamic-linker"
// SHARED-NOT: "--export-dynamic"
// SHARED-NOT: crt1.o

// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -nostdlib %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=NOSTD
// NOSTD-NOT: crt1.o
// NOSTD-NOT: "-lc"

// Bare metal is unchanged: no sysroot, no crt1.o.
// RUN: %clang -### --target=klausscpu-unknown-elf %s -o %t.elf 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BARE
// BARE-NOT: crt1.o
// BARE-NOT: "-lc"
