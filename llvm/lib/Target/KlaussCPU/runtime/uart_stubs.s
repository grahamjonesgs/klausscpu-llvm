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
	txstrmemr	r0
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
	txstrmemr	r0
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
	.ident	"clang version 23.0.0git (https://github.com/grahamjonesgs/llvm-project.git aeb7c1046b473026044f939f1b1ca0b7be686493)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.addrsig_sym _uart_char_buf
