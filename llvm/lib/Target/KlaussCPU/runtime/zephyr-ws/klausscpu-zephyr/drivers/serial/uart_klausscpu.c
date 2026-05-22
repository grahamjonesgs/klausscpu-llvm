/*
 * uart_klausscpu.c — Zephyr UART driver for KlaussCPU.
 * DT_DRV_COMPAT must match the compatible string in the DTS binding.
 */
#define DT_DRV_COMPAT klausscpu_uart

/*
 *
 * Uses the same KlaussCPU UART builtins as the bare-metal uart_stubs.c.
 * Only poll_in / poll_out are implemented (no IRQ mode needed for printk).
 *
 * TXCHARMEMR reads a byte from a memory address and transmits it.
 * RXRNB receives a byte non-blocking (result undefined if FIFO empty).
 * RXRB receives a byte blocking.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>

/* The char buffer must be a non-stack global so the compiler emits a
 * SETR-resolved address for TXCHARMEMR (not a FrameIndex). */
static volatile char _uart_tx_buf;

static int uart_klausscpu_poll_in(const struct device *dev, unsigned char *c)
{
    ARG_UNUSED(dev);
    /*
     * RXRNB is non-blocking: sets zero_flag when FIFO empty but doesn't
     * write the destination. We have no way to inspect the zero flag from
     * C, so we always return -1 (no data) to force callers to use
     * poll_in in a loop. The blocking receive is available via
     * uart_klausscpu_rxrb() if needed by shell/console.
     *
     * For hello_world (printk only) poll_in returning -1 is fine.
     */
    ARG_UNUSED(c);
    return -1;
}

static void uart_klausscpu_poll_out(const struct device *dev, unsigned char c)
{
    ARG_UNUSED(dev);
    /* '\n' → CR+LF for serial terminals. */
    if (c == '\n') {
        _uart_tx_buf = '\r';
        __builtin_klausscpu_txcharmemr((const void *)&_uart_tx_buf);
    }
    _uart_tx_buf = c;
    __builtin_klausscpu_txcharmemr((const void *)&_uart_tx_buf);
}

static int uart_klausscpu_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    /* Hardware UART needs no initialisation — always ready after reset. */
    return 0;
}

static const struct uart_driver_api uart_klausscpu_api = {
    .poll_in  = uart_klausscpu_poll_in,
    .poll_out = uart_klausscpu_poll_out,
};

DEVICE_DT_INST_DEFINE(0,
    uart_klausscpu_init,
    NULL,            /* pm_device */
    NULL,            /* data */
    NULL,            /* config */
    PRE_KERNEL_1,
    55, /* PRE_KERNEL_1 device priority (= KERNEL_INIT_PRIORITY_DEVICE default) */
    &uart_klausscpu_api);
