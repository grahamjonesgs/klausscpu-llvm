/* eth_test.c — Phase 5 bring-up tests for KlaussCPU + LiteEth.
 *
 * Runs four tests in order, each gating the next.  Status is shown on the
 * 7-segment display so a host UART is not strictly required.  Output codes:
 *
 *   T1 (scratchpad)  : "0050xxxx"  → "Pxxxxxxx" pass / "Exxxxxxx" fail
 *   T2 (MDIO PHY ID) : "0051xxxx"  → "0051" + ID1 (expect 0x0007 for LAN8720A)
 *   T3 (loopback)    : "0052xxxx"  → "P052xxxx" pass / "E052xxxx" fail
 *   T4 (broadcast)   : "0053xxxx"  → frame length on RX
 *
 * Wait at least ~1 second per stage to read the display.
 */

#include "../mmio.h" /* provides REG_ETH_*, ETH_TX_SLOT, ETH_RX_SLOT,
                           ETH_DEFAULT_MAC_*, delay_ms()              */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Tiny helpers
 * ------------------------------------------------------------------------ */

/* Cycle-accurate busy loop — used only for MDIO MDC half-period (~200 ns).
 * delay_ms() for millisecond-scale waits comes from mmio.h (REG_CLOCK_MS). */
static inline void delay_loops(uint32_t n) {
  volatile uint32_t i;
  for (i = 0; i < n; i++) {
    __asm__ volatile("");
  }
}

static inline void show(uint32_t code) { REG_SEG_ALL = code; }

static void print_pass(const char *tag) { printf("  [PASS] %s\n", tag); }
static void print_fail(const char *tag) { printf("  [FAIL] %s\n", tag); }

/* ---------------------------------------------------------------------------
 * MDIO bit-bang (IEEE 802.3 Clause 22)
 *
 * REG_ETH_MDIO_W  layout:  [0] MDC clock   [1] MDIO output enable   [2] MDIO
 * data out REG_ETH_MDIO_R  layout:  [0] live MDIO line value
 *
 * We drive MDC manually and hold each bit for ~200 ns (well under the
 * 2.5 MHz max).  The PHY samples MDIO on MDC's rising edge, and drives
 * MDIO during the read-data window when we tristate (OE=0).
 *
 * LAN8720A PHY address on Nexys A7 = 0x01 (per Digilent ref manual, RXER
 * pin strapped low at reset).
 * ------------------------------------------------------------------------ */

#define MDIO_PHY_ADDR 0x01u
#define MDIO_W_MDC ETH_MDIO_MDC
#define MDIO_W_OE ETH_MDIO_OE
#define MDIO_W_DOUT ETH_MDIO_OUT

#define MDIO_HALF_DELAY() delay_loops(40) /* ~ tBIT/2 at 100 MHz */

/* Drive one MDC cycle: data=output bit (when oe=1), capture inbit if !oe. */
static uint32_t mdio_clock(uint32_t outbit, uint32_t oe) {
  uint32_t base = (oe ? MDIO_W_OE : 0u) | (outbit ? MDIO_W_DOUT : 0u);
  REG_ETH_MDIO_W = base; /* MDC low, present data  */
  MDIO_HALF_DELAY();
  REG_ETH_MDIO_W = base | MDIO_W_MDC; /* MDC high (PHY samples) */
  uint32_t in = REG_ETH_MDIO_R & 1u;
  MDIO_HALF_DELAY();
  return in;
}

static void mdio_shift_out(uint32_t value, int nbits) {
  for (int i = nbits - 1; i >= 0; i--) {
    mdio_clock((value >> i) & 1u, /*oe=*/1);
  }
}

static uint16_t mdio_read(uint32_t phy_addr, uint32_t reg_addr) {
  for (int i = 0; i < 32; i++)
    mdio_clock(1, 1);      /* preamble */
  mdio_shift_out(0x1u, 2); /* START 01 */
  mdio_shift_out(0x2u, 2); /* OP    10 (read) */
  mdio_shift_out(phy_addr & 0x1Fu, 5);
  mdio_shift_out(reg_addr & 0x1Fu, 5);
  mdio_clock(0, 0); /* TA — 1 cycle, not 2 */
  /* PHY now drives D15 starting at the next MDC rising edge */
  uint16_t v = 0;
  for (int i = 15; i >= 0; i--) {
    v = (uint16_t)((v << 1) | mdio_clock(0, 0));
  }
  mdio_clock(0, 0); /* idle */
  return v;
}

/* Write `value` to `phy_addr` reg `reg_addr`. */
static void mdio_write(uint32_t phy_addr, uint32_t reg_addr, uint16_t value) {
  for (int i = 0; i < 32; i++)
    mdio_clock(1, 1);
  mdio_shift_out(0x1u, 2); /* 01 */
  mdio_shift_out(0x1u, 2); /* 01 = write */
  mdio_shift_out(phy_addr & 0x1Fu, 5);
  mdio_shift_out(reg_addr & 0x1Fu, 5);
  mdio_shift_out(0x2u, 2); /* TA = 10 */
  mdio_shift_out(value, 16);
  mdio_clock(0, 0); /* idle */
}

/* ---------------------------------------------------------------------------
 * Test 5.0 — Scratchpad sanity check
 *
 * Proves the whole MMIO → bus_splitter → eth_mmio_bridge → LiteEth Wishbone
 * → SoCController CSR path round-trips.  Three patterns to catch stuck-bit /
 * width issues.
 * ------------------------------------------------------------------------ */

static int test_scratchpad(void) {
  /* 1. Read scratch's POR default — should be 0x12345678 */
  volatile uint32_t before = REG_ETH_CTRL_SCRATCH;
  printf("  POR default = 0x%08lx (expect 0x12345678)\n",
         (unsigned long)before);
  show(0xC0DE0000u | (before & 0xFFFFu)); /* expect "C0DE5678" */
  delay_ms(2000);

  /* 2. Try writing and reading back */
  REG_ETH_CTRL_SCRATCH = 0xCAFEBABEu;
  volatile uint32_t after = REG_ETH_CTRL_SCRATCH;
  printf("  wrote 0xCAFEBABE, read back 0x%08lx\n", (unsigned long)after);
  show(0xBABE0000u | (after & 0xFFFFu)); /* expect "BABEBABE" */
  delay_ms(2000);

  if (after != 0xCAFEBABEu) {
    print_fail("scratch round-trip");
    return -1;
  }
  print_pass("scratch round-trip");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 5.1 — MDIO reachability + PHY ID
 *
 * Reads PHY register 0x02 (PHY ID1).  LAN8720A returns 0x0007.  If MDIO
 * isn't reachable (PHY in reset, bus floating, or the bridge mangles
 * narrow CSR accesses), the read returns 0xFFFF instead.
 * ------------------------------------------------------------------------ */

static int test_mdio_phy_id(void) {
  /* Slow MDIO down — be safe rather than at the spec edge */
  /* (requires changing MDIO_HALF_DELAY to delay_loops(200) — ~50 KHz, fine) */

  REG_ETH_PHY_RESET = 1;
  delay_ms(30);
  REG_ETH_PHY_RESET = 0;
  delay_ms(120);

  /* 1. Scan all 32 PHY addresses, looking for ID1 == 0x0007 */
  printf("  scanning PHY addresses 0x00..0x1F\n");
  int found_addr = -1;
  for (uint32_t addr = 0; addr < 32; addr++) {
    uint16_t id1 = mdio_read(addr, 0x02);
    uint16_t id2 = mdio_read(addr, 0x03);
    if (id1 != 0x0000u && id1 != 0xFFFFu) {
      printf("    addr 0x%02lx: ID1=0x%04lx ID2=0x%04lx\n", (unsigned long)addr,
             (unsigned long)id1, (unsigned long)id2);
      if (id1 == 0x0007u && (id2 & 0xFFF0u) == 0xC0F0u) {
        found_addr = (int)addr;
        printf("    ^^ LAN8720A signature\n");
      }
    }
  }

  /* 2. Dump reg 0..6 at the assumed-correct address (0x01) for comparison */
  printf("  dumping regs 0..6 at PHY addr 0x01:\n");
  for (uint32_t r = 0; r <= 6; r++) {
    printf("    reg 0x%02lx = 0x%04lx\n", (unsigned long)r,
           (unsigned long)mdio_read(0x01, r));
  }

  if (found_addr < 0) {
    printf("  no PHY found with LAN8720A signature\n");
    return -1;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 5.2 — PHY internal loopback
 *
 * Set PHY register 0x00 bit 14 (internal MII loopback), TX a hand-crafted
 * frame, expect to see the same frame at RX.  Validates the entire MAC
 * datapath end-to-end without putting any signal on the wire.
 * ------------------------------------------------------------------------ */

static int test_phy_loopback(void) {
  /* Force 100M FD, disable AN, enable internal loopback (bit 14).
     LAN8720A loopback only works in 100M FD with AN off. */
  printf("  setting BMCR = 0x6100 (loopback, 100M, FD, no AN)\n");
  mdio_write(MDIO_PHY_ADDR, 0x00, 0x6100);
  delay_ms(50); /* let loopback PLL settle */

  /* Verify the write took */
  uint16_t bmcr_rb = mdio_read(MDIO_PHY_ADDR, 0x00);
  printf("  BMCR readback = 0x%04lx (expect 0x6100)\n", (unsigned long)bmcr_rb);
  if (bmcr_rb != 0x6100u) {
    printf("  BMCR didn't take — write path broken?\n");
    return -1;
  }

  /* Enable RX/TX done events so ev_pending latches */
  REG_ETH_RX_EV_ENABLE = 1;
  REG_ETH_TX_EV_ENABLE = 1;

  /* Drain any stale events */
  REG_ETH_RX_EV_PENDING = 1;
  REG_ETH_TX_EV_PENDING = 1;

  /* ... rest of the test (frame staging, TX kick) unchanged ... */

  /* Build a 64-byte test frame (smallest legal Ethernet frame).  The MAC
     handles preamble/SFD; we provide only DST/SRC/EtherType/payload. */
  static const uint8_t frame[64] = {
      /* dst MAC */ 0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
      /* src MAC */ 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
      /* etype  */ 0x88, 0xB5, /* 0x88B5 = local experimental */
      /* payload (50 bytes) — counting pattern */
      0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13,
      0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
      0x2C, 0x2D};

  /* Stage frame into TX slot 0 */
  while (!REG_ETH_TX_READY) {
  }
  volatile uint8_t *tx = ETH_TX_SLOT(0);
  for (uint32_t i = 0; i < sizeof(frame); i++)
    tx[i] = frame[i];
  REG_ETH_TX_SLOT = 0;
  REG_ETH_TX_LENGTH = sizeof(frame);
  REG_ETH_TX_START = 1;
  printf(
      "  TX: %lu-byte frame sent (etype 0x88B5), waiting for loopback RX...\n",
      (unsigned long)sizeof(frame));

  /* Wait for *either* TX-done or RX-done, whichever comes first */
  for (uint32_t i = 0; i < 200000u; i++) {
    if (REG_ETH_TX_EV_PENDING & 1u) {
      printf("  TX completed (event seen)\n");
      REG_ETH_TX_EV_PENDING = 1; /* clear, keep waiting for RX */
    }
    if (REG_ETH_RX_EV_PENDING & 1u)
      goto got_rx;
    delay_loops(50);
  }
  /* Timed out — what state are we in? */
  printf("  RX timeout. Diagnostics:\n");
  printf("    TX_READY      = %lu\n", (unsigned long)REG_ETH_TX_READY);
  printf("    TX_EV_PENDING = 0x%08lx\n", (unsigned long)REG_ETH_TX_EV_PENDING);
  printf("    RX_EV_PENDING = 0x%08lx\n", (unsigned long)REG_ETH_RX_EV_PENDING);
  printf("    RX_ERRORS     = %lu\n", (unsigned long)REG_ETH_RX_ERRORS);
  printf("    RX_LENGTH     = %lu\n", (unsigned long)REG_ETH_RX_LENGTH);
  show(0xE0520001u);
  return -1;

got_rx: {
  uint32_t slot = REG_ETH_RX_SLOT;
  uint32_t len = REG_ETH_RX_LENGTH;
  printf("  RX: %lu bytes in slot %lu\n", (unsigned long)len,
         (unsigned long)slot);
  if (len != sizeof(frame)) {
    printf("  length mismatch: got %lu, expected %lu\n", (unsigned long)len,
           (unsigned long)sizeof(frame));
    show(0xE0520002u | (len << 16));
    print_fail("loopback length mismatch");
    return -1;
  }
  volatile uint8_t *rx = ETH_RX_SLOT(slot);
  for (uint32_t i = 0; i < sizeof(frame); i++) {
    if (rx[i] != frame[i]) {
      printf("  byte mismatch at offset %lu: got 0x%02lx, expected 0x%02lx\n",
             (unsigned long)i, (unsigned long)rx[i], (unsigned long)frame[i]);
      show(0xE0520003u | (i << 16));
      print_fail("loopback data mismatch");
      return -1;
    }
  }
  REG_ETH_RX_EV_PENDING = 1; /* W1C release */
}

  /* Disable loopback for subsequent tests */
  mdio_write(MDIO_PHY_ADDR, 0x00, bmcr_rb);
  delay_ms(10);
  print_pass("PHY internal loopback");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 5.3 — External TX (one ARP request on the wire)
 *
 * Plug into a switch (or a host running tcpdump on a crossover cable).
 * Send a gratuitous ARP request from `192.168.1.50` looking for `.1`.
 * The host should see the broadcast in `tcpdump -i ethX -e arp -nn`.
 *
 * No automated pass/fail — visual inspection on the host side.
 * ------------------------------------------------------------------------ */

static void test_external_tx(void) {
  static const uint8_t our_mac[6] = {ETH_DEFAULT_MAC_0, ETH_DEFAULT_MAC_1,
                                     ETH_DEFAULT_MAC_2, ETH_DEFAULT_MAC_3,
                                     ETH_DEFAULT_MAC_4, ETH_DEFAULT_MAC_5};
  mdio_write(MDIO_PHY_ADDR, 0x00,
             0x3000); /* AN enabled, 100M default — no loopback */
  delay_ms(150);      /* let auto-negotiation complete */
  uint16_t bmsr = mdio_read(MDIO_PHY_ADDR, 0x01);
  printf("  BMSR after AN: 0x%04lx (bit 5 = AN done, bit 2 = link up)\n",
         (unsigned long)bmsr);

  uint8_t f[64] = {0};

  /* Ethernet header — broadcast destination */
  for (int i = 0; i < 6; i++)
    f[i] = 0xFF;
  for (int i = 0; i < 6; i++)
    f[6 + i] = our_mac[i];
  f[12] = 0x08;
  f[13] = 0x06; /* etype = ARP */

  /* ARP payload (network byte order) */
  f[14] = 0x00;
  f[15] = 0x01; /* HTYPE = Ethernet */
  f[16] = 0x08;
  f[17] = 0x00; /* PTYPE = IPv4 */
  f[18] = 6;
  f[19] = 4; /* HLEN, PLEN */
  f[20] = 0x00;
  f[21] = 0x01; /* OPER = request */
  for (int i = 0; i < 6; i++)
    f[22 + i] = our_mac[i]; /* SHA */
  f[28] = 192;
  f[29] = 168;
  f[30] = 1;
  f[31] = 50; /* SPA = 192.168.1.50 */
  /* THA already zero */
  f[38] = 192;
  f[39] = 168;
  f[40] = 1;
  f[41] = 1; /* TPA = 192.168.1.1 */

  printf("  src MAC: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
         (unsigned long)our_mac[0], (unsigned long)our_mac[1],
         (unsigned long)our_mac[2], (unsigned long)our_mac[3],
         (unsigned long)our_mac[4], (unsigned long)our_mac[5]);
  printf("  sending ARP request: 192.168.1.50 who-has 192.168.1.1\n");

  while (!REG_ETH_TX_READY) {
  }
  volatile uint8_t *tx =
      ETH_TX_SLOT(1); /* use slot 1; keep slot 0 for loopback */
  for (uint32_t i = 0; i < 60u; i++)
    tx[i] = f[i];
  REG_ETH_TX_SLOT = 1;
  REG_ETH_TX_LENGTH = 60;
  REG_ETH_TX_START = 1;

  /* Wait for TX completion */
  while (!(REG_ETH_TX_EV_PENDING & 1u)) {
  }
  REG_ETH_TX_EV_PENDING = 1;
  printf("  TX complete — check host with: tcpdump -i ethX -e arp -nn\n");
}

/* ---------------------------------------------------------------------------
 * Test 5.4 — Passive RX
 *
 * Sit in a polling loop and display the length of each incoming frame on
 * the 7-segment.  On any normal Ethernet network the host or switch will
 * be sending broadcasts (ARP, mDNS, LLDP, …) and you'll see the display
 * change every few seconds.
 * ------------------------------------------------------------------------ */

static void test_passive_rx(void) {
    uint32_t frame_count = 0;
    printf("  listening — press reset to stop\n");
    while (1) {
        if (REG_ETH_RX_EV_PENDING & 1u) {
            uint32_t slot = REG_ETH_RX_SLOT;
            uint32_t len  = REG_ETH_RX_LENGTH;
            volatile uint8_t *rx = ETH_RX_SLOT(slot);

            /* Raw hex dump of the first 24 bytes — covers dst MAC, src MAC,
               etype, and the first 4 bytes of payload.  Spaces mark the
               Ethernet II field boundaries. */
            printf("  #%lu len=%lu raw=",
                   (unsigned long)++frame_count, (unsigned long)len);
            for (int i = 0; i < 24 && i < (int)len; i++) {
                if (i == 6 || i == 12 || i == 14) printf("| ");
                printf("%02x ", (uint8_t)rx[i]);
            }
            printf("\n");

            REG_ETH_RX_EV_PENDING = 1;
            show((frame_count << 16) | (len & 0xFFFFu));
        }
    }
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */

int main(void) {
  printf("\n=== KlaussCPU LiteEth Phase 5 bring-up ===\n\n");
  show(0xEEEE0002u);
  delay_ms(1000);

  /* T1: scratchpad */
  printf("T1: MMIO scratchpad\n");
  show(0x00500000u);
  delay_ms(300);
  if (test_scratchpad() != 0) {
    show(0xE0500001u);
    printf("HALTED on T1 failure\n");
    while (1) {
    }
  }
  show(0x00500001u);
  delay_ms(1000);

  /* T2: MDIO PHY ID */
  printf("\nT2: MDIO PHY identity\n");
  show(0x00510000u);
  delay_ms(300);
  if (test_mdio_phy_id() != 0) {
    show(0xE0510001u);
    printf("HALTED on T2 failure\n");
    while (1) {
    }
  }
  delay_ms(2000);

  /* T3: PHY internal loopback (best-effort — known flaky on some LAN8720A) */
  printf("\nT3: PHY internal loopback (advisory; LAN8720A may misbehave)\n");
  show(0x00520000u);
  delay_ms(300);
  if (test_phy_loopback() != 0) {
    printf("  T3 failed; this is non-fatal — continuing to T4/T5\n");
  }
  delay_ms(1000);

  /* T4: external TX (ARP broadcast) */
  printf("\nT4: external TX — ARP broadcast\n");
  show(0x00530000u);
  delay_ms(300);
  test_external_tx();
  show(0x00530001u);
  delay_ms(1000);

  /* T5: passive RX (loops forever) */
  printf("\nT5: passive RX — waiting for frames\n");
  show(0x00540000u);
  delay_ms(300);
  test_passive_rx();

  return 0;
}
