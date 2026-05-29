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

/* ── CPU performance counters — 0xF00D_xxxx ─────────────────────────────── */
/* All 64-bit, free-running, read-only.  Clear every counter with             */
/* PERF_CTRL_CLEAR (write-1-auto-clear).  Profile a region: clear, run, read. */

#define PERF_BASE                  (MMIO_BASE + 0x000D0000u)

#define REG_PERF_CTRL              (*(volatile uint32_t *)(PERF_BASE + 0x0000u))
/* Tier 0 — denominators */
#define REG_PERF_CYCLES            (*(volatile uint64_t *)(PERF_BASE + 0x0008u))
#define REG_PERF_INSTR             (*(volatile uint64_t *)(PERF_BASE + 0x0010u))
/* Tier 1 — cycle accounting (mutually exclusive buckets) */
#define REG_PERF_FETCH_CYCLES      (*(volatile uint64_t *)(PERF_BASE + 0x0018u))
#define REG_PERF_EXEC_CYCLES       (*(volatile uint64_t *)(PERF_BASE + 0x0020u))
#define REG_PERF_MUL_CYCLES        (*(volatile uint64_t *)(PERF_BASE + 0x0028u))
#define REG_PERF_DIV_CYCLES        (*(volatile uint64_t *)(PERF_BASE + 0x0030u))
#define REG_PERF_INT_CYCLES        (*(volatile uint64_t *)(PERF_BASE + 0x0038u))
#define REG_PERF_IDLE_CYCLES       (*(volatile uint64_t *)(PERF_BASE + 0x0040u))
#define REG_PERF_MUL_OPS           (*(volatile uint64_t *)(PERF_BASE + 0x0048u))
#define REG_PERF_DIV_OPS           (*(volatile uint64_t *)(PERF_BASE + 0x0050u))
#define REG_PERF_INT_OPS           (*(volatile uint64_t *)(PERF_BASE + 0x0058u))
/* Tier 2 — instruction mix (ALU+LOAD+STORE+BRANCH+JUMP+CALL+INDIRECT+OTHER==INSTR) */
#define REG_PERF_CNT_ALU           (*(volatile uint64_t *)(PERF_BASE + 0x0060u))
#define REG_PERF_CNT_LOAD          (*(volatile uint64_t *)(PERF_BASE + 0x0068u))
#define REG_PERF_CNT_STORE         (*(volatile uint64_t *)(PERF_BASE + 0x0070u))
#define REG_PERF_CNT_BRANCH        (*(volatile uint64_t *)(PERF_BASE + 0x0078u))
#define REG_PERF_CNT_BRANCH_TAKEN  (*(volatile uint64_t *)(PERF_BASE + 0x0080u))
#define REG_PERF_CNT_JUMP          (*(volatile uint64_t *)(PERF_BASE + 0x0088u))
#define REG_PERF_CNT_CALL          (*(volatile uint64_t *)(PERF_BASE + 0x0090u))
#define REG_PERF_CNT_INDIRECT      (*(volatile uint64_t *)(PERF_BASE + 0x0098u))
#define REG_PERF_CNT_OTHER         (*(volatile uint64_t *)(PERF_BASE + 0x00A0u))

/* PERF_CTRL: bit 0 is write-1-auto-clear; zeroes every counter next cycle. */
#define PERF_CTRL_CLEAR            (1u << 0)

/* ── Interrupt controller / timer — 0xF00F_xxxx ─────────────────────────── */

#define INTC_BASE       (MMIO_BASE + 0x000F0000u)

#define REG_INT_MASK    (*(volatile uint32_t *)(INTC_BASE + 0x0000u))
#define REG_INT_PEND    (*(volatile uint32_t *)(INTC_BASE + 0x0008u))
#define REG_INT_VEC(n)  (*(volatile uint32_t *)(INTC_BASE + 0x0010u + 8u*(n)))
#define REG_TIMER_PER   (*(volatile uint32_t *)(INTC_BASE + 0x0030u))
#define REG_TIMER_CNT   (*(volatile uint32_t *)(INTC_BASE + 0x0038u))
#define REG_CLOCK_MS    (*(volatile uint64_t *)(INTC_BASE + 0x0040u))  /* atomic 64-bit */

#define INT_SRC_TIMER   0u
#define INT_SRC_ETH     1u   /* LiteEth RX/TX combined — wired in Phase 6 */

/* INT_MASK value covering all enabled sources.
 * Update this when new interrupt sources are connected:
 *   Phase 1-5 (polling only): INTMASK_ALL = 1  (timer only)
 *   Phase 6+  (ETH IRQ wired): change to 3     (timer + eth)   */
#define INTMASK_ALL     1u

/* ── Ethernet (LiteEth) — CSRs 0xF006_xxxx, slot SRAM 0xF008_xxxx ───────── */
/* All CSR registers are 32-bit, 4-byte spaced (NOT 8 like other peripherals) */

#define ETH_CSR_BASE              (MMIO_BASE + 0x00060000u)
#define ETH_SRAM_BASE             (MMIO_BASE + 0x00080000u)

/* SoCController */
#define REG_ETH_CTRL_RESET        (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0000u))
#define REG_ETH_CTRL_SCRATCH      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0004u))
#define REG_ETH_CTRL_BUS_ERRORS   (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0008u))

/* PHY: hardware reset + bit-banged MDIO */
#define REG_ETH_PHY_RESET         (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0800u))
#define REG_ETH_MDIO_W            (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0804u))
#define REG_ETH_MDIO_R            (*(volatile uint32_t *)(ETH_CSR_BASE + 0x0808u))

/* MDIO_W bit fields */
#define ETH_MDIO_MDC              (1u << 0)   /* MDC clock */
#define ETH_MDIO_OE               (1u << 1)   /* drive MDIO output */
#define ETH_MDIO_OUT              (1u << 2)   /* MDIO data out */

/* MAC RX (sram_writer: HW fills slot SRAM, then sets RX_EV_PENDING) */
#define REG_ETH_RX_SLOT           (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1000u))
#define REG_ETH_RX_LENGTH         (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1004u))
#define REG_ETH_RX_ERRORS         (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1008u))
#define REG_ETH_RX_EV_STATUS      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x100Cu))
#define REG_ETH_RX_EV_PENDING     (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1010u))
#define REG_ETH_RX_EV_ENABLE      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1014u))

/* MAC TX (sram_reader: HW reads slot SRAM and transmits on TX_START) */
#define REG_ETH_TX_START          (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1018u))
#define REG_ETH_TX_READY          (*(volatile uint32_t *)(ETH_CSR_BASE + 0x101Cu))
#define REG_ETH_TX_LEVEL          (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1020u))
#define REG_ETH_TX_SLOT           (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1024u))
#define REG_ETH_TX_LENGTH         (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1028u))
#define REG_ETH_TX_EV_STATUS      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x102Cu))
#define REG_ETH_TX_EV_PENDING     (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1030u))
#define REG_ETH_TX_EV_ENABLE      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1034u))

/* MAC stats */
#define REG_ETH_PREAMBLE_CRC      (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1038u))
#define REG_ETH_RX_PREAMBLE_ERR   (*(volatile uint32_t *)(ETH_CSR_BASE + 0x103Cu))
#define REG_ETH_RX_CRC_ERR        (*(volatile uint32_t *)(ETH_CSR_BASE + 0x1040u))

/* Slot SRAMs: 2 KiB each, plain byte-addressable memory.
 * Layout: RX0 @ +0x0000, RX1 @ +0x0800, TX0 @ +0x1000, TX1 @ +0x1800 */
#define ETH_SLOT_SIZE             2048u
#define ETH_RX_SLOT_PTR(n)   ((volatile uint8_t *)(unsigned long)(ETH_SRAM_BASE + 0x0000u + (n)*ETH_SLOT_SIZE))
#define ETH_TX_SLOT_PTR(n)   ((volatile uint8_t *)(unsigned long)(ETH_SRAM_BASE + 0x1000u + (n)*ETH_SLOT_SIZE))
/* Short aliases used by imported driver code */
#define ETH_RX_SLOT(n)  ETH_RX_SLOT_PTR(n)
#define ETH_TX_SLOT(n)  ETH_TX_SLOT_PTR(n)

/* MAC address: byte0 bits[1:0]=00 → "globally-administered unicast" per IEEE.
 * (The 00:AB:CD prefix isn't a registered OUI but no network filters on that.)
 * 02:xx — locally-administered; some routers/DHCP servers silently drop
 * DISCOVERs from locally-administered MACs.  Using 00:AB:CD avoids that. */

#define ETH_MAC_ADDR  { 0x00u, 0xABu, 0xCDu, 0x00u, 0x00u, 0x01u }
/* Individual byte macros for code that initialises byte arrays */
#define ETH_DEFAULT_MAC_0  0x00u
#define ETH_DEFAULT_MAC_1  0xABu
#define ETH_DEFAULT_MAC_2  0xCDu
#define ETH_DEFAULT_MAC_3  0x00u
#define ETH_DEFAULT_MAC_4  0x00u
#define ETH_DEFAULT_MAC_5  0x01u

/* LAN8720A PHY address on Nexys A7 */
#define ETH_PHY_ADDR  1u

/* Endianness helpers — wire is big-endian; CPU is little-endian.
 * Guarded: lwIP redefines htons/htonl as macros pointing to lwip_htons/lwip_htonl.
 * When lwIP headers are included first the guard fires and we skip these,
 * avoiding a static/non-static redefinition error. */
#ifndef htons
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >>  8) | ((v & 0xFF000000u) >> 24);
}
#ifndef ntohs
#define ntohs(v) htons(v)
#define ntohl(v) htonl(v)
#endif
#endif

/* ── Hardware crypto — 0xF00A_xxxx / 0xF00B_xxxx / 0xF00C_xxxx ──────────── */
/* Register layout matches MMIO_MAP.md exactly.                               */

/* AES-128 core: 0xF00A_0000 */
#define AES_BASE   (MMIO_BASE + 0x000A0000u)

#define REG_AES_CTRL    (*(volatile uint32_t *)(AES_BASE + 0x000u))
#define REG_AES_STATUS  (*(volatile uint32_t *)(AES_BASE + 0x008u))
#define REG_AES_KEY0    (*(volatile uint64_t *)(AES_BASE + 0x010u))
#define REG_AES_KEY1    (*(volatile uint64_t *)(AES_BASE + 0x018u))
#define REG_AES_IN0     (*(volatile uint64_t *)(AES_BASE + 0x040u))
#define REG_AES_IN1     (*(volatile uint64_t *)(AES_BASE + 0x048u))
#define REG_AES_OUT0    (*(volatile uint64_t *)(AES_BASE + 0x050u))
#define REG_AES_OUT1    (*(volatile uint64_t *)(AES_BASE + 0x058u))

#define AES_CTRL_GO       (1u << 0)   /* kick encrypt/decrypt; sampled with ENC */
#define AES_CTRL_ENC      (1u << 1)   /* 1=encrypt, 0=decrypt */
#define AES_CTRL_KEY_LOAD (1u << 2)   /* expand key schedule */
#define AES_CTRL_KEY_ZERO (1u << 3)   /* wipe key registers */
#define AES_STATUS_BUSY   (1u << 0)
#define AES_STATUS_DONE   (1u << 1)   /* sticky; cleared by next GO/KEY_LOAD */

/* AES-GCM / GHASH: 0xF00A_0080
 * Raw GHASH interface: tag = (tag XOR X) * H in GF(2^128).
 * Software sequences AES-CTR + these GHASH steps to build full AES-GCM. */
#define REG_GCM_CTRL    (*(volatile uint32_t *)(AES_BASE + 0x080u))
#define REG_GCM_STATUS  (*(volatile uint32_t *)(AES_BASE + 0x088u))
#define REG_GCM_H0      (*(volatile uint64_t *)(AES_BASE + 0x090u))  /* H bytes 0..7   */
#define REG_GCM_H1      (*(volatile uint64_t *)(AES_BASE + 0x098u))  /* H bytes 8..15  */
#define REG_GCM_X0      (*(volatile uint64_t *)(AES_BASE + 0x0A0u))  /* X bytes 0..7   */
#define REG_GCM_X1      (*(volatile uint64_t *)(AES_BASE + 0x0A8u))  /* X bytes 8..15  */
#define REG_GCM_TAG0    (*(volatile uint64_t *)(AES_BASE + 0x0B0u))  /* tag bytes 0..7  (RO) */
#define REG_GCM_TAG1    (*(volatile uint64_t *)(AES_BASE + 0x0B8u))  /* tag bytes 8..15 (RO) */

#define GCM_CTRL_GO     (1u << 0)  /* tag = (tag XOR X) * H; self-clearing */
#define GCM_CTRL_RESET  (1u << 1)  /* tag = 0; self-clearing */
#define GCM_STATUS_BUSY (1u << 0)
#define GCM_STATUS_DONE (1u << 1)

/* SHA-256 core: 0xF00B_0000
 * Software must do all SHA-256 padding; hardware compresses one block per START.
 * Hardware byteswaps each 32-bit half of BLOCK/DIGEST so software uses natural
 * memory order: ((uint64_t*)BLOCK)[i] = ((uint64_t*)msg)[i]. */
#define SHA_BASE   (MMIO_BASE + 0x000B0000u)

#define REG_SHA_CTRL     (*(volatile uint32_t *)(SHA_BASE + 0x000u))
#define REG_SHA_STATUS   (*(volatile uint32_t *)(SHA_BASE + 0x008u))
#define SHA_BLOCK_PTR    ((volatile uint64_t *)(SHA_BASE + 0x010u))   /* 8 × 64-bit slots */
#define SHA_DIGEST_PTR   ((volatile uint64_t *)(SHA_BASE + 0x050u))   /* 4 × 64-bit slots */
/* Indexed access (equivalent to SHA_BLOCK_PTR[n] / SHA_DIGEST_PTR[n]). */
#define REG_SHA_BLOCK(n)  (*(volatile uint64_t *)(SHA_BASE + 0x010u + (n)*8u))
#define REG_SHA_DIGEST(n) (*(volatile uint64_t *)(SHA_BASE + 0x050u + (n)*8u))

#define SHA_CTRL_INIT    (1u << 0)   /* reset H to FIPS-180-4 IV */
#define SHA_CTRL_START   (1u << 1)   /* compress block in BLOCK regs */
/* Note: no SHA_CTRL_LAST or SHA_BITLEN register — software writes the length
 * bytes into the block data and calls SHA_CTRL_START for every block. */
#define SHA_STATUS_BUSY  (1u << 0)
#define SHA_STATUS_DONE  (1u << 1)

/* HMAC wrapper: 0xF00B_0080
 * START/FINAL are single-cycle H-load pulses; KEY_LOAD is ~135 cycles. */
#define REG_HMAC_CTRL    (*(volatile uint32_t *)(SHA_BASE + 0x080u))
#define REG_HMAC_STATUS  (*(volatile uint32_t *)(SHA_BASE + 0x088u))
#define REG_HMAC_KEY0    (*(volatile uint64_t *)(SHA_BASE + 0x090u))
#define REG_HMAC_KEY1    (*(volatile uint64_t *)(SHA_BASE + 0x098u))
#define REG_HMAC_KEY2    (*(volatile uint64_t *)(SHA_BASE + 0x0A0u))
#define REG_HMAC_KEY3    (*(volatile uint64_t *)(SHA_BASE + 0x0A8u))
/* Indexed alias for loops. */
#define REG_HMAC_KEY(n)  (*(volatile uint64_t *)(SHA_BASE + 0x090u + (n)*8u))

#define HMAC_CTRL_KEY_LOAD (1u << 0)  /* recompute inner/outer midstates */
#define HMAC_CTRL_START    (1u << 1)  /* H ← inner_state (single cycle) */
#define HMAC_CTRL_FINAL    (1u << 2)  /* H ← outer_state (single cycle) */
#define HMAC_CTRL_KEY_ZERO (1u << 3)  /* wipe key + midstates */
#define HMAC_STATUS_BUSY      (1u << 0)
#define HMAC_STATUS_KEY_VALID (1u << 1)  /* midstates valid; cleared by KEY_ZERO */

/* TRNG: 0xF00C_0000 */
#define TRNG_BASE  (MMIO_BASE + 0x000C0000u)

#define REG_TRNG_CTRL    (*(volatile uint32_t *)(TRNG_BASE + 0x000u))
#define REG_TRNG_STATUS  (*(volatile uint32_t *)(TRNG_BASE + 0x008u))
#define REG_TRNG_DATA    (*(volatile uint64_t *)(TRNG_BASE + 0x010u))  /* read pops FIFO */

#define TRNG_CTRL_ENABLE  (1u << 0)
#define TRNG_CTRL_RESEED  (1u << 1)   /* self-clearing */
#define TRNG_STATUS_READY     (1u << 0)
#define TRNG_STATUS_HEALTH_OK (1u << 1)

/* ── Crypto wait helpers ─────────────────────────────────────────────────── */

static inline void aes_wait_done(void)     { while (REG_AES_STATUS & AES_STATUS_BUSY) {} }
static inline void sha_wait_done(void)     { while (!(REG_SHA_STATUS & SHA_STATUS_DONE)) {} }
static inline void gcm_wait_done(void)     { while (REG_GCM_STATUS & GCM_STATUS_BUSY) {} }
static inline void hmac_wait_keyload(void) { while (REG_HMAC_STATUS & HMAC_STATUS_BUSY) {} }

/* Accumulate one 128-bit block (lo=bytes 0..7, hi=bytes 8..15) into GHASH tag. */
static inline void gcm_ghash_block(uint64_t lo, uint64_t hi) {
    REG_GCM_X0 = lo;
    REG_GCM_X1 = hi;
    REG_GCM_CTRL = GCM_CTRL_GO;
    gcm_wait_done();
}

/* Non-blocking TRNG read: returns 1 if a word was available, 0 otherwise.
 * Returns 0 (and does not read) if the health monitor has tripped. */
static inline int trng_read64(uint64_t *out) {
    if (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) return 0;
    if (!(REG_TRNG_STATUS & TRNG_STATUS_HEALTH_OK)) return 0;
    *out = REG_TRNG_DATA;
    return 1;
}

/* ── Convenience wrappers ────────────────────────────────────────────────── */
/* These match the old io_stubs.c calling convention so existing programs    */
/* need only add #include "mmio.h" and remove the extern declarations.       */

static inline void leds(uint32_t val)       { REG_LEDS    = val; }
static inline void seg7(uint32_t val)       { REG_SEG_ALL = val; }
static inline void seg7blank(void)          { REG_SEG_BLANK = 0; }
static inline void seg7_hi(uint32_t val)    { REG_SEG_HIGH  = val; }
static inline void seg7_lo(uint32_t val)    { REG_SEG_LOW   = val; }
static inline uint32_t switches(void)       { return REG_SWITCHES; }

/* ── Timing helper ───────────────────────────────────────────────────────── */
/* Busy-wait using the free-running ms counter. Works before RTOS starts.
 * In RTOS tasks prefer rtos_sleep_ticks(ms / 10) to avoid blocking the scheduler. */
static inline void delay_ms(uint32_t ms) {
    uint64_t end = REG_CLOCK_MS + ms;
    while (REG_CLOCK_MS < end) {}
}

#endif /* KLAUSS_MMIO_H */
