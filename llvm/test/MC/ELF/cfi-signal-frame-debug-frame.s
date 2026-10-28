// A .cfi_signal_frame in .debug_frame: like GNU as, mark its CIE with the
// augmentation "S" (no 'z', so no augmentation data follows), and give it a
// CIE of its own instead of sharing the plain one.
// RUN: llvm-mc -filetype=obj -triple x86_64-pc-linux-gnu %s -o - | llvm-dwarfdump --debug-frame - | FileCheck %s

// CHECK:      00000000 {{[0-9a-f]+}} ffffffff CIE
// CHECK:        Augmentation: ""
// CHECK:      FDE cie=00000000 pc=00000001...00000002
// CHECK:      [[S:[0-9a-f]+]] {{[0-9a-f]+}} ffffffff CIE
// CHECK:        Augmentation: "S"
// CHECK:      DW_CFA_def_cfa: RSP +8
// CHECK:      FDE cie=[[S]] pc=00000000...00000001

        .cfi_sections .debug_frame
f:
        .cfi_startproc
        .cfi_signal_frame
        nop
        .cfi_endproc

g:
        .cfi_startproc
        nop
        .cfi_endproc
