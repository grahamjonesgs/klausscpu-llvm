/*
 * test_eth.c — KlaussCPU Ethernet bring-up test (no lwIP).
 *
 * Tests the LiteEth MAC hardware in four stages, each gated by a
 * switch so you can stop after any stage:
 *
 *   Stage 1 — Scratch register round-trip (proves MMIO bridge)
 *   Stage 2 — PHY reset + MDIO ID read (proves PHY is alive on RMII)
 *   Stage 3 — Wait for link-up (auto-negotiation complete)
 *   Stage 4 — Send gratuitous ARP; receive-loop for 5 seconds
 *
 * 7-seg display:
 *   Idle between stages:  "ETHO" + stage number on rightmost digit
 *   Stage 1 PASS:         "5Cr4__Xx" (scratch value read back)
 *   Stage 2 PHY ID1:      raw 16-bit PHY ID1 value on lower 4 digits
 *   Stage 3:              "LnK_UP__" on link, "LnK_dn__" while waiting
 *   Stage 4 RX loop:      length of each received frame on lower 4 digits
 *   Any FAIL:             "FAIL" on upper display, error code lower
 *
 * LEDs:   bit 0 set after scratch OK, bit 1 after PHY ID OK,
 *         bit 2 after link up, bit 3 set per frame received.
 * Success after all 4 stages: LEDs = 0x000F, 7-seg = "CAFE0000".
 */

#include <stdio.h>
#include "../mmio.h"
#include "../src/mdio.h"
#include "../src/eth.h"

// ── helpers ───────────────────────────────────────────────────────────────

static void pass(const char *tag) { printf("  [PASS] %s\n", tag); }
static void fail(const char *tag) {
    printf("  [FAIL] %s\n", tag);
    REG_SEG_HIGH = 0xFA1F;   // "FAIL"
}

// Wait up to `timeout_ms` for the PHY BMSR link-up bit to assert.
// Returns 1 if link came up, 0 on timeout.
static int wait_link(uint32_t timeout_ms) {
    uint64_t deadline = REG_CLOCK_MS + timeout_ms;
    while (REG_CLOCK_MS < deadline) {
        uint16_t bmsr = mdio_read(ETH_PHY_ADDR, PHY_REG_BMSR);
        if (bmsr & PHY_BMSR_LINK_UP) return 1;
        delay_ms(50);
        REG_SEG_LOW = 0xdE00;   // "dE  " = "dEAd" blinking indicator
    }
    return 0;
}

// Print a received Ethernet frame summary.
static void print_frame(const uint8_t *f, uint32_t len) {
    uint16_t etype = (uint16_t)((f[12] << 8) | f[13]);
    printf("RX %lu bytes  dst=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx"
           "  etype=0x%04lx\n",
           (unsigned long)len,
           (unsigned long)f[0],  (unsigned long)f[1],  (unsigned long)f[2],
           (unsigned long)f[3],  (unsigned long)f[4],  (unsigned long)f[5],
           (unsigned long)etype);
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU Ethernet bring-up test ===\n\n");
    REG_SEG_ALL = 0xE1000000u;   // "E1" = stage 1
    REG_LEDS    = 0x0000;

    // ── Stage 1: scratch register ─────────────────────────────────────────
    printf("Stage 1: MMIO scratch register\n");

    REG_ETH_CTRL_SCRATCH = 0xCAFEBABEu;
    uint32_t scratch = REG_ETH_CTRL_SCRATCH;
    if (scratch != 0xCAFEBABEu) {
        printf("  scratch = 0x%08lx (expected 0xCAFEBABE)\n",
               (unsigned long)scratch);
        fail("scratch");
        return 1;
    }
    pass("scratch 0xCAFEBABE round-trip");
    REG_LEDS |= 0x0001;

    // ── Stage 2: PHY MDIO ID ──────────────────────────────────────────────
    printf("\nStage 2: PHY identity via MDIO\n");
    REG_SEG_ALL = 0xE2000000u;

    // Reset and release PHY
    REG_ETH_PHY_RESET = 1;
    delay_ms(30);
    REG_ETH_PHY_RESET = 0;
    delay_ms(120);

    uint16_t id1 = mdio_read(ETH_PHY_ADDR, PHY_REG_ID1);
    uint16_t id2 = mdio_read(ETH_PHY_ADDR, PHY_REG_ID2);
    printf("  PHY ID1 = 0x%04lx  (LAN8720A expects 0x%04lx)\n",
           (unsigned long)id1, (unsigned long)LAN8720A_ID1);
    printf("  PHY ID2 = 0x%04lx\n", (unsigned long)id2);

    REG_SEG_LOW = (uint32_t)id1;   // show raw ID1 on lower display

    if (id1 != LAN8720A_ID1) {
        fail("PHY ID1 mismatch — check MDIO wiring / PHY address");
        return 2;
    }
    pass("LAN8720A identified");
    REG_LEDS |= 0x0002;

    // ── Stage 3: link-up ──────────────────────────────────────────────────
    printf("\nStage 3: waiting for link up (5 s timeout)\n");
    REG_SEG_ALL = 0xE3000000u;

    // Enable RX/TX events (polled, no IRQ yet)
    REG_ETH_RX_EV_ENABLE = 1;
    REG_ETH_TX_EV_ENABLE = 1;
    REG_ETH_RX_EV_PENDING = 1;
    REG_ETH_TX_EV_PENDING = 1;

    if (!wait_link(5000)) {
        printf("  BMSR = 0x%04lx\n",
               (unsigned long)mdio_read(ETH_PHY_ADDR, PHY_REG_BMSR));
        fail("link did not come up within 5 s");
        return 3;
    }
    pass("link up");
    REG_SEG_HIGH = 0x1111;   // "LL__" = link
    REG_LEDS |= 0x0004;

    // ── Stage 4: ARP TX + RX loop ─────────────────────────────────────────
    printf("\nStage 4: ARP TX + RX loop (5 s)\n");
    REG_SEG_ALL = 0xE4000000u;

    // 192.168.1.50 in host byte order
    uint32_t our_ip    = (192u << 24) | (168u << 16) | (1u << 8) | 50u;
    uint32_t target_ip = (192u << 24) | (168u << 16) | (1u << 8) | 1u;

    if (eth_send_arp_request(g_eth_mac, our_ip, target_ip) != 0)
        printf("  ARP TX failed (frame too large?)\n");
    else
        printf("  Gratuitous ARP sent (192.168.1.50 who-has 192.168.1.1)\n");

    uint8_t rxbuf[1536];
    uint32_t rx_count = 0;
    uint64_t deadline = REG_CLOCK_MS + 5000;

    while (REG_CLOCK_MS < deadline) {
        uint32_t len = eth_rx_poll(rxbuf, sizeof(rxbuf));
        if (len) {
            rx_count++;
            REG_LEDS |= 0x0008;
            REG_SEG_LOW = len;
            print_frame(rxbuf, len);
        }
    }

    printf("\n  RX frames received: %lu\n", (unsigned long)rx_count);

    if (rx_count == 0)
        printf("  (no frames received — check cable and switch)\n");
    else
        pass("RX frames seen on wire");

    // ── Done ──────────────────────────────────────────────────────────────
    printf("\n=== Ethernet bring-up test complete ===\n");
    printf("LEDs = 0x%04lx  RX count = %lu\n",
           (unsigned long)REG_LEDS, (unsigned long)rx_count);

    REG_SEG_ALL = 0xCAFE0000u;
    while (1) {}
    return 0;
}
