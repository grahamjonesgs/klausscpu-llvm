// uart_stubs.c — UART I/O implementation using KlaussCPU hardware instructions.
//
// Physical memory is little-endian (after April 2026 CPU fix): byte at address
// X occupies bits[8*(X mod 4)+7 : 8*(X mod 4)] of the 32-bit physical word.
// LDIDX8/STIDX8, MEMGET8/MEMSET8, and TXSTRMEMR all use LE-lane mapping and
// are therefore consistent with the physical memory layout.
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
// With LE physical memory, TXSTRMEMR transmits bytes in address order
// (lowest address first), which is correct for C strings.
// ---------------------------------------------------------------------------
void uart_puts(const char *s) {
    __builtin_klausscpu_txstrmemr((const void *)s);
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
// assembly.  Use uart_getc_blocking() for reliable receive.
// ---------------------------------------------------------------------------
uint64_t uart_getc_nonblocking(void) {
    return __builtin_klausscpu_rxrnb();
}

// ---------------------------------------------------------------------------
// Convenience: puts() + newline.
// ---------------------------------------------------------------------------
void uart_println(const char *s) {
    __builtin_klausscpu_txstrmemr((const void *)s);
    __builtin_klausscpu_newline();
}
