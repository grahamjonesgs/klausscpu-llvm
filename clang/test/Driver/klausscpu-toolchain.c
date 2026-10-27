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
