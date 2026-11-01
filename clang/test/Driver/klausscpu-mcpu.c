// -mcpu selects a backend processor model: "generic" (the default, with the
// M8 scheduling model) or "no-sched" (the A/B baseline without it).  The
// driver passes it to cc1 as -target-cpu and the front end records it as the
// function attribute the backend reads.

// RUN: %clang -### --target=klausscpu-unknown-elf -mcpu=no-sched -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=DRIVER
// DRIVER: "-cc1" "-triple" "klausscpu-unknown-unknown-elf"
// DRIVER-SAME: "-target-cpu" "no-sched"

// RUN: %clang --target=klausscpu-unknown-elf -mcpu=no-sched -S -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang --target=klausscpu-unknown-elf -mcpu=generic -S -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefix=IRGEN
// IR: "target-cpu"="no-sched"
// IRGEN: "target-cpu"="generic"

// RUN: not %clang --target=klausscpu-unknown-elf -mcpu=bogus -c %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BAD
// BAD: error: unknown target CPU 'bogus'
// BAD: note: valid target CPU values are: generic, no-sched

int f(int x) { return x + 1; }
