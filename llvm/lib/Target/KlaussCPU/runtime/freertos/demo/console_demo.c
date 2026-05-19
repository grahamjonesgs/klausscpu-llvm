/*
 * console_demo.c — KlaussCPU FreeRTOS remote console via telnet and SSH.
 *
 * Starts lwIP with DHCP, an NTP sync task, a telnet server on port 23,
 * and an SSH server on port 22 (wolfSSH + hardware AES/TRNG acceleration).
 * Once DHCP completes the IP address is printed on the UART; connect with:
 *
 *   telnet <ip>          — unencrypted shell
 *   ssh admin@<ip>       — encrypted shell (password: klausscpu)
 *
 * Shell commands: help  info  time  heap  tick  leds  seg  load  exit
 * NTP keeps a running UTC clock that the 'time' command reads.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/apps/sntp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "../lwip_port/ethernetif.h"
#include "../src/eth.h"
#include "../telnet/telnet.h"
#include "../telnet/pic_loader.h"
#include "../ssh/ssh_server.h"
#include "../mmio.h"

/* ── Shared state ───────────────────────────────────────────────────────── */

#define NET_READY_BIT   (1u << 0)

static EventGroupHandle_t  g_net_events;
volatile time_t            g_unix_time;   /* updated by NTP callback; extern in ssh_server.c */
static struct netif         g_netif;

/* ── SNTP callback ──────────────────────────────────────────────────────── */

void app_sntp_set_time(unsigned int sec) {
    g_unix_time = (time_t)sec;
}

/* ── Ethernet input task ────────────────────────────────────────────────── */

static void eth_input_task(void *arg) {
    struct netif *netif = (struct netif *)arg;
    while (1) {
        while (REG_ETH_RX_EV_PENDING & 1u)
            ethernetif_input(netif);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ── Network init task ──────────────────────────────────────────────────── */

static void netinit_task(void *arg) {
    (void)arg;

    printf("console: Ethernet init...\n");
    eth_init();

    ip4_addr_t zero = {0};
    netif_add(&g_netif, &zero, &zero, &zero,
              NULL, ethernetif_init, tcpip_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    xTaskCreate(eth_input_task, "ETHRX", 256, &g_netif,
                TCPIP_THREAD_PRIO - 1, NULL);

    dhcp_start(&g_netif);
    printf("console: DHCP starting...\n");

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    while (!dhcp_supplied_address(&g_netif)) {
        if (xTaskGetTickCount() > deadline) {
            printf("console: DHCP timeout\n");
            vTaskSuspend(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t ip = ntohl(netif_ip4_addr(&g_netif)->addr);
    printf("console: IP = %lu.%lu.%lu.%lu\n",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)( ip        & 0xFF));
    printf("console: connect with:  telnet %lu.%lu.%lu.%lu\n",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)( ip        & 0xFF));

    xEventGroupSetBits(g_net_events, NET_READY_BIT);
    vTaskSuspend(NULL);
}

/* ── NTP task ───────────────────────────────────────────────────────────── */

static void ntp_task(void *arg) {
    (void)arg;
    xEventGroupWaitBits(g_net_events, NET_READY_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    printf("console: SNTP started\n");

    /* SNTP updates g_unix_time via callback — just keep the task alive
       so the NTP re-sync timer continues firing. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}

/* ── vAssertCalled ──────────────────────────────────────────────────────── */

void vAssertCalled(const char *file, unsigned long line) {
    printf("ASSERT: %s:%lu\n", file, line);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0;
    printf("=== KlaussCPU remote console ===\n");

    g_net_events = xEventGroupCreate();
    configASSERT(g_net_events);

    tcpip_init(NULL, NULL);

    pic_loader_init();   /* create PIC loader mutex before scheduler starts */

    xTaskCreate(netinit_task, "NETINIT", 512, NULL, 3, NULL);
    xTaskCreate(ntp_task,     "NTP",     512, NULL, 2, NULL);

    /* Start the telnet server — it will listen once the network is up. */
    telnet_start(&g_unix_time);

    /* Start the SSH server — also waits for network (listener retries in task). */
    if (ssh_server_start() != 0)
        printf("console: WARNING: SSH server failed to start\n");

    vTaskStartScheduler();
    return 1;
}
