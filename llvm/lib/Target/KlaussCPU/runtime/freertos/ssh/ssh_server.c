/* ssh_server.c — wolfSSH server task for KlaussCPU FreeRTOS.
 *
 * Architecture:
 *   Listener task: accept() loop, spawns one connection task per client.
 *   Connection task: runs wolfSSH_accept(), then interactive shell loop.
 *   Shell: same commands as telnet/telnet.c (help/info/time/heap/tick/leds/seg).
 *
 * I/O: lwIP sockets API (blocking, per-task).
 */

#define WOLFSSL_USER_SETTINGS
#include "../wolfssl/user_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* lwIP socket API. */
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* wolfSSH. */
#include <wolfssh/ssh.h>
#include <wolfssh/log.h>

#include "ssh_server.h"
#include "sshkeys.h"
#include "../wolfssl_hw.h"
#include "../../mmio.h"

extern volatile time_t g_unix_time; /* set by NTP — declared in console_demo.c */

/* ── Authentication ──────────────────────────────────────────────────────── */

static ssh_auth_cb_t s_auth_cb;

static int default_auth_cb(const char *user, const char *password,
                           const uint8_t *pubkey, size_t pubkey_len) {
    (void)pubkey; (void)pubkey_len;
    /* Simple password auth: admin / klausscpu */
    if (strcmp(user, "admin") == 0 && strcmp(password, "klausscpu") == 0)
        return 0;
    return -1;
}

void ssh_server_set_auth_cb(ssh_auth_cb_t cb) { s_auth_cb = cb; }

/* wolfSSH authentication callback. */
static int wolfssh_auth_cb(uint8_t type, WS_UserAuthData *auth, void *ctx) {
    (void)ctx;
    ssh_auth_cb_t cb = s_auth_cb ? s_auth_cb : default_auth_cb;
    const char *user = (const char *)auth->username;

    if (type == WOLFSSH_USERAUTH_PASSWORD) {
        const char *pw = (const char *)auth->sf.password.password;
        return (cb(user, pw, NULL, 0) == 0) ? WOLFSSH_USERAUTH_SUCCESS
                                             : WOLFSSH_USERAUTH_FAILURE;
    }
    if (type == WOLFSSH_USERAUTH_PUBLICKEY) {
        const uint8_t *pk = auth->sf.publicKey.publicKey;
        word32 pk_len     = auth->sf.publicKey.publicKeySz;
        return (cb(user, NULL, pk, pk_len) == 0) ? WOLFSSH_USERAUTH_SUCCESS
                                                  : WOLFSSH_USERAUTH_FAILURE;
    }
    return WOLFSSH_USERAUTH_FAILURE;
}

/* ── lwIP I/O callbacks ──────────────────────────────────────────────────── */

static int ssh_recv(WOLFSSH *ssh, void *buf, word32 sz, void *ctx) {
    int sock = *(int *)ctx;
    int n = lwip_recv(sock, buf, sz, 0);
    if (n == 0) return WS_CBIO_ERR_CONN_CLOSE;
    if (n < 0)  return WS_CBIO_ERR_GENERAL;
    return n;
}

static int ssh_send(WOLFSSH *ssh, void *buf, word32 sz, void *ctx) {
    int sock = *(int *)ctx;
    int n = lwip_send(sock, buf, sz, 0);
    if (n < 0) return WS_CBIO_ERR_GENERAL;
    return n;
}

/* ── Interactive shell (mirrors telnet/telnet.c shell) ───────────────────── */

#define SSH_BANNER   "\r\nKlaussCPU SSH shell — type 'help'\r\n\r\n"
#define SSH_PROMPT   "$ "
#define SSH_LINE_MAX 128

static void ssh_send_str(WOLFSSH *ssh, const char *s) {
    wolfSSH_stream_send(ssh, (uint8_t *)s, (word32)strlen(s));
}

static void ssh_sendf(WOLFSSH *ssh, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) wolfSSH_stream_send(ssh, (uint8_t *)buf, (word32)n);
}

static void run_shell(WOLFSSH *ssh) {
    char line[SSH_LINE_MAX];
    int  line_len = 0;

    ssh_send_str(ssh, SSH_BANNER);
    ssh_send_str(ssh, SSH_PROMPT);

    uint8_t ch;
    while (1) {
        int n = wolfSSH_stream_read(ssh, &ch, 1);
        if (n <= 0) break; /* connection closed or error */

        if (ch == '\r' || ch == '\n') {
            ssh_send_str(ssh, "\r\n");
            line[line_len] = '\0';

            /* Dispatch command. */
            if (line_len == 0) {
                /* empty line */
            } else if (strcmp(line, "help") == 0) {
                ssh_send_str(ssh,
                    "Commands: help  info  time  heap  tick  leds  seg  exit\r\n");
            } else if (strcmp(line, "info") == 0) {
                ssh_sendf(ssh, "KlaussCPU FreeRTOS SSH server\r\n"
                               "Tasks: %lu  Stack watermark: %lu words\r\n",
                    (unsigned long)uxTaskGetNumberOfTasks(),
                    (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            } else if (strcmp(line, "time") == 0) {
                if (g_unix_time) {
                    struct tm *t = gmtime((time_t *)&g_unix_time);
                    ssh_sendf(ssh, "UTC: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                        t->tm_hour, t->tm_min, t->tm_sec);
                } else {
                    ssh_send_str(ssh, "Time not set (NTP pending)\r\n");
                }
            } else if (strcmp(line, "heap") == 0) {
                ssh_sendf(ssh, "Free heap: %lu bytes\r\n",
                    (unsigned long)xPortGetFreeHeapSize());
            } else if (strcmp(line, "tick") == 0) {
                ssh_sendf(ssh, "Tick count: %lu\r\n",
                    (unsigned long)xTaskGetTickCount());
            } else if (strncmp(line, "leds ", 5) == 0) {
                unsigned long v = strtoul(line + 5, NULL, 16);
                REG_LEDS = (uint32_t)v;
                ssh_sendf(ssh, "LEDs = 0x%04lx\r\n", v);
            } else if (strncmp(line, "seg ", 4) == 0) {
                unsigned long v = strtoul(line + 4, NULL, 16);
                REG_SEG_ALL = (uint32_t)v;
                ssh_sendf(ssh, "SEG = 0x%08lx\r\n", v);
            } else if (strcmp(line, "exit") == 0 ||
                       strcmp(line, "quit") == 0) {
                ssh_send_str(ssh, "Bye.\r\n");
                break;
            } else {
                ssh_sendf(ssh, "Unknown command: %s\r\n", line);
            }

            line_len = 0;
            ssh_send_str(ssh, SSH_PROMPT);
        } else if (ch == 127 || ch == 8) {
            /* Backspace. */
            if (line_len > 0) {
                line_len--;
                ssh_send_str(ssh, "\b \b");
            }
        } else if (ch >= 0x20 && line_len < SSH_LINE_MAX - 1) {
            line[line_len++] = (char)ch;
            wolfSSH_stream_send(ssh, &ch, 1); /* echo */
        }
    }
}

/* ── Connection task ─────────────────────────────────────────────────────── */

typedef struct {
    WOLFSSH *ssh;
    int      sock;
} conn_args_t;

static void ssh_conn_task(void *arg) {
    conn_args_t *ca = (conn_args_t *)arg;
    WOLFSSH *ssh = ca->ssh;
    int      sock = ca->sock;
    vPortFree(ca);

    wolfSSH_SetIOReadCtx(ssh,  &sock);
    wolfSSH_SetIOWriteCtx(ssh, &sock);

    int rc = wolfSSH_accept(ssh);
    if (rc != WS_SUCCESS) {
        printf("ssh: accept failed (%d)\n", rc);
        goto cleanup;
    }

    run_shell(ssh);

cleanup:
    wolfSSH_free(ssh);
    lwip_close(sock);
    vTaskDelete(NULL);
}

/* ── Listener task ───────────────────────────────────────────────────────── */

static WOLFSSH_CTX *s_ctx;

static void ssh_listener_task(void *arg) {
    (void)arg;

    int listen_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        printf("ssh: socket() failed\n");
        vTaskDelete(NULL);
    }

    int yes = 1;
    lwip_setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SSH_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (lwip_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("ssh: bind() failed\n");
        lwip_close(listen_sock);
        vTaskDelete(NULL);
    }
    lwip_listen(listen_sock, SSH_MAX_CONNS);
    printf("ssh: listening on port %d\n", SSH_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = lwip_accept(listen_sock,
                                 (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) continue;

        printf("ssh: new connection from %s\n",
               inet_ntoa(client_addr.sin_addr));

        WOLFSSH *ssh = wolfSSH_new(s_ctx);
        if (!ssh) {
            printf("ssh: wolfSSH_new() failed\n");
            lwip_close(client);
            continue;
        }

        /* Set hardware crypto device for this session. */
        /* Hardware AES is registered globally via wc_CryptoCb_RegisterDevice;
         * no per-session devId setter is needed in wolfSSH 1.4.x. */
        conn_args_t *ca = pvPortMalloc(sizeof(*ca));
        if (!ca) {
            wolfSSH_free(ssh);
            lwip_close(client);
            continue;
        }
        ca->ssh  = ssh;
        ca->sock = client;

        if (xTaskCreate(ssh_conn_task, "SSHC", SSH_TASK_STACK, ca,
                        SSH_TASK_PRIO, NULL) != pdPASS) {
            wolfSSH_free(ssh);
            lwip_close(client);
            vPortFree(ca);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int ssh_server_start(void) {
    /* Initialize wolfSSH global state. */
    if (wolfSSH_Init() != WS_SUCCESS) {
        printf("ssh: wolfSSH_Init failed\n");
        return -1;
    }

    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) {
        printf("ssh: wolfSSH_CTX_new failed\n");
        return -1;
    }

    /* I/O callbacks (wolfSSH 1.4.x API). */
    wolfSSH_SetIORecv(s_ctx, ssh_recv);
    wolfSSH_SetIOSend(s_ctx, ssh_send);

    /* Authentication callback. */
    wolfSSH_SetUserAuth(s_ctx, wolfssh_auth_cb);

    /* Load Ed25519 host key. */
    const uint8_t *priv = sshkeys_get_private();
    size_t         priv_len = sshkeys_get_private_len();
    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, priv, (word32)priv_len,
                                         WOLFSSH_FORMAT_RAW) != WS_SUCCESS) {
        printf("ssh: UsePrivateKey failed\n");
        return -1;
    }

    /* Spawn listener. */
    if (xTaskCreate(ssh_listener_task, "SSHL", SSH_TASK_STACK, NULL,
                    SSH_TASK_PRIO, NULL) != pdPASS) {
        printf("ssh: xTaskCreate failed\n");
        return -1;
    }
    return 0;
}
