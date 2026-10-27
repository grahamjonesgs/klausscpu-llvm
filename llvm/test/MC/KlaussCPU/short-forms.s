# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s \
# RUN:   | FileCheck %s --check-prefixes=SHIMM,SHBR
# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -klausscpu-short-branches=false %s | FileCheck %s --check-prefixes=SHIMM,LONGBR
# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -mattr=-short-branches %s | FileCheck %s --check-prefixes=SHIMM,LONGBR
# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -mattr=-short-forms %s | FileCheck %s --check-prefixes=LONGIMM,LONGBR
# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -klausscpu-short-forms=false -klausscpu-short-branches=true %s \
# RUN:   | FileCheck %s --check-prefixes=LONGIMM,LONGBR
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj --defsym=RELAX=1 %s -o %t.o
# RUN: llvm-objdump -dr %t.o | FileCheck %s --check-prefix=RELAX

# The short-forms / short-branches subtarget features gate the CompressPats
# (KlaussCPUInstrInfo.td); -klausscpu-short-forms / -klausscpu-short-branches
# override them, and turning the short forms off turns the branches off too.

.ifndef RELAX
# SHIMM:   setr r1, 5 # encoding: [0x00,0x51,0xd0,0x4b]
# LONGIMM: setr r1, 5 # encoding: [0x00,0x01,0xd0,0x8b,0x05,0x00,0x00,0x00]
setr r1, 5
# SHIMM:   addv r1, r2, 200 # encoding: [0x20,0x81,0x2c,0x48]
# LONGIMM: addv r1, r2, 200 # encoding: [0x20,0x01,0x20,0x88,0xc8,0x00,0x00,0x00]
addv r1, r2, 200
# SHIMM:   ldidx16 r1, r2, -256 # encoding: [0x20,0x01,0x28,0x59]
# LONGIMM: ldidx16 r1, r2, -256 # encoding: [0x20,0x01,0x20,0x99,0x00,0xff,0xff,0xff]
ldidx16 r1, r2, -256

# A branch to a symbol starts short: the absolute and the PC-relative
# spelling alike.  JMPO has no PC-relative 2-word form and prints as jmpo.
# SHBR:   jmpzrel foo # encoding: [A,A,0b000010AA,0x61]
# SHBR:   fixup A - offset: 0, value: foo, kind: FK_KlaussCPU_PCREL18
# LONGBR: jmpz foo # encoding: [0x00,0x00,0x08,0xa0,A,A,A,A]
jmpz foo
# SHBR:   jmprel foo # encoding: [A,A,0b000000AA,0x61]
# LONGBR: jmprel foo # encoding: [0x00,0x00,0x00,0xa1,A,A,A,A]
jmprel foo
# SHBR:   jmpo foo # encoding: [A,A,0b000110AA,0x61]
# LONGBR: jmpo foo # encoding: [0x00,0x00,0x18,0xa0,A,A,A,A]
jmpo foo
foo:
.else

# The asm backend keeps a short branch whose target is local and within
# simm18 words, and relaxes the rest to the 2-word PC-relative form (JMPO:
# the absolute one).
# RELAX:      0: 00 00 08 61 jmpzrel 0x0 <near>
# RELAX-NEXT: 4: ff ff 1b 61 jmpo 0x0 <near>
# RELAX-NEXT: 8: 00 00 08 a1 d8 27 09 00 jmpzrel 0x927e0 <far>
# RELAX-NEXT: 10: 00 00 18 a0 00 00 00 00 jmpo
# RELAX-NEXT: R_KLAUSSCPU_ABS32 .text+0x927e0
# RELAX-NEXT: 18: 00 00 00 a1 00 00 00 00 jmprel
# RELAX-NEXT: R_KLAUSSCPU_PCREL32 ext
near:
  jmpz near
  jmpo near
  jmpz far
  jmpo far
  jmp ext
  .space 600000
far:
  nop
.endif
