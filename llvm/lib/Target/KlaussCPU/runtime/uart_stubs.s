	.file	"uart_stubs.c"
	.text
	.globl	uart_tx_hex                     # -- Begin function uart_tx_hex
	.p2align	2
	.type	uart_tx_hex,@function
uart_tx_hex:                            # @uart_tx_hex
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	txr	r0
	setsp	r15
	pop	r15
	ret
.Lfunc_end0:
	.size	uart_tx_hex, .Lfunc_end0-uart_tx_hex
                                        # -- End function
	.globl	uart_putc                       # -- Begin function uart_putc
	.p2align	2
	.type	uart_putc,@function
uart_putc:                              # @uart_putc
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	setr	r12, _uart_char_buf
	memset8	r0, r12
	txcharmemr	r12
	setsp	r15
	pop	r15
	ret
.Lfunc_end1:
	.size	uart_putc, .Lfunc_end1-uart_putc
                                        # -- End function
	.globl	uart_puts                       # -- Begin function uart_puts
	.p2align	2
	.type	uart_puts,@function
uart_puts:                              # @uart_puts
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	setr	r10, 3
	andr	r9, r0, r10
	cmprv	r9, 0
	setr	r12, 255
	setr	r14, _uart_char_buf
	setr	r13, -8
	setr	r11, 4
	jmpe	.LBB2_6
	jmp	.LBB2_1
.LBB2_1:                                # %if.then
	shlr	r10, r9, r10
	setr	r9, 32
	subr	r10, r9, r10
	setr	r9, -4
	andr	r9, r0, r9
	memget32	r8, r9
.LBB2_2:                                # %for.body
                                        # =>This Inner Loop Header: Depth=1
	addr	r10, r10, r13
	shrr	r2, r8, r10
	andr	r1, r2, r12
	cmprv	r1, 0
	jmpe	.LBB2_5
	jmp	.LBB2_3
.LBB2_3:                                # %if.end
                                        #   in Loop: Header=BB2_2 Depth=1
	memset8	r2, r14
	txcharmemr	r14
	cmprv	r10, 0
	jmpgt	.LBB2_2
	jmp	.LBB2_4
.LBB2_4:                                # %for.end
	addr	r0, r9, r11
.LBB2_5:                                # %cleanup9
	cmprv	r1, 0
	jmpe	.LBB2_12
	jmp	.LBB2_6
.LBB2_6:                                # %while.cond.preheader
	setr	r10, 24
.LBB2_7:                                # %while.cond
                                        # =>This Loop Header: Depth=1
                                        #     Child Loop BB2_8 Depth 2
	memget32	r8, r0
	copy	r1, r10
.LBB2_8:                                # %for.body20
                                        #   Parent Loop BB2_7 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	shrr	r2, r8, r1
	andr	r9, r2, r12
	cmprv	r9, 0
	jmpe	.LBB2_11
	jmp	.LBB2_9
.LBB2_9:                                # %if.end29
                                        #   in Loop: Header=BB2_8 Depth=2
	memset8	r2, r14
	txcharmemr	r14
	addr	r1, r1, r13
	cmprv	r1, -8
	jmpne	.LBB2_8
	jmp	.LBB2_10
.LBB2_10:                               # %for.end35
                                        #   in Loop: Header=BB2_7 Depth=1
	addr	r0, r0, r11
.LBB2_11:                               # %cleanup37
                                        #   in Loop: Header=BB2_7 Depth=1
	cmprv	r9, 0
	jmpne	.LBB2_7
	jmp	.LBB2_12
.LBB2_12:                               # %cleanup41
	setsp	r15
	pop	r15
	ret
.Lfunc_end2:
	.size	uart_puts, .Lfunc_end2-uart_puts
                                        # -- End function
	.globl	uart_newline                    # -- Begin function uart_newline
	.p2align	2
	.type	uart_newline,@function
uart_newline:                           # @uart_newline
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	newline
	setsp	r15
	pop	r15
	ret
.Lfunc_end3:
	.size	uart_newline, .Lfunc_end3-uart_newline
                                        # -- End function
	.globl	uart_getc_blocking              # -- Begin function uart_getc_blocking
	.p2align	2
	.type	uart_getc_blocking,@function
uart_getc_blocking:                     # @uart_getc_blocking
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	rxrb	r12
	setsp	r15
	pop	r15
	ret
.Lfunc_end4:
	.size	uart_getc_blocking, .Lfunc_end4-uart_getc_blocking
                                        # -- End function
	.globl	uart_getc_nonblocking           # -- Begin function uart_getc_nonblocking
	.p2align	2
	.type	uart_getc_nonblocking,@function
uart_getc_nonblocking:                  # @uart_getc_nonblocking
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	rxrnb	r12
	setsp	r15
	pop	r15
	ret
.Lfunc_end5:
	.size	uart_getc_nonblocking, .Lfunc_end5-uart_getc_nonblocking
                                        # -- End function
	.globl	uart_println                    # -- Begin function uart_println
	.p2align	2
	.type	uart_println,@function
uart_println:                           # @uart_println
# %bb.0:                                # %entry
	push	r15
	getsp	r15
	setr	r10, 3
	andr	r9, r0, r10
	cmprv	r9, 0
	setr	r12, 255
	setr	r14, _uart_char_buf
	setr	r13, -8
	setr	r11, 4
	jmpe	.LBB6_6
	jmp	.LBB6_1
.LBB6_1:                                # %if.then.i
	shlr	r10, r9, r10
	setr	r9, 32
	subr	r10, r9, r10
	setr	r9, -4
	andr	r9, r0, r9
	memget32	r8, r9
.LBB6_2:                                # %for.body.i
                                        # =>This Inner Loop Header: Depth=1
	addr	r10, r10, r13
	shrr	r2, r8, r10
	andr	r1, r2, r12
	cmprv	r1, 0
	jmpe	.LBB6_5
	jmp	.LBB6_3
.LBB6_3:                                # %if.end.i
                                        #   in Loop: Header=BB6_2 Depth=1
	memset8	r2, r14
	txcharmemr	r14
	cmprv	r10, 0
	jmpgt	.LBB6_2
	jmp	.LBB6_4
.LBB6_4:                                # %for.end.i
	addr	r0, r9, r11
.LBB6_5:                                # %cleanup9.i
	cmprv	r1, 0
	jmpe	.LBB6_12
	jmp	.LBB6_6
.LBB6_6:                                # %while.cond.i.preheader
	setr	r10, 24
.LBB6_7:                                # %while.cond.i
                                        # =>This Loop Header: Depth=1
                                        #     Child Loop BB6_8 Depth 2
	memget32	r8, r0
	copy	r1, r10
.LBB6_8:                                # %for.body20.i
                                        #   Parent Loop BB6_7 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	shrr	r2, r8, r1
	andr	r9, r2, r12
	cmprv	r9, 0
	jmpe	.LBB6_11
	jmp	.LBB6_9
.LBB6_9:                                # %if.end29.i
                                        #   in Loop: Header=BB6_8 Depth=2
	memset8	r2, r14
	txcharmemr	r14
	addr	r1, r1, r13
	cmprv	r1, -8
	jmpne	.LBB6_8
	jmp	.LBB6_10
.LBB6_10:                               # %for.end35.i
                                        #   in Loop: Header=BB6_7 Depth=1
	addr	r0, r0, r11
.LBB6_11:                               # %cleanup37.i
                                        #   in Loop: Header=BB6_7 Depth=1
	cmprv	r9, 0
	jmpne	.LBB6_7
	jmp	.LBB6_12
.LBB6_12:                               # %uart_puts.exit
	newline
	setsp	r15
	pop	r15
	ret
.Lfunc_end6:
	.size	uart_println, .Lfunc_end6-uart_println
                                        # -- End function
	.type	_uart_char_buf,@object          # @_uart_char_buf
	.local	_uart_char_buf
	.comm	_uart_char_buf,1,1
	.ident	"clang version 23.0.0git (https://github.com/grahamjonesgs/llvm-project.git a24b90ad892ba73353dc013cf47bd68eac7f286e)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym _uart_char_buf
