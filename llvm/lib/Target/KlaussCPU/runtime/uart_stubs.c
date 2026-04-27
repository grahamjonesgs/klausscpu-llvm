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
typedef unsigned int       uint32_t;

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
// MEMGET8 (byte load) uses scrambled addressing: byte at logical address A
// is read from physical address (A & ~3) | (3 - (A & 3)), reversing bytes
// within every 4-byte word.  MEMGET32 (32-bit LE load) is consistent: byte
// at the lowest word address lands in bits[7:0] of the returned value.
// So we read 4 chars at a time as a uint32_t and extract bytes via right-
// shift, which gives the correct sequential byte order.
// ---------------------------------------------------------------------------
void uart_puts(const char *s) {
    const uint32_t *w = (const uint32_t *)s;
    while (1) {
        uint32_t word = *w++;
        int shift;
        for (shift = 24; shift >= 0; shift -= 8) {
            char c = (char)((word >> shift) & 0xFF);
            if (c == '\0') return;
            _uart_char_buf = c;
            __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
        }
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
    const uint32_t *w = (const uint32_t *)s;
    while (1) {
        uint32_t word = *w++;
        int shift;
        for (shift = 24; shift >= 0; shift -= 8) {
            char c = (char)((word >> shift) & 0xFF);
            if (c == '\0') goto done;
            _uart_char_buf = c;
            __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
        }
    }
done:
    __builtin_klausscpu_newline();
}
