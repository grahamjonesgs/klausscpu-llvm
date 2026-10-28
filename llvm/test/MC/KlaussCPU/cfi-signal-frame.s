# An interrupt handler's .cfi_signal_frame survives in .debug_frame (KlaussCPU
# emits no .eh_frame): its FDE gets a CIE of its own with the augmentation
# "S", as GNU as writes it, so a debugger unwinds through the handler into
# the interrupted code at its exact PC.  The other FDEs share the plain CIE.
# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-dwarfdump --debug-frame %t.o | FileCheck %s

# CHECK:      00000000 {{[0-9a-f]+}} ffffffff CIE
# CHECK:        Augmentation: ""
# CHECK:      FDE cie=00000000 pc=00000000...00000004
# CHECK:      FDE cie=00000000 pc=00000014...00000018
# CHECK:      [[S:[0-9a-f]+]] {{[0-9a-f]+}} ffffffff CIE
# CHECK:        Augmentation: "S"
# CHECK:      FDE cie=[[S]] pc=00000004...00000014
# CHECK-NOT:  CIE

  .cfi_sections .debug_frame
  .text
leaf:
  .cfi_startproc
  ret
  .cfi_endproc

isr:
  .cfi_startproc
  .cfi_signal_frame
  push r0
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset r0, 0
  call leaf
  pop r0
  .cfi_adjust_cfa_offset -8
  iret
  .cfi_endproc

leaf2:
  .cfi_startproc
  ret
  .cfi_endproc
