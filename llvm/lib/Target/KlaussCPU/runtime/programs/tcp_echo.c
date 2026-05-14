/*
 * tcp_echo.c — KlaussCPU lwIP TCP echo server (port 7) with DHCP.
 *
 * After acquiring an IP via DHCP, listens on TCP port 7 and echoes
 * back every byte received.  Responds to ping simultaneously.
 * Uses the lwIP raw/callback API (NO_SYS=1).
 *
 * Test from the Pi:
 *   echo "hello KlaussCPU" | nc <board-ip> 7
 *   nc <board-ip> 7    # interactive — type lines, see them echoed back
 *
 * 7-seg:  IP address once leased
 * LEDs:   bit0=init  bit1=link  bit2=IP leased  bit3=RX drops
 */

#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

#define ECHO_PORT  7u

static struct netif g_netif;

// ── TCP echo callbacks ────────────────────────────────────────────────────

static err_t echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        return err;
    }

    if (p == NULL) {
        /* Remote side closed — close our end too. */
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Walk the pbuf chain and echo each segment. */
    err_t wr_err = ERR_OK;
    for (struct pbuf *q = p; q != NULL && wr_err == ERR_OK; q = q->next)
        wr_err = tcp_write(pcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);

    if (wr_err == ERR_OK)
        tcp_output(pcb);

    /* Acknowledge the received data regardless of write result. */
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void echo_err(void *arg, err_t err) {
    (void)arg;
    printf("TCP echo: connection reset (%d)\n", (int)err);
}

static err_t echo_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || new_pcb == NULL) return ERR_VAL;

    uint32_t rip = ntohl(new_pcb->remote_ip.addr);
    printf("TCP echo: connect from %lu.%lu.%lu.%lu:%u\n",
           (unsigned long)((rip>>24)&0xFF), (unsigned long)((rip>>16)&0xFF),
           (unsigned long)((rip>> 8)&0xFF), (unsigned long)( rip     &0xFF),
           (unsigned int)new_pcb->remote_port);

    tcp_setprio(new_pcb, TCP_PRIO_MIN);
    tcp_arg(new_pcb, NULL);
    tcp_recv(new_pcb, echo_recv);
    tcp_err(new_pcb, echo_err);
    return ERR_OK;
}

static struct tcp_pcb *echo_server_init(uint16_t port) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { printf("tcp_new failed\n"); return NULL; }

    if (tcp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        printf("tcp_bind failed\n");
        tcp_abort(pcb);
        return NULL;
    }

    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        printf("tcp_listen failed\n");
        tcp_abort(pcb);
        return NULL;
    }

    tcp_accept(lpcb, echo_accept);
    printf("TCP echo server on port %u\n", (unsigned int)port);
    return lpcb;
}

// ── Netif callbacks ───────────────────────────────────────────────────────

static struct tcp_pcb *g_echo_pcb = NULL;

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
        REG_SEG_ALL = nif->ip_addr.addr;
        REG_LEDS   |= 0x0004u;

        /* Start the echo server once we have an address. */
        if (!g_echo_pcb)
            g_echo_pcb = echo_server_init(ECHO_PORT);
    }
}

static void on_link_change(struct netif *nif) {
    if (netif_is_link_up(nif)) {
        printf("link UP\n");
        REG_LEDS |= 0x0002u;
    } else {
        printf("link DOWN\n");
        REG_LEDS &= ~0x0002u;
    }
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU lwIP TCP echo server ===\n\n");
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

    uint32_t last_diag   = 0;
    uint32_t last_errors = 0;
    uint32_t drop_count  = 0;

    for (;;) {
        while (REG_ETH_RX_EV_PENDING & 1u)
            ethernetif_input(&g_netif);

        sys_check_timeouts();

        uint32_t errs = REG_ETH_RX_ERRORS;
        if (errs != last_errors) {
            drop_count += errs - last_errors;
            last_errors  = errs;
            REG_LEDS |= 0x0008u;
        }

        uint32_t now = (uint32_t)REG_CLOCK_MS;
        if ((now - last_diag) >= 5000u) {
            last_diag = now;
            if (!g_netif.ip_addr.addr) {
                struct dhcp *d = netif_dhcp_data(&g_netif);
                printf("[t=%lu s] DHCP state=%u tries=%u  drops=%lu\n",
                       (unsigned long)(now/1000u),
                       (unsigned int)(d ? d->state : 0),
                       (unsigned int)(d ? d->tries : 0),
                       (unsigned long)drop_count);
            } else {
                printf("[t=%lu s] up  drops=%lu\n",
                       (unsigned long)(now/1000u), (unsigned long)drop_count);
            }
        }
    }

    return 0;
}
