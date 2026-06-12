/* uart_shim.c — minimal UART TX for the Rust hello (self-contained subset of
 * klausscpu-runtime/src/uart_stubs.c, without the loader console-redirect).
 *
 * Buffer must be a global so TXCHARMEMR gets a SETR-resolved address, not a
 * FrameIndex that eliminateFrameIndex cannot handle for R-format instructions.
 */

static volatile char _uart_char_buf;

static void _uart_raw(char c) {
    _uart_char_buf = c;
    __builtin_klausscpu_txcharmemr((const void *)&_uart_char_buf);
}

/* '\n' expands to CR+LF so output looks right on a serial terminal. */
void uart_putc(char c) {
    if (c == '\n')
        _uart_raw('\r');
    _uart_raw(c);
}
