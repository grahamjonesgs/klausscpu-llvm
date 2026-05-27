/*
 * uart_klausscpu.c — Zephyr UART driver for KlaussCPU.
 *
 * TX: TXCHARMEMR builtin (reads byte from memory, transmits via UART).
 * RX: RXRB builtin (blocking receive) — run in a dedicated thread that
 *     fills a ring buffer; poll_in drains the ring buffer non-blockingly.
 *
 * The hardware has no RX FIFO status register accessible from C (RXRNB
 * sets the CPU zero flag but doesn't expose it to C code), so polling
 * via a background thread is the only viable approach.
 */
#define DT_DRV_COMPAT klausscpu_uart

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>

/* TX buffer must be a non-stack global for TXCHARMEMR address resolution. */
static volatile char _uart_tx_buf;

/* ── RX ring buffer fed by a blocking-receive thread ─────────────────────── */

#define RX_RING_SIZE 64

static uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head;   /* written by rx_thread */
static volatile uint32_t rx_tail;   /* read by poll_in */

#define RX_THREAD_STACK_SIZE 512
static K_THREAD_STACK_DEFINE(rx_stack, RX_THREAD_STACK_SIZE);
static struct k_thread rx_thread_data;

static void uart_rx_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        uint64_t ch = __builtin_klausscpu_rxrb();
        uint32_t next = (rx_head + 1) % RX_RING_SIZE;
        if (next != rx_tail) {
            rx_ring[rx_head] = (uint8_t)ch;
            rx_head = next;
        }
    }
}

/* ── Driver API ──────────────────────────────────────────────────────────── */

static int uart_klausscpu_poll_in(const struct device *dev, unsigned char *c)
{
    ARG_UNUSED(dev);

    if (rx_head == rx_tail) {
        return -1;
    }
    *c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_RING_SIZE;
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

    k_thread_create(&rx_thread_data, rx_stack, RX_THREAD_STACK_SIZE,
                    uart_rx_thread, NULL, NULL, NULL,
                    K_PRIO_COOP(1), 0, K_NO_WAIT);
    k_thread_name_set(&rx_thread_data, "uart_rx");
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
