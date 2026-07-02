/* uart_shim.c — minimal UART TX for the Rust hello, using the KlaussCPU MMIO
 * UART at 0xF001_0000 (self-contained subset of klausscpu-runtime's mmio.h /
 * uart_stubs.c, without the loader console-redirect).
 *
 * Polled 8-bit UART:
 *   +0x00 TX      write  — low byte transmitted; transmitter busy until sent
 *   +0x10 STATUS  read   — bit0 = TX busy, bit1 = RX empty, bit2 = RX full
 *
 * Plain volatile stores/loads lower to memset32/memget32 (register-addressed
 * MMIO) — no CPU I/O opcodes. (-nostdinc, so uint32_t is typedef'd here rather
 * than pulled from <stdint.h>.)
 */

typedef unsigned int uint32_t;

#define UART_BASE            0xF0010000u
#define REG_UART_TX          (*(volatile uint32_t *)(UART_BASE + 0x0000u))
#define REG_UART_STATUS      (*(volatile uint32_t *)(UART_BASE + 0x0010u))
#define UART_STATUS_TX_BUSY  (1u << 0)

/* Blocking single-byte transmit: wait for the transmitter to idle, then send. */
static void _uart_raw(char c) {
    while (REG_UART_STATUS & UART_STATUS_TX_BUSY) { }
    REG_UART_TX = (unsigned char)c;
}

/* '\n' expands to CR+LF so output looks right on a serial terminal. */
void uart_putc(char c) {
    if (c == '\n')
        _uart_raw('\r');
    _uart_raw(c);
}
