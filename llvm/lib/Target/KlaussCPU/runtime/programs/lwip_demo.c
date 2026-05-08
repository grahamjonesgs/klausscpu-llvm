/*
 * lwip_demo.c — KlaussCPU lwIP stack demo (NO_SYS=1).
 *
 * Brings up the full TCP/IP stack:
 *   - DHCP (falls back to static 192.168.1.50 after 10 s)
 *   - ICMP echo (responds to ping automatically)
 *   - TCP echo server on port 7 (RFC 862 — echoes every byte back)
 *
 * 7-seg display:
 *   Startup:         "INIT____"
 *   DHCP in progress:"dHCP____"
 *   IP assigned:     lower 8 hex digits = IP address (e.g. C0A80132 = 192.168.1.50)
 *   Per connection:  upper 4 = "Conn", lower 4 = connection count
 *
 * LEDs:
 *   bit 0: lwIP initialised
 *   bit 1: link up
 *   bit 2: IP address assigned
 *   bit 3: TCP connection active
 *
 * Run with `tcpdump -i <iface> host 192.168.1.50` on a PC to observe traffic.
 * Ping with: ping 192.168.1.50
 * TCP echo:  nc 192.168.1.50 7
 */

#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"   /* ethernet_input() */
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

// ── Network configuration ─────────────────────────────────────────────────
// Used as fallback if DHCP does not respond within DHCP_TIMEOUT_MS.

#define DHCP_TIMEOUT_MS  10000u

// Fallback static IP: 192.168.1.50 / 255.255.255.0 / GW 192.168.1.1
#define STATIC_IP   IPADDR4_INIT_BYTES(192, 168, 1, 50)
#define STATIC_NM   IPADDR4_INIT_BYTES(255, 255, 255, 0)
#define STATIC_GW   IPADDR4_INIT_BYTES(192, 168, 1, 1)

// ── Global state ──────────────────────────────────────────────────────────

static struct netif g_netif;
static volatile int g_ip_assigned = 0;
static volatile int g_conn_count  = 0;

// ── Netif status callback — called when IP is assigned / removed ──────────

static void netif_status_cb(struct netif *nif) {
    if (nif->ip_addr.addr != 0) {
        uint32_t ip = ntohl(nif->ip_addr.addr);
        printf("IP assigned: %lu.%lu.%lu.%lu\n",
               (unsigned long)((ip >> 24) & 0xFF),
               (unsigned long)((ip >> 16) & 0xFF),
               (unsigned long)((ip >>  8) & 0xFF),
               (unsigned long)( ip        & 0xFF));
        REG_SEG_ALL = nif->ip_addr.addr;   // display raw IP on 7-seg
        REG_LEDS   |= 0x0004;
        g_ip_assigned = 1;
    } else {
        printf("IP removed\n");
        g_ip_assigned = 0;
        REG_LEDS &= ~0x0004u;
    }
}

// ── TCP echo server (port 7) ──────────────────────────────────────────────

static err_t echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        tcp_close(pcb);
        if (p) pbuf_free(p);
        REG_LEDS &= ~0x0008u;
        return ERR_OK;
    }
    // Echo all received data back
    err_t wr_err = tcp_write(pcb, p->payload, p->len, TCP_WRITE_FLAG_COPY);
    if (wr_err == ERR_OK) tcp_output(pcb);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void echo_err(void *arg, err_t err) {
    (void)arg; (void)err;
    REG_LEDS &= ~0x0008u;
}

static err_t echo_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || new_pcb == NULL) return ERR_VAL;

    g_conn_count++;
    printf("TCP echo: connection #%d accepted\n", g_conn_count);
    REG_LEDS |= 0x0008;
    REG_SEG_ALL = (uint32_t)(0xC0E00000u | (uint32_t)g_conn_count);

    tcp_setprio(new_pcb, TCP_PRIO_MIN);
    tcp_recv(new_pcb, echo_recv);
    tcp_err(new_pcb, echo_err);
    return ERR_OK;
}

static void start_echo_server(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { printf("tcp_new failed\n"); return; }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, 7);
    if (err != ERR_OK) { printf("tcp_bind failed: %d\n", (int)err); return; }

    struct tcp_pcb *listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) { printf("tcp_listen failed\n"); return; }

    tcp_accept(listen_pcb, echo_accept);
    printf("TCP echo server listening on port 7\n");
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU lwIP demo (NO_SYS=1) ===\n");
    REG_SEG_ALL = 0x1E110000u;   // "INIT"
    REG_LEDS    = 0x0000;

    // 1. Initialise LiteEth hardware (PHY reset + MDIO verify + enable events)
    eth_init();

    // 2. Initialise lwIP
    lwip_init();
    REG_LEDS |= 0x0001;

    // 3. Add the network interface
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  0, 0, 0, 0);   // zero = let DHCP fill in
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw,      0, 0, 0, 0);

    netif_add(&g_netif, &ipaddr, &netmask, &gw,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_status_callback(&g_netif, netif_status_cb);
    netif_set_up(&g_netif);
    REG_LEDS |= 0x0002;

    // 4. Start DHCP
    printf("Starting DHCP...\n");
    REG_SEG_ALL = 0xEEC90000u;   // stage indicator while DHCP runs
    dhcp_start(&g_netif);

    uint64_t dhcp_deadline = REG_CLOCK_MS + DHCP_TIMEOUT_MS;

    // 5. Main loop — poll until IP assigned or DHCP times out
    while (!g_ip_assigned && REG_CLOCK_MS < dhcp_deadline) {
        ethernetif_input(&g_netif);
        sys_check_timeouts();
    }

    if (!g_ip_assigned) {
        printf("DHCP timeout — using static fallback\n");
        dhcp_stop(&g_netif);
        ip4_addr_t sip = STATIC_IP, snm = STATIC_NM, sgw = STATIC_GW;
        netif_set_addr(&g_netif, &sip, &snm, &sgw);
    }

    // 6. Start the TCP echo server
    start_echo_server();

    printf("\nlwIP stack running. Ping or connect to port 7 to test.\n\n");

    // 7. Run forever — poll for frames and service timers
    for (;;) {
        ethernetif_input(&g_netif);
        sys_check_timeouts();
    }

    return 0;
}
