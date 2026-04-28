// uart_stubs.c — UART I/O implementation using KlaussCPU hardware instructions.
//
// Each function maps to a single hardware instruction via the LLVM intrinsic
// mechanism.  No memory-mapped I/O addresses are used; the hardware instruction
// set handles all UART state internally.
//
// Compile with:
//   clang -target klausscpu-unknown-elf -O1 -nostdlib -nostdinc \
//         -ffreestanding -c uart_stubs.c

typedef unsigned long long uint64_t;

// ---------------------------------------------------------------------------
// Transmit: send 64-bit register value as 16 hex digits over UART.
// ---------------------------------------------------------------------------
void uart_tx_hex(uint64_t val) {
    __builtin_klausscpu_txr(val);
}

// Scratch buffer for single-character UART transmission.
// Must be global so TXCHARMEMR gets a SETR-resolved address, not a
// FrameIndex that eliminateFrameIndex cannot handle for R-format instructions.
static volatile char _uart_char_buf;

// ---------------------------------------------------------------------------
// Transmit: send a single character (byte) over UART.
// ---------------------------------------------------------------------------
void uart_putc(char c) {
    _uart_char_buf = c;
    __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
}

// ---------------------------------------------------------------------------
// Transmit: send null-terminated string over UART.
//
// Physical memory is big-endian: the first (lowest-address) byte of a string
// is stored at bits[31:24] of the 32-bit physical word.  MEMGET32 returns
// this raw word, so extracting bytes with decreasing shift (24, 16, 8, 0)
// gives the bytes in correct sequential order.
//
// TXSTRMEMR and LDIDX8 both use LE-lane mapping (after April 2026 HW fix):
// they scan from bits[7:0] first, which is the LAST byte in a big-endian word.
// Using either of those here would produce reversed 4-byte groups.
// MEMGET32 is the only instruction that correctly reads big-endian .rodata.
//
// Unaligned handling: .rodata strings are NOT guaranteed to be 4-byte aligned.
// MEMGET32 reads from the 4-byte aligned address containing the pointer.  When
// the string starts at byte offset N within a 32-bit word (N = addr & 3), we
// start extraction at shift = 24 - 8*N to skip the bytes before the string,
// then continue with fully-aligned 4-byte reads.
// ---------------------------------------------------------------------------
void uart_puts(const char *s) {
    const char *p = s;

    // Handle misaligned start.
    uint64_t misalign = (uint64_t)p & 3;
    if (misalign != 0) {
        const char *aligned = (const char *)((uint64_t)p & ~(uint64_t)3);
        uint64_t word = __builtin_klausscpu_memget32((const void *)aligned);
        int shift;
        for (shift = 24 - 8 * (int)misalign; shift >= 0; shift -= 8) {
            char c = (char)((word >> shift) & 0xFF);
            if (c == '\0') return;
            _uart_char_buf = c;
            __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
        }
        p = aligned + 4;
    }

    // Aligned loop: read 4 bytes at a time.
    while (1) {
        uint64_t word = __builtin_klausscpu_memget32((const void *)p);
        int shift;
        for (shift = 24; shift >= 0; shift -= 8) {
            char c = (char)((word >> shift) & 0xFF);
            if (c == '\0') return;
            _uart_char_buf = c;
            __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
        }
        p += 4;
    }
}

// ---------------------------------------------------------------------------
// Transmit: CR+LF.
// ---------------------------------------------------------------------------
void uart_newline(void) {
    __builtin_klausscpu_newline();
}

// ---------------------------------------------------------------------------
// Receive: blocking — wait for UART byte, return it.
// ---------------------------------------------------------------------------
uint64_t uart_getc_blocking(void) {
    return __builtin_klausscpu_rxrb();
}

// ---------------------------------------------------------------------------
// Receive: non-blocking.
// WARNING: RXRNB sets zero_flag when the FIFO is empty but does NOT write the
// destination register; the returned value is undefined when the FIFO is
// empty.  The hardware zero_flag cannot be inspected from C without inline
// assembly.  Use uart_getc_blocking() for reliable receive.  This function is
// provided for completeness; callers that need the empty-sentinel (-1) must
// use the rcc-assembled uart_stubs.kla version instead.
// ---------------------------------------------------------------------------
uint64_t uart_getc_nonblocking(void) {
    return __builtin_klausscpu_rxrnb();
}

// ---------------------------------------------------------------------------
// Convenience: puts() + newline (matches libc puts() semantics roughly).
// ---------------------------------------------------------------------------
void uart_println(const char *s) {
    uart_puts(s);
    __builtin_klausscpu_newline();
}
