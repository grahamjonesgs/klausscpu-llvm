/*
 * telnet.c — KlaussCPU FreeRTOS telnet server + command shell.
 *
 * Listens on TCP port 23.  Accepts one client at a time.  Negotiates
 * character mode (server echo, suppress go-ahead) so standard telnet
 * clients and PuTTY work interactively without local echo.
 *
 * Telnet IAC negotiation sent on connect:
 *   IAC WILL ECHO           — server echoes keystrokes
 *   IAC WILL SUPPRESS-GA    — suppress Go Ahead (required for char mode)
 *   IAC DO   SUPPRESS-GA    — request client also suppresses GA
 *   IAC DONT LINEMODE       — disable client line buffering
 *
 * Shell commands:
 *   help          — list commands
 *   info          — FreeRTOS version, tick count, heap free
 *   time          — UTC time (if NTP has synced)
 *   leds [hex]    — read / write LED register
 *   seg  [hex]    — write 7-segment display
 *   heap          — FreeRTOS heap statistics
 *   exit / quit   — close connection
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "telnet.h"
#include "pic_loader.h"
#include "mmio.h"       /* REG_LEDS, REG_SEG_ALL, REG_CLOCK_MS */

/* ── Telnet IAC constants ───────────────────────────────────────────────── */

#define IAC     255
#define WILL    251
#define WONT    252
#define DO      253
#define DONT    254
#define SB      250
#define SE      240

#define OPT_ECHO        1
#define OPT_SUPPRESS_GA 3
#define OPT_LINEMODE    34

/* ── Session state ──────────────────────────────────────────────────────── */

#define CMD_MAX     256
#define OUT_MAX     1024

typedef struct {
    int  sock;
    char cmd[CMD_MAX];
    int  cmd_len;
    char out[OUT_MAX];      /* output buffer — flushed after each command */
    int  out_len;
    volatile time_t *ntp_time;
    int  quit;
} session_t;

/* ── Output helpers ─────────────────────────────────────────────────────── */

static void sess_flush(session_t *s) {
    if (s->out_len > 0) {
        lwip_send(s->sock, s->out, s->out_len, 0);
        s->out_len = 0;
    }
}

static void sess_write(session_t *s, const char *buf, int len) {
    /* Expand '\n' → '\r\n' for telnet compatibility. */
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (s->out_len >= OUT_MAX - 2) sess_flush(s);
            s->out[s->out_len++] = '\r';
        }
        if (s->out_len >= OUT_MAX - 1) sess_flush(s);
        s->out[s->out_len++] = buf[i];
    }
}

static void sess_printf(session_t *s, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    /* vsnprintf returns total bytes that *would* be written — cap to actual. */
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    if (n > 0) sess_write(s, tmp, n);
}

/* ── Telnet option negotiation ──────────────────────────────────────────── */

static void send_iac(session_t *s, uint8_t cmd, uint8_t opt) {
    uint8_t buf[3] = { IAC, cmd, opt };
    lwip_send(s->sock, buf, 3, 0);
}

static void negotiate(session_t *s) {
    send_iac(s, WILL, OPT_ECHO);           /* server echoes */
    send_iac(s, WILL, OPT_SUPPRESS_GA);    /* suppress Go Ahead */
    send_iac(s, DO,   OPT_SUPPRESS_GA);    /* ask client to also suppress GA */
    send_iac(s, DONT, OPT_LINEMODE);       /* disable client line buffering */
}

/* ── Command implementations ────────────────────────────────────────────── */

static void cmd_help(session_t *s) {
    sess_printf(s,
        "Commands:\r\n"
        "  help          this message\r\n"
        "  info          system information\r\n"
        "  time          current UTC time (requires NTP sync)\r\n"
        "  heap          FreeRTOS heap statistics\r\n"
        "  leds [hex]    read or write LED register (e.g. leds ff00)\r\n"
        "  seg  <hex>    write 7-segment display (e.g. seg cafe0000)\r\n"
        "  tick          raw tick counter\r\n"
        "  load [file]   load and run a PIC ELF from SD (default PROG.ELF)\r\n"
        "  exit          close connection\r\n");
}

static void cmd_info(session_t *s) {
    TickType_t ticks = xTaskGetTickCount();
    uint32_t   upms  = (uint32_t)(ticks * portTICK_PERIOD_MS);
    uint32_t   up_s  = upms / 1000;
    uint32_t   up_m  = up_s / 60;
    uint32_t   up_h  = up_m / 60;

    sess_printf(s, "KlaussCPU FreeRTOS %u.%u.%u\r\n",
                tskKERNEL_VERSION_MAJOR,
                tskKERNEL_VERSION_MINOR,
                tskKERNEL_VERSION_BUILD);
    sess_printf(s, "Uptime : %02lu:%02lu:%02lu\r\n",
                (unsigned long)up_h,
                (unsigned long)(up_m % 60),
                (unsigned long)(up_s % 60));
    sess_printf(s, "Tick Hz: %lu\r\n", (unsigned long)configTICK_RATE_HZ);
    sess_printf(s, "Clock  : %lu Hz\r\n", (unsigned long)configCPU_CLOCK_HZ);
    sess_printf(s, "Heap   : %lu bytes free\r\n",
                (unsigned long)xPortGetFreeHeapSize());
}

static void cmd_time(session_t *s) {
    if (!s->ntp_time || *s->ntp_time == 0) {
        sess_printf(s, "NTP not synced yet\r\n");
        return;
    }
    /* Add elapsed ms since last sync to get current time. */
    time_t now = *s->ntp_time
               + (time_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    struct tm t;
    gmtime_r(&now, &t);
    sess_printf(s, "UTC: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
}

static void cmd_heap(session_t *s) {
    sess_printf(s, "Free now  : %lu bytes\r\n",
                (unsigned long)xPortGetFreeHeapSize());
    sess_printf(s, "Free min  : %lu bytes (ever)\r\n",
                (unsigned long)xPortGetMinimumEverFreeHeapSize());
    sess_printf(s, "Total     : %lu bytes\r\n",
                (unsigned long)configTOTAL_HEAP_SIZE);
}

static void cmd_tick(session_t *s) {
    sess_printf(s, "%lu ticks  (%lu ms)\r\n",
                (unsigned long)xTaskGetTickCount(),
                (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
}

static void cmd_leds(session_t *s, const char *arg) {
    if (arg && *arg) {
        unsigned long val = strtoul(arg, NULL, 16);
        REG_LEDS = (uint32_t)val;
        sess_printf(s, "LEDs set to 0x%04lX\r\n", val & 0xFFFF);
    } else {
        sess_printf(s, "LEDs = 0x%04lX\r\n", (unsigned long)(REG_LEDS & 0xFFFF));
    }
}

static void cmd_seg(session_t *s, const char *arg) {
    if (!arg || !*arg) {
        sess_printf(s, "Usage: seg <hex32>  (e.g. seg cafe0000)\r\n");
        return;
    }
    unsigned long val = strtoul(arg, NULL, 16);
    REG_SEG_ALL = (uint32_t)val;
    sess_printf(s, "7-seg set to 0x%08lX\r\n", val);
}

static void cmd_load(session_t *s, const char *arg) {
    /* 8.3 filename: must be upper-case for FatFs with FF_USE_LFN=0. */
    const char *filename = (arg && *arg) ? arg : "PROG.ELF";
    sess_printf(s, "Loading %s from SD card...\r\n", filename);
    sess_flush(s);

    int rc = pic_run_from_sd(filename, s->sock);

    if (rc < 0)
        sess_printf(s, "\r\nLoad/run failed (code %d)\r\n", rc);
    else
        sess_printf(s, "\r\nProgram exited: %d\r\n", rc);
}

/* ── Command dispatcher ─────────────────────────────────────────────────── */

static void execute(session_t *s) {
    /* Trim leading spaces. */
    char *p = s->cmd;
    while (*p == ' ') p++;
    if (*p == '\0') return;

    /* Split verb and argument. */
    char *arg = p;
    while (*arg && *arg != ' ') arg++;
    if (*arg == ' ') { *arg++ = '\0'; while (*arg == ' ') arg++; }
    if (!*arg) arg = NULL;

    if      (!strcmp(p, "help"))             cmd_help(s);
    else if (!strcmp(p, "info"))             cmd_info(s);
    else if (!strcmp(p, "time"))             cmd_time(s);
    else if (!strcmp(p, "heap"))             cmd_heap(s);
    else if (!strcmp(p, "tick"))             cmd_tick(s);
    else if (!strcmp(p, "leds"))             cmd_leds(s, arg);
    else if (!strcmp(p, "seg"))              cmd_seg(s, arg);
    else if (!strcmp(p, "load"))             cmd_load(s, arg);
    else if (!strcmp(p, "exit") ||
             !strcmp(p, "quit") ||
             !strcmp(p, "logout"))           s->quit = 1;
    else
        sess_printf(s, "Unknown command: %s  (type 'help')\r\n", p);
}

/* ── Input character handler ────────────────────────────────────────────── */

/* Returns 0 to continue, non-zero to drop the character (IAC byte consumed). */
static int handle_iac(session_t *s, const uint8_t *buf, int len, int *consumed) {
    /* Minimum: IAC cmd opt = 3 bytes. */
    if (len < 3) { *consumed = 1; return 1; }   /* incomplete — skip IAC */
    uint8_t cmd = buf[1];
    uint8_t opt = buf[2];
    *consumed = 3;

    /* Respond to DO/WILL/DONT/WONT to keep the client happy. */
    if (cmd == DO) {
        if (opt == OPT_ECHO || opt == OPT_SUPPRESS_GA)
            send_iac(s, WILL, opt);
        else
            send_iac(s, WONT, opt);
    } else if (cmd == WILL) {
        if (opt == OPT_SUPPRESS_GA)
            send_iac(s, DO, opt);
        else
            send_iac(s, DONT, opt);
    }
    /* WONT / DONT: nothing to do. */
    return 1;
}

static void handle_char(session_t *s, uint8_t c) {
    if (c == '\r' || c == '\n') {
        /* Execute command. */
        lwip_send(s->sock, "\r\n", 2, 0);
        s->cmd[s->cmd_len] = '\0';
        execute(s);
        sess_flush(s);
        s->cmd_len = 0;
        if (!s->quit)
            lwip_send(s->sock, "> ", 2, 0);
    } else if (c == 127 || c == 8) {
        /* Backspace / DEL: erase one character. */
        if (s->cmd_len > 0) {
            s->cmd_len--;
            lwip_send(s->sock, "\b \b", 3, 0);
        }
    } else if (c == 3) {
        /* Ctrl-C: clear line. */
        while (s->cmd_len > 0) {
            lwip_send(s->sock, "\b \b", 3, 0);
            s->cmd_len--;
        }
    } else if (c == 4) {
        /* Ctrl-D: disconnect. */
        s->quit = 1;
    } else if (c >= 32 && c < 127 && s->cmd_len < CMD_MAX - 1) {
        /* Printable — echo and buffer. */
        s->cmd[s->cmd_len++] = (char)c;
        lwip_send(s->sock, &c, 1, 0);
    }
    /* Ignore other control chars. */
}

/* ── Session loop ───────────────────────────────────────────────────────── */

static void run_session(session_t *s) {
    uint8_t buf[64];

    negotiate(s);
    vTaskDelay(pdMS_TO_TICKS(50));   /* let negotiation settle */

    sess_printf(s, "\r\nKlaussCPU FreeRTOS shell  (type 'help')\r\n");
    sess_flush(s);
    lwip_send(s->sock, "> ", 2, 0);

    /* Set receive timeout so we don't block forever if client vanishes. */
    struct timeval tv = { .tv_sec = 300, .tv_usec = 0 };
    lwip_setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!s->quit) {
        int n = lwip_recv(s->sock, buf, sizeof(buf), 0);
        if (n <= 0) break;  /* disconnected or timeout */

        int i = 0;
        while (i < n && !s->quit) {
            if (buf[i] == IAC) {
                int consumed = 1;
                handle_iac(s, buf + i, n - i, &consumed);
                i += consumed;
            } else {
                handle_char(s, buf[i++]);
            }
        }
    }
}

/* ── Telnet server task ─────────────────────────────────────────────────── */

static volatile time_t *g_ntp_time;

static void telnet_task(void *arg) {
    (void)arg;

    int srv = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { vTaskSuspend(NULL); return; }

    int yes = 1;
    lwip_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(TELNET_PORT),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };
    lwip_bind(srv, (struct sockaddr *)&sa, sizeof(sa));
    lwip_listen(srv, 1);

    printf("telnet: listening on port %d\n", TELNET_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = lwip_accept(srv, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) continue;

        printf("telnet: client connected from %s\n",
               inet_ntoa(client_addr.sin_addr));

        session_t s = {
            .sock    = client,
            .cmd_len = 0,
            .out_len = 0,
            .ntp_time = g_ntp_time,
            .quit    = 0,
        };
        run_session(&s);

        lwip_close(client);
        printf("telnet: client disconnected\n");
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void telnet_start(volatile time_t *ntp_time_ptr) {
    g_ntp_time = ntp_time_ptr;
    xTaskCreate(telnet_task, "TELNET", TELNET_TASK_STACK, NULL,
                TELNET_TASK_PRIO, NULL);
}
