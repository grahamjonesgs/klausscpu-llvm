/*
 * http_server.c — KlaussCPU lwIP HTTP server on port 80.
 *
 * Serves a live HTML status page.  Uses lwIP raw/callback API (NO_SYS=1).
 *
 * Close discipline:
 *   We send the response but do NOT call tcp_close from inside the recv
 *   callback — doing so then calling tcp_recved on the same PCB sends a
 *   RST.  Instead we wait for the client's FIN (p==NULL in http_recv) and
 *   close then.  A 4-second poll timeout cleans up clients that never send
 *   a FIN (e.g. HTTP/1.1 keep-alive that ignores "Connection: close").
 *
 * Test:
 *   curl http://<board-ip>/
 *   Open in a browser: http://<board-ip>/
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
#include "lwip/mem.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

#define HTTP_PORT   80u
#define REQ_BUF    512u
#define POLL_TICKS   8u   /* 8 × 500 ms = 4 s idle timeout */

static struct netif g_netif;

// ── Per-connection state ──────────────────────────────────────────────────

struct http_conn {
    char     req[REQ_BUF];
    uint16_t req_len;
    uint8_t  responded;
};

// ── Static response buffer (safe: NO_SYS=1 is single-threaded) ───────────

static char s_resp[1400];

// ── Response builder ──────────────────────────────────────────────────────

static err_t http_send(struct tcp_pcb *pcb) {
    uint32_t ip     = ntohl(netif_ip4_addr(&g_netif)->addr);
    uint32_t uptime = (uint32_t)(REG_CLOCK_MS / 1000u);
    uint32_t drops  = REG_ETH_RX_ERRORS;
    uint32_t ck_kb  = CACHE_INFO_TOTAL_BYTES(REG_CACHE_INFO) / 1024u;
    uint32_t ck_ways= CACHE_INFO_WAYS(REG_CACHE_INFO);

    int n = snprintf(s_resp, sizeof(s_resp),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width'>"
        "<title>KlaussCPU</title>"
        "<style>"
        "body{font-family:monospace;background:#0d1117;color:#c9d1d9;padding:40px;margin:0}"
        "h1{color:#58a6ff;margin-bottom:6px}"
        "p{color:#8b949e;margin-top:0}"
        "table{border-collapse:collapse;margin-top:16px}"
        "td{padding:6px 24px 6px 0;border-bottom:1px solid #21262d}"
        "td:first-child{color:#8b949e;width:120px}"
        "</style></head><body>"
        "<h1>KlaussCPU</h1>"
        "<p>64-bit RISC CPU &bull; Nexys A7-100T &bull; lwIP %s</p>"
        "<table>"
        "<tr><td>IP</td><td>%lu.%lu.%lu.%lu</td></tr>"
        "<tr><td>Uptime</td><td>%lu s</td></tr>"
        "<tr><td>Cache</td><td>%lu KB, %lu-way, 16 B lines</td></tr>"
        "<tr><td>RX drops</td><td>%lu</td></tr>"
        "</table></body></html>",
        LWIP_VERSION_STRING,
        (unsigned long)((ip>>24)&0xFF), (unsigned long)((ip>>16)&0xFF),
        (unsigned long)((ip>> 8)&0xFF), (unsigned long)( ip     &0xFF),
        (unsigned long)uptime,
        (unsigned long)ck_kb, (unsigned long)ck_ways,
        (unsigned long)drops);

    if (n < 0 || n >= (int)sizeof(s_resp))
        n = (int)sizeof(s_resp) - 1;

    err_t err = tcp_write(pcb, s_resp, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
        tcp_output(pcb);
    return err;
}

// ── TCP callbacks ─────────────────────────────────────────────────────────

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct http_conn *conn = (struct http_conn *)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        tcp_arg(pcb, NULL);   /* clear BEFORE free — tcp_abort/err may call http_err */
        mem_free(conn);
        return err;
    }

    if (!p) {
        /* Client sent FIN after receiving our response — close our side. */
        tcp_arg(pcb, NULL);   /* clear BEFORE free */
        mem_free(conn);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Accumulate request until we have the full headers. */
    if (!conn->responded) {
        for (struct pbuf *q = p; q; q = q->next) {
            uint16_t room = (uint16_t)(REQ_BUF - 1u - conn->req_len);
            uint16_t n    = q->len < room ? (uint16_t)q->len : room;
            if (n) {
                memcpy(conn->req + conn->req_len, q->payload, n);
                conn->req_len += n;
            }
        }
        conn->req[conn->req_len] = '\0';

        if (strstr(conn->req, "\r\n\r\n") || conn->req_len >= REQ_BUF - 1u) {
            conn->responded = 1;
            err_t wr = http_send(pcb);
            if (wr != ERR_OK) {
                /* Write failed — abort.  tcp_abort calls http_err(errf_arg),
                 * so clear arg first to prevent http_err double-freeing. */
                tcp_arg(pcb, NULL);
                mem_free(conn);
                pbuf_free(p);
                tcp_abort(pcb);
                return ERR_ABRT;
            }
        }
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
    struct http_conn *conn = (struct http_conn *)arg;
    if (conn && conn->responded) {
        /* Client hasn't sent FIN within the timeout — abort.
         * tcp_abort calls http_err(errf_arg): clear arg first. */
        tcp_arg(pcb, NULL);
        mem_free(conn);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void http_err(void *arg, err_t err) {
    (void)err;
    struct http_conn *conn = (struct http_conn *)arg;
    if (conn) mem_free(conn);
}

static err_t http_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !new_pcb) return ERR_VAL;

    struct http_conn *conn =
        (struct http_conn *)mem_malloc(sizeof(struct http_conn));
    if (!conn) { tcp_abort(new_pcb); return ERR_MEM; }
    conn->req_len   = 0;
    conn->responded = 0;

    uint32_t rip = ntohl(new_pcb->remote_ip.addr);
    printf("HTTP: %lu.%lu.%lu.%lu:%u\n",
           (unsigned long)((rip>>24)&0xFF), (unsigned long)((rip>>16)&0xFF),
           (unsigned long)((rip>> 8)&0xFF), (unsigned long)( rip     &0xFF),
           (unsigned int)new_pcb->remote_port);

    tcp_setprio(new_pcb, TCP_PRIO_MIN);
    tcp_arg(new_pcb, conn);
    tcp_recv(new_pcb, http_recv);
    tcp_err(new_pcb, http_err);
    tcp_poll(new_pcb, http_poll, POLL_TICKS);
    return ERR_OK;
}

// ── Netif callbacks ───────────────────────────────────────────────────────

static struct tcp_pcb *g_http_pcb = NULL;

static void on_status_change(struct netif *nif) {
    if (netif_ip4_addr(nif)->addr) {
        uint32_t ip = ntohl(netif_ip4_addr(nif)->addr);
        printf("IP  : %lu.%lu.%lu.%lu\n",
               (unsigned long)((ip>>24)&0xFF), (unsigned long)((ip>>16)&0xFF),
               (unsigned long)((ip>> 8)&0xFF), (unsigned long)( ip     &0xFF));
        printf("URL : http://%lu.%lu.%lu.%lu/\n\n",
               (unsigned long)((ip>>24)&0xFF), (unsigned long)((ip>>16)&0xFF),
               (unsigned long)((ip>> 8)&0xFF), (unsigned long)( ip     &0xFF));
        REG_SEG_ALL = netif_ip4_addr(nif)->addr;
        REG_LEDS   |= 0x0004u;

        if (!g_http_pcb) {
            struct tcp_pcb *pcb = tcp_new();
            if (pcb && tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT) == ERR_OK) {
                g_http_pcb = tcp_listen(pcb);
                if (g_http_pcb) {
                    tcp_accept(g_http_pcb, http_accept);
                    printf("HTTP server listening on port %u\n", HTTP_PORT);
                }
            }
        }
    }
}

static void on_link_change(struct netif *nif) {
    printf("link %s\n", netif_is_link_up(nif) ? "UP" : "DOWN");
    if (netif_is_link_up(nif)) REG_LEDS |=  0x0002u;
    else                        REG_LEDS &= ~0x0002u;
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU HTTP server ===\n\n");
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
            if (netif_ip4_addr(&g_netif)->addr)
                printf("[t=%lu s] up  drops=%lu\n",
                       (unsigned long)(now/1000u), (unsigned long)drop_count);
        }
    }

    return 0;
}
