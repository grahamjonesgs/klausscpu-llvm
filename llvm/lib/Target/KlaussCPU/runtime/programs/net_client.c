/*
 * net_client.c — KlaussCPU outbound network client demo.
 *
 * Uses lwIP built-in app modules:
 *   - lwip/apps/sntp  : queries pool.ntp.org, re-syncs every 60 s.
 *   - lwip/apps/http_client : GETs http://httpbin.org/ip once after sync.
 *
 * On SNTP sync, clock_set_epoch() is called so time()/gmtime()/strftime()
 * work everywhere via the gettimeofday() hook in syscalls.c.
 *
 * 7-seg: assigned IP
 * LEDs:  bit0=init  bit1=link  bit2=IP  bit3=NTP synced
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

static void print_tm(const struct tm *t) {
    printf("UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
}
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/altcp.h"
#include "lwip/apps/sntp.h"
#include "lwip/apps/http_client.h"
#include "netif/ethernet.h"
#include "../mmio.h"
#include "../src/eth.h"
#include "../lwip_port/ethernetif.h"

#define HTTP_HOST    "httpbin.org"
#define HTTP_PORT    80u
#define HTTP_URI     "/ip"

/* defined in syscalls.c — sets the epoch for time()/gmtime()/strftime() */
extern void clock_set_epoch(long long unix_sec);

// ── State ─────────────────────────────────────────────────────────────────

static struct netif g_netif;
static uint8_t      g_synced    = 0;   /* 0=no sync, 1=synced */
static uint8_t      g_http_done = 0;   /* 0=idle 2=pending 3=started */

// ── SNTP callback ─────────────────────────────────────────────────────────
// Declared in lwipopts.h:  void app_sntp_set_time(unsigned int sec);
// Wired via:  #define SNTP_SET_SYSTEM_TIME(sec)  app_sntp_set_time(sec)

void app_sntp_set_time(unsigned int sec) {
    clock_set_epoch((long long)sec);
    g_synced = 1;
    REG_LEDS |= 0x0008u;

    time_t t = (time_t)sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    printf("SNTP: synced  ");
    print_tm(&tm);

    if (!g_http_done)
        g_http_done = 2;
}

// ── HTTP client callbacks ─────────────────────────────────────────────────

static err_t httpc_body_recv(void *arg, struct altcp_pcb *conn,
                              struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return ERR_OK;
    if (err != ERR_OK) { pbuf_free(p); return err; }
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = (const uint8_t *)q->payload;
        for (uint16_t i = 0; i < q->len; i++)
            putchar(d[i]);
    }
    altcp_recved(conn, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void httpc_done(void *arg, httpc_result_t result,
                        u32_t content_len, u32_t srv_res, err_t err) {
    (void)arg; (void)content_len; (void)err;
    printf("\nHTTP: result=%d  status=%lu\n",
           (int)result, (unsigned long)srv_res);
}

// ── Netif callbacks ───────────────────────────────────────────────────────

static httpc_connection_t s_http_settings;   /* zero-init in BSS */

static void on_status_change(struct netif *nif) {
    if (netif_ip4_addr(nif)->addr) {
        uint32_t ip = ntohl(netif_ip4_addr(nif)->addr);
        printf("IP  : %lu.%lu.%lu.%lu\n",
               (unsigned long)((ip>>24)&0xFF), (unsigned long)((ip>>16)&0xFF),
               (unsigned long)((ip>> 8)&0xFF), (unsigned long)( ip     &0xFF));
        REG_SEG_ALL = netif_ip4_addr(nif)->addr;
        REG_LEDS   |= 0x0004u;

        sntp_setservername(0, "pool.ntp.org");
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_init();
        printf("SNTP started → pool.ntp.org (re-sync every 60 s)\n");
    }
}

static void on_link_change(struct netif *nif) {
    printf("link %s\n", netif_is_link_up(nif) ? "UP" : "DOWN");
    if (netif_is_link_up(nif)) REG_LEDS |=  0x0002u;
    else                        REG_LEDS &= ~0x0002u;
}

// ── main ──────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== KlaussCPU network client (SNTP + HTTP GET) ===\n\n");
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

    s_http_settings.result_fn = httpc_done;

    uint32_t last_diag = 0;

    for (;;) {
        while (REG_ETH_RX_EV_PENDING & 1u)
            ethernetif_input(&g_netif);

        sys_check_timeouts();   /* drives SNTP retry + re-sync timer */

        uint32_t now = (uint32_t)REG_CLOCK_MS;

        /* Fire one HTTP GET after first SNTP sync. */
        if (g_http_done == 2) {
            g_http_done = 3;
            httpc_state_t *state;
            err_t e = httpc_get_file_dns(HTTP_HOST, HTTP_PORT, HTTP_URI,
                                          &s_http_settings,
                                          httpc_body_recv, NULL, &state);
            printf("HTTP GET http://%s%s  (%s)\n", HTTP_HOST, HTTP_URI,
                   e == ERR_OK ? "started" : "error");
        }

        /* Periodic UTC display using standard time functions. */
        if ((now - last_diag) >= 10000u) {
            last_diag = now;
            if (g_synced) {
                time_t t = time(NULL);
                struct tm tm;
                gmtime_r(&t, &tm);
                print_tm(&tm);
            } else if (!netif_ip4_addr(&g_netif)->addr) {
                printf("[t=%lu s] waiting for DHCP...\n",
                       (unsigned long)(now / 1000u));
            }
        }
    }

    return 0;
}
