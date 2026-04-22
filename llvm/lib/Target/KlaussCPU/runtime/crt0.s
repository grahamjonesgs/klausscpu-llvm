	.file	"crt0.c"
	.text
	.globl	_start                          # -- Begin function _start
	.p2align	2
	.type	_start,@function
_start:                                 # @_start
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	addsp	-32
	setr	r12, __bss_end
	setr	r14, __bss_start
	cmprr	r14, r12
	jmpe	.LBB0_2
	jmp	.LBB0_1
.LBB0_1:                                # %for.body.preheader
	setr	r13, 0
	setr	r11, 1
	jmp	.LBB0_3
.LBB0_3:                                # %for.body
                                        # =>This Inner Loop Header: Depth=1
	memset8	r13, r14
	addr	r14, r14, r11
	cmprr	r14, r12
	jmpe	.LBB0_2
	jmp	.LBB0_3
.LBB0_2:                                # %for.cond.cleanup
	setr	r0, 0
	copy	r1, r0
	call	main
	jmp	.LBB0_4
.LBB0_4:                                # %while.body
                                        # =>This Inner Loop Header: Depth=1
	jmp	.LBB0_4
.Lfunc_end0:
	.size	_start, .Lfunc_end0-_start
                                        # -- End function
	.ident	"clang version 23.0.0git (https://github.com/grahamjonesgs/llvm-project.git 8ca394830a64f7fa315998427a27fdaa3abfdfe0)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym _start
	.addrsig_sym __bss_start
	.addrsig_sym __bss_end
