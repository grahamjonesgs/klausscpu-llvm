/*
 * ping_demo.c — KlaussCPU lwIP ICMP ping test.
 *
 * Static IP configuration — no DHCP.  Connect the Nexys A7 directly to a
 * Raspberry Pi 4 (or any Linux host) without a switch.
 *
 * FPGA side:  192.168.1.99 / 255.255.255.0
 * Pi side:    sudo ip addr add 192.168.1.1/24 dev eth0
 *             ping 192.168.1.99
 *
 * lwIP handles ARP and ICMP echo reply automatically once the netif is up.
 * No application-level code is needed for ping — just run the polling loop.
 *
 * 7-seg:
 *   "INIT    " during startup
 *   lower 8 digits = IP address in hex once up (e.g. "C0A80163" = 192.168.1.99)
 *
 * LEDs:
 *   bit 0: eth_init complete
 *   bit 1: lwIP netif up
 *   bit 2: first ARP request seen on wire (ARP table active)
 *   bit 3: blinks on every received frame
 */

#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

// ── Network configuration ─────────────────────────────────────────────────
// Adjust these to match your bench setup.

#define FPGA_IP0  192
#define FPGA_IP1  168
#define FPGA_IP2    1
#define FPGA_IP3   99

#define FPGA_NM0  255
#define FPGA_NM1  255
#define FPGA_NM2  255
#define FPGA_NM3    0

#define FPGA_GW0  192
#define FPGA_GW1  168
#define FPGA_GW2    1
#define FPGA_GW3    1   // Pi's IP

static struct netif g_netif;

// ── Netif callbacks ───────────────────────────────────────────────────────

static void on_link_change(struct netif *nif) {
    if (netif_is_link_up(nif)) {
        printf("netif: link UP\n");
        REG_LEDS |= 0x0002u;
    } else {
        printf("netif: link DOWN\n");
        REG_LEDS &= ~0x0002u;
    }
}

static void on_status_change(struct netif *nif) {
    if (nif->ip_addr.addr) {
        uint32_t ip = ntohl(nif->ip_addr.addr);
        printf("netif: IP assigned %lu.%lu.%lu.%lu\n",
               (unsigned long)((ip >> 24) & 0xFFu),
               (unsigned long)((ip >> 16) & 0xFFu),
               (unsigned long)((ip >>  8) & 0xFFu),
               (unsigned long)( ip        & 0xFFu));
        REG_SEG_ALL = nif->ip_addr.addr;   // show IP on 7-seg (big-endian on wire)
    }
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU lwIP ping demo ===\n");
    printf("FPGA IP : %d.%d.%d.%d\n", FPGA_IP0, FPGA_IP1, FPGA_IP2, FPGA_IP3);
    printf("Gateway : %d.%d.%d.%d\n", FPGA_GW0, FPGA_GW1, FPGA_GW2, FPGA_GW3);
    printf("Pi cmd  : sudo ip addr add %d.%d.%d.%d/24 dev eth0\n",
           FPGA_GW0, FPGA_GW1, FPGA_GW2, FPGA_GW3);
    printf("\n");

    REG_SEG_ALL = 0x1E110000u;   // "INIT"
    REG_LEDS    = 0x0000u;

    // 1. Bring up physical layer: PHY reset, AN, link poll.
    eth_init();
    REG_LEDS |= 0x0001u;

    // 2. Initialise lwIP.
    lwip_init();

    // 3. Add the network interface with a static IP.
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  FPGA_IP0, FPGA_IP1, FPGA_IP2, FPGA_IP3);
    IP4_ADDR(&netmask, FPGA_NM0, FPGA_NM1, FPGA_NM2, FPGA_NM3);
    IP4_ADDR(&gw,      FPGA_GW0, FPGA_GW1, FPGA_GW2, FPGA_GW3);

    netif_add(&g_netif, &ipaddr, &netmask, &gw,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&g_netif);

    netif_set_link_callback  (&g_netif, on_link_change);
    netif_set_status_callback(&g_netif, on_status_change);

#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&g_netif, "klausscpu");
#endif

    // Mark the interface and link as up — eth_init() already confirmed link.
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);   // triggers on_link_change + on_status_change

    printf("\nlwIP up. Run: ping %d.%d.%d.%d\n\n",
           FPGA_IP0, FPGA_IP1, FPGA_IP2, FPGA_IP3);

    // 4. Polling loop — ethernetif_input delivers frames to the IP layer;
    //    sys_check_timeouts fires ARP/ICMP/TCP retransmit timers.
    uint32_t rx_count   = 0;
    uint32_t last_diag  = 0;
    uint32_t last_errors = 0;

    for (;;) {
        ethernetif_input(&g_netif);
        sys_check_timeouts();

        // Toggle LED3 on each received frame so you can see traffic.
        uint32_t new_rx = REG_ETH_RX_ERRORS;   // proxy: rises on any drop
        if (new_rx != last_errors) {
            last_errors = new_rx;
            REG_LEDS |= 0x0004u;               // RX drop detected
            printf("warn: RX_ERRORS=%lu\n", (unsigned long)new_rx);
        }

        // Periodic status line every 10 s.
        uint32_t now = (uint32_t)REG_CLOCK_MS;
        if ((now - last_diag) >= 10000u) {
            last_diag = now;
            printf("[t=%lu s] RX frames processed: %lu  RX_ERRORS: %lu\n",
                   (unsigned long)(now / 1000u),
                   (unsigned long)rx_count,
                   (unsigned long)REG_ETH_RX_ERRORS);
        }
    }

    return 0;
}
