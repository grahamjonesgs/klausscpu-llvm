/* mmio.h — KlaussCPU memory-mapped peripherals
 *
 * MMIO region: any access with top nibble 0xF goes to the MMIO bus via
 * bus_splitter.v rather than the DDR2 cache controller.
 *
 * Address decode:  addr[31:28] == 0xF → MMIO
 *                  addr[27:16]        → device id
 *                  addr[15:0]         → register offset within device
 *
 * All registers are 8-byte aligned; access width matches the field width
 * listed below (8/16/32/64-bit loads/stores all work via byte enables).
 * Mark pointers volatile — MMIO is never cached.
 */
#ifndef KLAUSS_MMIO_H
#define KLAUSS_MMIO_H

#include <stdint.h>

/* ── Base addresses ──────────────────────────────────────────────────────── */

#define MMIO_BASE   ((uint32_t)0xF0000000u)

#define SD_BASE     (MMIO_BASE + 0x00000000u)   /* 0xF000_xxxx */
#define UART_BASE   (MMIO_BASE + 0x00010000u)   /* 0xF001_xxxx  (reserved) */
#define RGB_BASE    (MMIO_BASE + 0x00020000u)   /* 0xF002_xxxx */
#define SEG_BASE    (MMIO_BASE + 0x00030000u)   /* 0xF003_xxxx */
#define IO_BASE     (MMIO_BASE + 0x00040000u)   /* 0xF004_xxxx */

/* ── SD card — 0xF000_xxxx ───────────────────────────────────────────────── */

#define REG_SD_CTRL     (*(volatile uint32_t *)(SD_BASE + 0x0000u))
#define REG_SD_DATA     (*(volatile uint32_t *)(SD_BASE + 0x0008u))
#define REG_SD_STATUS   (*(volatile uint32_t *)(SD_BASE + 0x0010u))
#define SD_BUF_PTR      ((volatile uint8_t  *)(SD_BASE + 0x0200u))
#define SD_BUF_PTR64    ((volatile uint64_t *)(SD_BASE + 0x0200u))

/* SD_CTRL bit fields */
#define SD_CTRL_CLKDIV(d)   ((uint32_t)((d) & 0xFFFFu))
#define SD_CTRL_CS_HI       (1u << 16)
#define SD_CTRL_PWR_ON      (1u << 17)

/* SD_STATUS bit fields */
#define SD_STATUS_BUSY      (1u << 0)
#define SD_STATUS_CARD      (1u << 1)

/* SCK frequency = 100 MHz / (2 × (clk_div + 1)) */
#define SD_CLK_INIT   249u   /* ~200 kHz — safe for SD init */
#define SD_CLK_FAST     1u   /* ~25 MHz  — full speed       */

/* ── RGB LEDs — 0xF002_xxxx ─────────────────────────────────────────────── */
/* Each channel is 4 bits (0 = off, 15 = max brightness ~25% duty cycle).   */

#define REG_RGB1    (*(volatile uint32_t *)(RGB_BASE + 0x0000u))
#define REG_RGB2    (*(volatile uint32_t *)(RGB_BASE + 0x0008u))

/* Pack R, G, B (each 0..15) into the 12-bit register value. */
static inline uint32_t rgb12(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r & 0xFu) << 8) |
           ((uint32_t)(g & 0xFu) << 4) |
                      (b & 0xFu);
}

/* ── 7-segment display — 0xF003_xxxx ────────────────────────────────────── */
/* Eight digits: SEG_HIGH = upper 4, SEG_LOW = lower 4.                     */
/* Write: packed hex digits. Read: raw padded ({4'h0, digit} per byte).     */

#define REG_SEG_LOW     (*(volatile uint32_t *)(SEG_BASE + 0x0000u))
#define REG_SEG_HIGH    (*(volatile uint32_t *)(SEG_BASE + 0x0008u))
#define REG_SEG_ALL     (*(volatile uint32_t *)(SEG_BASE + 0x0010u))
#define REG_SEG_BLANK   (*(volatile uint32_t *)(SEG_BASE + 0x0018u))

/* ── LEDs and switches — 0xF004_xxxx ────────────────────────────────────── */

#define REG_LEDS        (*(volatile uint32_t *)(IO_BASE + 0x0000u))
#define REG_SWITCHES    (*(volatile uint32_t *)(IO_BASE + 0x0008u))

/* ── Cache controller — 0xF005_xxxx ─────────────────────────────────────── */
/* 2-way set-associative write-back, 64 KB total, 16-byte lines, 2048 sets.  */
/* Counters are 64-bit and free-running; clear with CACHE_CTRL_CLEAR.        */

#define CACHE_BASE            (MMIO_BASE + 0x00050000u)

#define REG_CACHE_CTRL        (*(volatile uint32_t *)(CACHE_BASE + 0x0000u))
#define REG_CACHE_INFO        (*(volatile uint64_t *)(CACHE_BASE + 0x0008u))
#define REG_CACHE_RD_HITS     (*(volatile uint64_t *)(CACHE_BASE + 0x0040u))
#define REG_CACHE_RD_MISSES   (*(volatile uint64_t *)(CACHE_BASE + 0x0048u))
#define REG_CACHE_WR_HITS     (*(volatile uint64_t *)(CACHE_BASE + 0x0050u))
#define REG_CACHE_WR_MISSES   (*(volatile uint64_t *)(CACHE_BASE + 0x0058u))
#define REG_CACHE_WRITEBACKS  (*(volatile uint64_t *)(CACHE_BASE + 0x0060u))
#define REG_CACHE_STALL_CYC   (*(volatile uint64_t *)(CACHE_BASE + 0x0068u))

/* CACHE_CTRL: bit 0 is write-1-auto-clear; clears all six counters. */
#define CACHE_CTRL_CLEAR      (1u << 0)

/* Field extractors for the read-only CACHE_INFO register.
 * Expected value for current build: 0x0001_0000_1008_0002
 *   ways=2, sets=2048, line=16 B, total=64 KB                              */
#define CACHE_INFO_WAYS(v)        ((uint32_t)( (v)        & 0xFFu))
#define CACHE_INFO_SETS(v)        ((uint32_t)(((v) >>  8) & 0xFFFFu))
#define CACHE_INFO_LINE_BYTES(v)  ((uint32_t)(((v) >> 24) & 0xFFu))
#define CACHE_INFO_TOTAL_BYTES(v) ((uint32_t)( (v) >> 32))

/* ── Interrupt controller / timer — 0xF00F_xxxx ─────────────────────────── */

#define INTC_BASE       (MMIO_BASE + 0x000F0000u)

#define REG_INT_MASK    (*(volatile uint32_t *)(INTC_BASE + 0x0000u))
#define REG_INT_PEND    (*(volatile uint32_t *)(INTC_BASE + 0x0008u))
#define REG_INT_VEC(n)  (*(volatile uint32_t *)(INTC_BASE + 0x0010u + 8u*(n)))
#define REG_TIMER_PER   (*(volatile uint32_t *)(INTC_BASE + 0x0030u))
#define REG_TIMER_CNT   (*(volatile uint32_t *)(INTC_BASE + 0x0038u))
#define REG_CLOCK_MS    (*(volatile uint64_t *)(INTC_BASE + 0x0040u))  /* atomic 64-bit */

#define INT_SRC_TIMER   0u

/* ── Convenience wrappers ────────────────────────────────────────────────── */
/* These match the old io_stubs.c calling convention so existing programs    */
/* need only add #include "mmio.h" and remove the extern declarations.       */

static inline void leds(uint32_t val)       { REG_LEDS    = val; }
static inline void seg7(uint32_t val)       { REG_SEG_ALL = val; }
static inline void seg7blank(void)          { REG_SEG_BLANK = 0; }
static inline void seg7_hi(uint32_t val)    { REG_SEG_HIGH  = val; }
static inline void seg7_lo(uint32_t val)    { REG_SEG_LOW   = val; }
static inline uint32_t switches(void)       { return REG_SWITCHES; }

#endif /* KLAUSS_MMIO_H */
