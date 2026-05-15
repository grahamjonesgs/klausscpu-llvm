/*
 * net_demo.c — FreeRTOS + lwIP network demo for KlaussCPU.
 *
 * Three concurrent tasks, each blocking independently:
 *
 *   netinit_task  — initialises Ethernet + lwIP, starts DHCP, prints IP.
 *                   Sets NET_READY_BIT in g_net_events when IP is assigned.
 *
 *   ntp_task      — waits for NET_READY_BIT, starts SNTP against pool.ntp.org,
 *                   blocks on a semaphore until app_sntp_set_time() fires,
 *                   then prints UTC time.
 *
 *   http_task     — waits for NET_READY_BIT, opens a TCP socket to
 *                   httpbin.org:80, sends "GET /get HTTP/1.0", receives and
 *                   prints the first 512 bytes of the response.
 *
 * Ethernet polling runs in a dedicated eth_input_task that drains the LiteEth
 * RX SRAM every millisecond and hands frames to the tcpip_thread via
 * tcpip_input().
 *
 * lwIP initialised with tcpip_init() (threaded mode, NO_SYS=0).
 * netif_add() receives tcpip_input — the tcpip_thread owns all stack state.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "event_groups.h"

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/apps/sntp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "netif/ethernet.h"

#include "../lwip_port/ethernetif.h"
#include "../src/eth.h"
#include "../mmio.h"

/* ── Shared state ───────────────────────────────────────────────────────── */

#define NET_READY_BIT   (1u << 0)
#define NTP_DONE_BIT    (1u << 1)
#define HTTP_DONE_BIT   (1u << 2)

static EventGroupHandle_t  g_net_events;
static SemaphoreHandle_t   g_time_sem;
static volatile time_t     g_unix_time;

static struct netif g_netif;

/* ── SNTP callback ──────────────────────────────────────────────────────── */
/* Wired via SNTP_SET_SYSTEM_TIME in lwipopts.h.
   Called from the tcpip_thread — use a semaphore to notify ntp_task. */

void app_sntp_set_time(unsigned int sec) {
    g_unix_time = (time_t)sec;
    xSemaphoreGive(g_time_sem);
}

/* ── Ethernet input task ─────────────────────────────────────────────────── */
/* Polls the LiteEth RX pending bit and hands frames to tcpip_input via
   ethernetif_input (which calls netif->input = tcpip_input internally).
   Running at high priority ensures frames are processed quickly. */

static void eth_input_task(void *arg) {
    struct netif *netif = (struct netif *)arg;
    while (1) {
        while (REG_ETH_RX_EV_PENDING & 1u)
            ethernetif_input(netif);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ── netinit_task ────────────────────────────────────────────────────────── */

static void netinit_task(void *arg) {
    (void)arg;
    printf("netinit: Ethernet init...\n");
    eth_init();

    /* lwIP is already initialised by tcpip_init() in main().
       Add our netif, passing tcpip_input so all RX frames go to the
       tcpip_thread (not processed inline). */
    ip4_addr_t zero = {0};
    netif_add(&g_netif, &zero, &zero, &zero,
              NULL, ethernetif_init, tcpip_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    /* Start ETH polling task (priority just below tcpip_thread). */
    xTaskCreate(eth_input_task, "ETHRX", 256, &g_netif,
                TCPIP_THREAD_PRIO - 1, NULL);

    /* Start DHCP. */
    dhcp_start(&g_netif);
    printf("netinit: DHCP starting...\n");

    /* Block until an IP address is assigned (up to 30 s). */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    while (!dhcp_supplied_address(&g_netif)) {
        if (xTaskGetTickCount() > deadline) {
            printf("netinit: DHCP timeout\n");
            vTaskSuspend(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t ip = ntohl(netif_ip4_addr(&g_netif)->addr);
    printf("netinit: IP = %lu.%lu.%lu.%lu\n",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)( ip        & 0xFF));

    /* Signal other tasks that the network is ready. */
    xEventGroupSetBits(g_net_events, NET_READY_BIT);
    vTaskSuspend(NULL);
}

/* ── ntp_task ────────────────────────────────────────────────────────────── */

static void ntp_task(void *arg) {
    (void)arg;

    /* Wait for network ready. */
    xEventGroupWaitBits(g_net_events, NET_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    printf("ntp: starting SNTP → pool.ntp.org\n");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    /* Block until app_sntp_set_time() fires (up to 60 s). */
    if (xSemaphoreTake(g_time_sem, pdMS_TO_TICKS(60000)) == pdTRUE) {
        struct tm t;
        gmtime_r(&g_unix_time, &t);
        printf("ntp: UTC = %04d-%02d-%02d %02d:%02d:%02d\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        printf("ntp: SNTP timeout after 60 s\n");
    }

    xEventGroupSetBits(g_net_events, NTP_DONE_BIT);
    vTaskSuspend(NULL);
}

/* ── http_task ───────────────────────────────────────────────────────────── */

static void http_task(void *arg) {
    (void)arg;

    /* Wait for network ready. */
    xEventGroupWaitBits(g_net_events, NET_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    printf("http: resolving httpbin.org...\n");

    /* Resolve hostname via DNS (blocking helper from lwip/netdb.h). */
    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = lwip_getaddrinfo("httpbin.org", "80", &hints, &res);
    if (rc != 0 || res == NULL) {
        printf("http: DNS failed (%d)\n", rc);
        xEventGroupSetBits(g_net_events, HTTP_DONE_BIT);
        vTaskSuspend(NULL);
    }

    /* Open TCP socket. */
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("http: socket() failed\n");
        lwip_freeaddrinfo(res);
        xEventGroupSetBits(g_net_events, HTTP_DONE_BIT);
        vTaskSuspend(NULL);
    }

    /* Set a 10-second receive timeout. */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Connect. */
    if (lwip_connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        printf("http: connect() failed\n");
        lwip_close(s);
        lwip_freeaddrinfo(res);
        xEventGroupSetBits(g_net_events, HTTP_DONE_BIT);
        vTaskSuspend(NULL);
    }
    lwip_freeaddrinfo(res);
    printf("http: connected to httpbin.org:80\n");

    /* Send HTTP/1.0 GET (Connection: close avoids chunked responses). */
    const char *req =
        "GET /get HTTP/1.0\r\n"
        "Host: httpbin.org\r\n"
        "Connection: close\r\n"
        "\r\n";
    lwip_send(s, req, strlen(req), 0);

    /* Receive and print the first 512 bytes of the response. */
    char buf[513];
    int total = 0;
    int n;
    while (total < 512 &&
           (n = lwip_recv(s, buf + total, 512 - total, 0)) > 0)
        total += n;

    lwip_close(s);

    if (total > 0) {
        buf[total] = '\0';
        printf("http: --- response (%d bytes) ---\n%s\n", total, buf);
    } else {
        printf("http: no response received\n");
    }

    xEventGroupSetBits(g_net_events, HTTP_DONE_BIT);
    vTaskSuspend(NULL);
}

/* ── reporter_task ──────────────────────────────────────────────────────── */

static void reporter_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(g_net_events,
                        NTP_DONE_BIT | HTTP_DONE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    printf("\n=== demo complete ===\n");
    REG_LEDS    = 0xFFFF;
    REG_SEG_ALL = 0xCAFE0000u;
    vTaskSuspend(NULL);
}

/* ── vAssertCalled ──────────────────────────────────────────────────────── */

void vAssertCalled(const char *file, unsigned long line) {
    printf("ASSERT: %s:%lu\n", file, line);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0;
    printf("=== KlaussCPU FreeRTOS + lwIP demo ===\n");

    g_net_events = xEventGroupCreate();
    g_time_sem   = xSemaphoreCreateBinary();
    configASSERT(g_net_events && g_time_sem);

    /* Initialise lwIP in threaded mode.  tcpip_init() creates the tcpip_thread
       and returns immediately; stack operations happen in that thread. */
    tcpip_init(NULL, NULL);

    /* Task priorities: reporter > http/ntp > netinit > ethrx (idle=0). */
    xTaskCreate(netinit_task, "NETINIT", 512, NULL, 3, NULL);
    xTaskCreate(ntp_task,     "NTP",     512, NULL, 2, NULL);
    xTaskCreate(http_task,    "HTTP",    768, NULL, 2, NULL);

    xTaskCreate(reporter_task, "REPORT", 256, NULL, 4, NULL);

    vTaskStartScheduler();
    return 1;
}
