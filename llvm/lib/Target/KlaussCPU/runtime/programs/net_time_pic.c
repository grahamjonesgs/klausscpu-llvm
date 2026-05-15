/*
 * net_time_pic.c — PIC loadable program: DHCP + SNTP → print UTC time.
 *
 * Compiled with -fPIC and linked into a PIC ELF.  Includes the full lwIP
 * stack + Ethernet driver; no shared state with the loader.
 *
 * Flow:
 *   1. Initialise Ethernet hardware (PHY reset + auto-negotiation)
 *   2. Init lwIP, add netif, request DHCP
 *   3. Poll until IP assigned (up to 30 s)
 *   4. Start SNTP against pool.ntp.org
 *   5. Poll until time synced (up to 60 s)
 *   6. Print UTC time with printf and return 0
 *
 * SD card file: PROG.ELF (loader auto-detects ELF magic).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/apps/sntp.h"
#include "netif/ethernet.h"

#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

/* ── State ─────────────────────────────────────────────────────────────── */

static struct netif g_netif;
static volatile int g_time_set  = 0;
static time_t       g_unix_time = 0;

/* ── SNTP callback ──────────────────────────────────────────────────────── */
/* Declared in lwipopts.h:  void app_sntp_set_time(unsigned int sec);
 * Wired via:  #define SNTP_SET_SYSTEM_TIME(sec)  app_sntp_set_time(sec)  */

void app_sntp_set_time(unsigned int sec) {
    g_unix_time = (time_t)sec;
    g_time_set  = 1;
}

/* ── Poll both Ethernet and lwIP timers ─────────────────────────────────── */

static void net_poll(void) {
    /* Drain all pending received frames. */
    while (REG_ETH_RX_EV_PENDING & 1u)
        ethernetif_input(&g_netif);
    sys_check_timeouts();
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== net_time: starting ===\n");

    /* Initialise Ethernet hardware (PHY reset takes ~150 ms). */
    eth_init();
    printf("net_time: Ethernet initialised\n");

    /* Initialise lwIP and add the Ethernet interface. */
    lwip_init();

    ip4_addr_t zero = {0};
    netif_add(&g_netif, &zero, &zero, &zero,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    /* Start DHCP. */
    dhcp_start(&g_netif);
    printf("net_time: DHCP starting...\n");

    /* Wait up to 30 s for an IP address. */
    unsigned long start = (unsigned long)REG_CLOCK_MS;
    while (!dhcp_supplied_address(&g_netif)) {
        net_poll();
        if ((unsigned long)REG_CLOCK_MS - start > 30000UL) {
            printf("ERROR: DHCP timeout after 30 s\n");
            return 1;
        }
    }

    /* Print assigned IP. */
    uint32_t ip = ntohl(netif_ip4_addr(&g_netif)->addr);
    printf("net_time: IP = %lu.%lu.%lu.%lu\n",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)( ip        & 0xFF));

    /* Start SNTP. */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    printf("net_time: SNTP started → pool.ntp.org\n");

    /* Wait up to 60 s for time sync. */
    start = (unsigned long)REG_CLOCK_MS;
    while (!g_time_set) {
        net_poll();
        if ((unsigned long)REG_CLOCK_MS - start > 60000UL) {
            printf("ERROR: SNTP timeout after 60 s\n");
            return 1;
        }
    }

    /* Print the time. */
    struct tm t;
    gmtime_r(&g_unix_time, &t);
    printf("net_time: UTC = %04d-%02d-%02d %02d:%02d:%02d\n",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);

    printf("=== net_time: done ===\n");
    return 0;
}
