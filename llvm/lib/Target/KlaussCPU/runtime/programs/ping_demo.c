/*
 * ping_demo.c — KlaussCPU lwIP ping demo with DHCP.
 *
 * Connects to a switch, acquires an IP via DHCP, then responds to ping.
 *
 * Key polling discipline:
 *   LiteEth has 2 RX slots.  If we drain only one slot per loop iteration
 *   and a new frame arrives into the just-freed slot before the next iteration,
 *   the third frame is dropped (RX_ERRORS++).  On a busy switch (ARP, mDNS,
 *   LLDP, STP) this reliably drops DHCP OFFERs.  Fix: drain ALL pending slots
 *   before running timers.  Also: never printf inside the drain loop — UART
 *   transmission blocks for milliseconds and causes the same problem.
 *
 * 7-seg:  lower 8 hex digits = assigned IP once leased
 * LEDs:   bit0=init  bit1=link  bit2=IP leased  bit3=RX drops seen
 */

#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"    /* DHCP_STATE_* constants */
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

static struct netif g_netif;

// ── Netif callbacks ───────────────────────────────────────────────────────

static void on_link_change(struct netif *nif) {
    if (netif_is_link_up(nif)) {
        printf("link UP\n");
        REG_LEDS |= 0x0002u;
    } else {
        printf("link DOWN\n");
        REG_LEDS &= ~0x0002u;
    }
}

static void on_status_change(struct netif *nif) {
    if (nif->ip_addr.addr) {
        uint32_t ip = ntohl(nif->ip_addr.addr);
        uint32_t nm = ntohl(nif->netmask.addr);
        uint32_t gw = ntohl(nif->gw.addr);
        printf("IP  : %lu.%lu.%lu.%lu\n",
               (unsigned long)((ip>>24)&0xFF), (unsigned long)((ip>>16)&0xFF),
               (unsigned long)((ip>> 8)&0xFF), (unsigned long)( ip     &0xFF));
        printf("mask: %lu.%lu.%lu.%lu\n",
               (unsigned long)((nm>>24)&0xFF), (unsigned long)((nm>>16)&0xFF),
               (unsigned long)((nm>> 8)&0xFF), (unsigned long)( nm     &0xFF));
        printf("GW  : %lu.%lu.%lu.%lu\n",
               (unsigned long)((gw>>24)&0xFF), (unsigned long)((gw>>16)&0xFF),
               (unsigned long)((gw>> 8)&0xFF), (unsigned long)( gw     &0xFF));
        printf("ping from any host on the subnet\n\n");
        REG_SEG_ALL = nif->ip_addr.addr;
        REG_LEDS   |= 0x0004u;
    }
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU lwIP ping demo (DHCP) ===\n\n");
    REG_SEG_ALL = 0x1E110000u;
    REG_LEDS    = 0x0000u;

    eth_init();
    REG_LEDS |= 0x0001u;

    lwip_init();

    ip4_addr_t zero = { 0 };
    netif_add(&g_netif, &zero, &zero, &zero,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_link_callback  (&g_netif, on_link_change);
    netif_set_status_callback(&g_netif, on_status_change);
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&g_netif, "klausscpu");
#endif
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    printf("Starting DHCP...\n");
    dhcp_start(&g_netif);

    // ── Polling loop ──────────────────────────────────────────────────────
    // Drain ALL pending RX slots on every iteration, then run timers.
    // Never print inside the drain loop — UART latency causes slot overflows.

    uint32_t last_diag   = 0;
    uint32_t last_errors = 0;
    uint32_t drop_count  = 0;

    for (;;) {
        // Drain every available frame before running timers.
        while (REG_ETH_RX_EV_PENDING & 1u)
            ethernetif_input(&g_netif);

        sys_check_timeouts();

        // Track drops without printing (printing here causes more drops).
        uint32_t errs = REG_ETH_RX_ERRORS;
        if (errs != last_errors) {
            drop_count += errs - last_errors;
            last_errors  = errs;
            REG_LEDS |= 0x0008u;
        }

        // Periodic status — safe to print here, outside the drain loop.
        uint32_t now = (uint32_t)REG_CLOCK_MS;
        if ((now - last_diag) >= 5000u) {
            last_diag = now;

            struct dhcp *d = netif_dhcp_data(&g_netif);
            uint8_t state  = d ? d->state : 0;
            uint8_t tries  = d ? d->tries : 0;

            if (!g_netif.ip_addr.addr) {
                // Map state number to a name for easy reading.
                const char *sname = "?";
                switch (state) {
                case DHCP_STATE_OFF:       sname = "OFF";       break;
                case DHCP_STATE_SELECTING: sname = "SELECTING"; break;
                case DHCP_STATE_REQUESTING:sname = "REQUESTING";break;
                case DHCP_STATE_BOUND:     sname = "BOUND";     break;
                case DHCP_STATE_RENEWING:  sname = "RENEWING";  break;
                case DHCP_STATE_REBINDING: sname = "REBINDING"; break;
                default:                   sname = "OTHER";     break;
                }
                printf("[t=%lu s] DHCP state=%s tries=%u  drops=%lu\n",
                       (unsigned long)(now/1000u), sname,
                       (unsigned int)tries, (unsigned long)drop_count);
            } else {
                printf("[t=%lu s] up  drops=%lu\n",
                       (unsigned long)(now/1000u), (unsigned long)drop_count);
            }
        }
    }

    return 0;
}
