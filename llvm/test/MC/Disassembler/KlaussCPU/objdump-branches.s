# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s

# Branch targets print as addresses and are symbolized by llvm-objdump
# (KlaussCPUMCInstrAnalysis::evaluateBranch): short A1 branches, fused
# compare-and-branch, the 2-word PC-relative form and backward branches.

# CHECK-LABEL: <start>:
# CHECK-NEXT:  0: jmpzrel 0x10 <target>
# CHECK-NEXT:  4: fbr r1, r2, 5, 0x10 <target>
# CHECK-NEXT:  8: fbri r1, -3, 2, 0x10 <target>
# CHECK-NEXT:  c: nop
# CHECK-LABEL: <target>:
# CHECK-NEXT: 10: jmprel 0x0 <start>
# CHECK-NEXT: 14: jmpo 0x0 <start>
# CHECK-NEXT: 18: trap
        .text
start:
        jmpzrel target
        fbr     r1, r2, 5, target
        fbri    r1, -3, 2, target
        nop
target:
        jmprel  start
        jmpo    start
        trap
