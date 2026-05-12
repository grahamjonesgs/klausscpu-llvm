// eth.c — KlaussCPU LiteEth raw L2 driver.

#include "../mmio.h"
#include "mdio.h"
#include "eth.h"
#include <string.h>
#include <stdio.h>

const uint8_t g_eth_mac[6] = ETH_MAC_ADDR;

// Always transmit from slot 0; see ethernetif.c low_level_output comment.
// TX_LEVEL==0 guard prevents double-transmission from the 2-entry TX FIFO.

// ── Volatile SRAM helpers ─────────────────────────────────────────────────
// Standard memcpy won't accept volatile pointers; these loops do.

static void sram_read(void *dst, volatile const uint8_t *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = src[i];
}

static void sram_write(volatile uint8_t *dst, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) dst[i] = s[i];
}

// ── PHY initialisation ────────────────────────────────────────────────────

void eth_init(void) {
    // 0. Soft-reset the LiteEth core (persists across CPU_RESETN).
    REG_ETH_CTRL_RESET = 1;
    delay_ms(10);
    REG_ETH_CTRL_RESET = 0;

    // 1. Assert PHY hardware reset (LAN8720A needs ≥ 25 ms).
    REG_ETH_PHY_RESET = 1;
    delay_ms(30);
    REG_ETH_PHY_RESET = 0;
    delay_ms(50);    // PLL lock

    // 2. Verify PHY identity.
    uint16_t id1 = mdio_read(ETH_PHY_ADDR, PHY_REG_ID1);
    printf("PHY ID1 = 0x%04lx (expect 0x%04lx)\n",
           (unsigned long)id1, (unsigned long)LAN8720A_ID1);

    // 4. Enable AN, advertise 100M FD, restart AN.
    //    BMCR = 0x3300: AN-enable | 100M | restart-AN | full-duplex.
    //    Forced speed without AN causes RMII rate mismatch in LiteEth.
    mdio_write(ETH_PHY_ADDR, PHY_REG_BMCR, 0x3300u);

    // 5. Poll for AN-complete + link-up (up to 3 s).
    printf("Waiting for link...\n");
    for (int i = 0; i < 30; i++) {
        delay_ms(100);
        uint16_t bmsr = mdio_read(ETH_PHY_ADDR, PHY_REG_BMSR);
        if (bmsr != 0xFFFFu &&
            (bmsr & (PHY_BMSR_AN_COMPLETE | PHY_BMSR_LINK_UP)) ==
                    (PHY_BMSR_AN_COMPLETE | PHY_BMSR_LINK_UP)) {
            printf("Link up (BMSR=0x%04lx)\n", (unsigned long)bmsr);
            break;
        }
        if (i == 29)
            printf("Link timeout (BMSR=0x%04lx)\n", (unsigned long)bmsr);
    }

    // 6. Enable RX/TX events; drain stale flags from boot transients.
    REG_ETH_RX_EV_ENABLE = 1;
    REG_ETH_TX_EV_ENABLE = 1;
    REG_ETH_RX_EV_PENDING = 1;
    REG_ETH_TX_EV_PENDING = 1;
}

// ── TX ────────────────────────────────────────────────────────────────────

int eth_tx(const void *frame, uint32_t len) {
    if (len < 14 || len > ETH_MAX_FRAME) return -1;

    while (!REG_ETH_TX_READY) {}
    sram_write(ETH_TX_SLOT_PTR(0), frame, len);
    REG_ETH_TX_SLOT   = 0;
    REG_ETH_TX_LENGTH = len;
    REG_ETH_TX_START  = 1;
    return 0;
}

// ── RX ────────────────────────────────────────────────────────────────────

uint32_t eth_rx_poll(void *buf, uint32_t max) {
    if (!(REG_ETH_RX_EV_PENDING & 1u)) return 0;

    uint32_t slot = REG_ETH_RX_SLOT;
    uint32_t len  = REG_ETH_RX_LENGTH;

    if (len == 0 || len > ETH_SLOT_SIZE) {
        REG_ETH_RX_EV_PENDING = 1;   // W1C — discard bad frame
        return 0;
    }
    if (len > max) len = max;

    sram_read(buf, ETH_RX_SLOT_PTR(slot), len);
    REG_ETH_RX_EV_PENDING = 1;   // W1C — release slot to HW
    return len;
}

// ── ARP request helper ────────────────────────────────────────────────────
// Builds and sends a 42-byte ARP request (who-has target_ip, tell our_ip).
// IPs are in HOST byte order; htonl() converts to network order in the frame.

int eth_send_arp_request(const uint8_t our_mac[6],
                         uint32_t our_ip, uint32_t target_ip) {
    uint8_t f[42];

    // Ethernet header
    memset(&f[0], 0xFF, 6);          // dst = broadcast
    memcpy(&f[6], our_mac, 6);       // src = our MAC
    f[12] = 0x08; f[13] = 0x06;      // ethertype = ARP

    // ARP payload
    f[14] = 0x00; f[15] = 0x01;      // HTYPE = Ethernet
    f[16] = 0x08; f[17] = 0x00;      // PTYPE = IPv4
    f[18] = 6;    f[19] = 4;         // HLEN = 6, PLEN = 4
    f[20] = 0x00; f[21] = 0x01;      // OPER = request

    memcpy(&f[22], our_mac, 6);      // sender MAC
    uint32_t our_ip_be = htonl(our_ip);
    memcpy(&f[28], &our_ip_be, 4);   // sender IP

    memset(&f[32], 0, 6);            // target MAC = unknown
    uint32_t tgt_ip_be = htonl(target_ip);
    memcpy(&f[38], &tgt_ip_be, 4);   // target IP

    return eth_tx(f, 42);
}
