/*
 * uart_klausscpu.c — Zephyr UART driver for KlaussCPU.
 *
 * TX: TXCHARMEMR builtin (reads byte from memory, transmits via UART).
 * RX: RXRNB (non-blocking receive) via inline asm with sentinel detection.
 *
 * RXRNB sets the CPU zero flag when the FIFO is empty but does NOT write
 * the destination register.  By pre-loading the register with 0x100 (not
 * a valid byte), we can detect empty vs. data without reading the flags.
 */
#define DT_DRV_COMPAT klausscpu_uart

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>

static volatile char _uart_tx_buf;

static int uart_klausscpu_poll_in(const struct device *dev, unsigned char *c)
{
    ARG_UNUSED(dev);

    uint64_t val;

    __asm__ volatile(
        "setr   %0, 256\n\t"
        "rxrnb  %0"
        : "=&r"(val)
        :
        :
    );

    if (val > 0xFF) {
        return -1;
    }
    *c = (unsigned char)val;
    return 0;
}

static void uart_klausscpu_poll_out(const struct device *dev, unsigned char c)
{
    ARG_UNUSED(dev);
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
    55,
    &uart_klausscpu_api);
