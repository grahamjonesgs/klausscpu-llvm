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

/* Defined in syscalls.c — set by crt0_loadable before main() to redirect
 * console I/O to an active SSH/telnet session when running under the loader. */
extern void (*g_console_mirror_fn)(char c);
extern int  (*g_console_input_fn)(void);

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
// Raw single-byte send — no CR conversion.
// ---------------------------------------------------------------------------
static void _uart_raw(char c) {
    _uart_char_buf = c;
    __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
}

// ---------------------------------------------------------------------------
// Transmit: send a single character (byte) over UART.
// '\n' is expanded to CR+LF so all output looks correct on a serial terminal.
// ---------------------------------------------------------------------------
void uart_putc(char c) {
    /* When a remote loader (SSH/telnet) is driving this program, redirect
     * output to that session only — not the physical UART.  The mirror
     * function does its own \n→\r\n expansion, so pass c unchanged. */
    if (g_console_mirror_fn) {
        g_console_mirror_fn(c);
        return;
    }
    if (c == '\n') _uart_raw('\r');
    _uart_raw(c);
}

// ---------------------------------------------------------------------------
// Transmit: send null-terminated string over UART (with CR+LF conversion).
// ---------------------------------------------------------------------------
void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

// ---------------------------------------------------------------------------
// Transmit: CR+LF.
// ---------------------------------------------------------------------------
void uart_newline(void) {
    uart_putc('\n');   /* CR+LF to UART + mirror if active */
}

// ---------------------------------------------------------------------------
// Receive: blocking — wait for UART byte, return it.
// ---------------------------------------------------------------------------
uint64_t uart_getc_blocking(void) {
    /* When a remote loader (SSH/telnet) is driving this program, read input
     * from that session instead of the physical UART RX FIFO. */
    if (g_console_input_fn)
        return (uint64_t)(unsigned char)g_console_input_fn();
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
    uart_puts(s);
    uart_putc('\n');
}
