// The MachineOutliner runs by default on minsize (-Oz) functions; -moutline
// turns it on everywhere, -mno-outline off.
// RUN: %clang --target=klausscpu-unknown-elf -moutline -S %s -### 2>&1 | FileCheck %s -check-prefix=ON
// ON-NOT: warning:
// ON: "-mllvm" "-enable-machine-outliner"

// RUN: %clang --target=klausscpu-unknown-elf -moutline -mno-outline -S %s -### 2>&1 | FileCheck %s -check-prefix=OFF
// OFF: "-mno-outline" "-mllvm" "-enable-machine-outliner=never"
