# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o 2>&1 | count 0
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s
#
# A taken fused branch (FBR) redirects from MEM and squashes the instruction
# behind it in EX.  Since klaussCPU 9ad6614, the oldest core the backend
# supports, that squash also resets the divider and gates the LCD strobe, so
# a divide or an LCD write may directly follow an fbr/fbri.  The assembler
# used to warn about it (on the bitstreams before 9ad6614 the wrong-path
# divide ran on and the LCD byte went out); it no longer does, and keeps
# the two adjacent.

# CHECK:      fbr r1, r2, 0,
# CHECK-NEXT: divr r3, r4, r5
# CHECK-NEXT: fbri r1, 3, 1,
# CHECK-NEXT: lcddatav 65
# CHECK-NEXT: fbr r1, r2, 3,
# CHECK-NEXT: modur r3, r4, r5
# CHECK-NEXT: fbr r1, r2, 3,
# CHECK-NEXT: lcddatar r3
# CHECK-NEXT: fbri r1, -2, 4,
# CHECK-NEXT: lcdcmdv 1
# CHECK-NEXT: fbr r1, r2, 0,
# CHECK-NEXT: divur r3, r4, r5
# CHECK-NEXT: fbri r1, 0, 8,
# CHECK-NEXT: modv r3, r3, 7
# CHECK-NEXT: fbri r1, 0, 8,
# CHECK-NEXT: divv r3, r3, 7
# CHECK-NEXT: fbr r1, r2, 0,
# CHECK-NEXT: modr r3, r4, r5
# CHECK-NEXT: ret
	fbr	r1, r2, 0, .L1
	divr	r3, r4, r5
	fbri	r1, 3, 1, .L1
.L1:
	lcddatav	65
	fbr	r1, r2, 3, .L2
	modur	r3, r4, r5
	fbr	r1, r2, 3, .L2
	lcddatar	r3
	fbri	r1, -2, 4, .L2
	lcdcmdv	1
	fbr	r1, r2, 0, .L2
	divur	r3, r4, r5
	fbri	r1, 0, 8, .L2
	modv	r3, r3, 7
	fbri	r1, 0, 8, .L2
	divv	r3, r3, 7
	fbr	r1, r2, 0, .L2
	modr	r3, r4, r5
.L2:
	ret
