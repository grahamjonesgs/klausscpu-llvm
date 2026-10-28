// The KlaussCPU driver compiles hosted unless -ffreestanding is given, adds
// only clang's own headers, and links with ld.lld -m elf32klausscpu.

// RUN: %clang -### --target=klausscpu-unknown-elf -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=HOSTED
// HOSTED: "-cc1" "-triple" "klausscpu-unknown-unknown-elf"
// HOSTED-NOT: "-ffreestanding"
// HOSTED-NOT: "-fno-builtin"

// RUN: %clang -### --target=klausscpu-unknown-elf -ffreestanding -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FREE
// FREE: "-cc1" "-triple" "klausscpu-unknown-unknown-elf"
// FREE-SAME: "-ffreestanding"

// RUN: %clang -### --target=klausscpu-unknown-elf %s -o %t.elf 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LINK
// LINK: ld.lld{{(.exe)?}}" "-m" "elf32klausscpu" "-o" "{{.*}}.elf"

// Frame pointers: every function that calls one has a frame (R15 chain);
// a leaf without stack objects has none unless asked for.  R15 stays
// reserved either way.
// RUN: %clang -### --target=klausscpu-unknown-elf -O1 -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FP-NONLEAF
// RUN: %clang -### --target=klausscpu-unknown-elf -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FP-NONLEAF
// RUN: %clang -### --target=klausscpu-unknown-elf -O1 -fno-omit-frame-pointer -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FP-NONLEAF
// RUN: %clang -### --target=klausscpu-unknown-elf -O1 -mno-omit-leaf-frame-pointer -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FP-ALL
// RUN: %clang -### --target=klausscpu-unknown-elf -O1 -fomit-frame-pointer -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FP-RESERVED
// FP-NONLEAF: "-mframe-pointer=non-leaf"
// FP-ALL: "-mframe-pointer=all"
// FP-RESERVED: "-mframe-pointer=reserved"
