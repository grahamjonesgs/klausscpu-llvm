// klausscpu-*-linux-musl: user programs for the KlaussCPU Linux port.  The
// KlaussCPU toolchain adds the sysroot's headers and links statically with
// crt1.o, -lc and compiler-rt's builtins, 64 KiB pages, no PIE.

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

// RUN: %clang -### --target=klausscpu-unknown-linux-musl --sysroot=%S/Inputs/klausscpu_linux_tree \
// RUN:   -nostdlib %s -o %t.elf 2>&1 | FileCheck %s --check-prefix=NOSTD
// NOSTD-NOT: crt1.o
// NOSTD-NOT: "-lc"

// Bare metal is unchanged: no sysroot, no crt1.o.
// RUN: %clang -### --target=klausscpu-unknown-elf %s -o %t.elf 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BARE
// BARE-NOT: crt1.o
// BARE-NOT: "-lc"
