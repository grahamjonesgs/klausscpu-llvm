# RUN: llvm-mc -triple=klausscpu %s -o /dev/null 2>&1 | FileCheck %s --implicit-check-not=warning:
#
# A taken fused branch (FBR) redirects from MEM and squashes the instruction
# behind it in EX, but a divide has already started the divider there and an
# LCD write has strobed the LCD (pipeline_core.sv).  KlaussCPUFuseCmpBr never
# fuses a branch into that position; the assembler warns when hand-written
# assembly does (the ActsInEXShadow flag of KlaussCPUInstrInfo.td).

# CHECK: :[[#@LINE+3]]:2: warning: a divide or LCD write directly behind a fused branch
# CHECK: :[[#@LINE+1]]:2: note: fused branch here
	fbr	r1, r2, 0, .L1
	divr	r3, r4, r5

# A label does not separate them: the divide is still the next instruction.
# CHECK: :[[#@LINE+4]]:2: warning: a divide or LCD write directly behind a fused branch
# CHECK: :[[#@LINE+1]]:2: note: fused branch here
	fbri	r1, 3, 1, .L1
.L1:
	lcddatav	65

# CHECK: :[[#@LINE+2]]:2: warning: a divide or LCD write
	fbr	r1, r2, 3, .L2
	modur	r3, r4, r5
# CHECK: :[[#@LINE+2]]:2: warning: a divide or LCD write
	fbr	r1, r2, 3, .L2
	lcddatar	r3
# CHECK: :[[#@LINE+2]]:2: warning: a divide or LCD write
	fbri	r1, -2, 4, .L2
	lcdcmdv	1

# No warning: another instruction between them, a directive (it may emit
# padding), the LCD reset (gated by the squash), or no fused branch.
	fbr	r1, r2, 0, .L2
	addr	r3, r3, r3
	divr	r3, r4, r5
	fbr	r1, r2, 0, .L2
	.p2align 4
	modv	r3, r3, 7
	fbr	r1, r2, 0, .L2
	lcd	1
	cmprr	r1, r2
	jmperel	.L2
	divur	r3, r4, r5
.L2:
	ret
