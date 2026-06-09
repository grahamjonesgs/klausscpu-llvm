/* ssh_server.c — wolfSSH server for KlaussCPU Zephyr.
 *
 * Adapted from freertos/ssh/ssh_server.c.  Uses Zephyr k_thread, k_malloc,
 * and the BSD socket API (CONFIG_NET_SOCKETS) instead of FreeRTOS + lwIP.
 *
 * This file is now only the SSH transport plumbing: authentication, the
 * wolfSSH socket I/O callbacks, the listener, and the per-connection threads.
 * The interactive shell itself is a Zephyr shell backend (ssh_shell_transport.c)
 * driving the single set of SHELL_CMD_REGISTER commands (shell_cmds.c) shared
 * with the serial console — there is no longer a hand-rolled command
 * interpreter here.
 *
 * Each connection thread is the SOLE reader of its socket: it runs
 * wolfSSH_accept(), then pumps decrypted bytes into its slot's shell RX ring
 * (ssh_shell_feed) until the peer closes.  The shell core thread is the sole
 * writer (see ssh_shell_transport.c for the threading rationale).
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include <wolfssh/ssh.h>
#ifdef WOLFSSH_SFTP
#include <wolfssh/wolfsftp.h>
#endif
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "ssh_server.h"
#include "ssh_shell_transport.h"
#include "llext_loader.h"
#include "sshkeys.h"
#include "wolfssl_hw.h"

LOG_MODULE_REGISTER(ssh_server, LOG_LEVEL_INF);

/* ── Authentication ──────────────────────────────────────────────────────── */

static ssh_auth_cb_t s_auth_cb;

static int default_auth_cb(const char *user, const char *password,
			   const uint8_t *pubkey, size_t pubkey_len)
{
	(void)pubkey;
	(void)pubkey_len;
	if (strcmp(user, "admin") == 0 && strcmp(password, "klausscpu") == 0) {
		return 0;
	}
	return -1;
}

void ssh_server_set_auth_cb(ssh_auth_cb_t cb)
{
	s_auth_cb = cb;
}

static int wolfssh_auth_cb(uint8_t type, WS_UserAuthData *auth, void *ctx)
{
	(void)ctx;
	ssh_auth_cb_t cb = s_auth_cb ? s_auth_cb : default_auth_cb;

	char user[64];
	word32 usrsz = auth->usernameSz < sizeof(user) - 1
			       ? auth->usernameSz
			       : (word32)(sizeof(user) - 1);
	memcpy(user, auth->username, usrsz);
	user[usrsz] = '\0';

	if (type == WOLFSSH_USERAUTH_PASSWORD) {
		char pw[128];
		word32 pwsz = auth->sf.password.passwordSz < sizeof(pw) - 1
				      ? auth->sf.password.passwordSz
				      : (word32)(sizeof(pw) - 1);
		memcpy(pw, auth->sf.password.password, pwsz);
		pw[pwsz] = '\0';

		int ok = (cb(user, pw, NULL, 0) == 0);

		LOG_INF("auth user='%s' password: %s", user,
			ok ? "OK" : "FAIL");
		return ok ? WOLFSSH_USERAUTH_SUCCESS
			  : WOLFSSH_USERAUTH_FAILURE;
	}
	if (type == WOLFSSH_USERAUTH_PUBLICKEY) {
		const uint8_t *pk = auth->sf.publicKey.publicKey;
		word32 pk_len = auth->sf.publicKey.publicKeySz;
		int ok = (cb(user, NULL, pk, pk_len) == 0);

		LOG_INF("auth user='%s' pubkey: %s", user,
			ok ? "OK" : "FAIL");
		return ok ? WOLFSSH_USERAUTH_SUCCESS
			  : WOLFSSH_USERAUTH_FAILURE;
	}
	return WOLFSSH_USERAUTH_FAILURE;
}

/* ── Socket I/O callbacks for wolfSSH ─────────────────────────────────────── */

static int ssh_recv(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
	int sock = *(int *)ctx;
	int n = zsock_recv(sock, buf, sz, 0);

	if (n == 0) {
		return WS_CBIO_ERR_CONN_CLOSE;
	}
	if (n < 0) {
		return WS_CBIO_ERR_GENERAL;
	}
	return n;
}

static int ssh_send(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
	int sock = *(int *)ctx;
	int n = zsock_send(sock, buf, sz, 0);

	if (n < 0) {
		return WS_CBIO_ERR_GENERAL;
	}
	return n;
}

/* ── Banner ──────────────────────────────────────────────────────────────── */

#define SSH_VERSION "2.0"   /* 2.0: SSH is now a Zephyr shell backend */
#define SSH_BANNER  "\r\nKlaussCPU Zephyr SSH shell v" SSH_VERSION       \
		    " (built " __DATE__ " " __TIME__ ") — type 'help'\r\n"

/* ── Connection handler thread ────────────────────────────────────────────── */

struct conn_args {
	WOLFSSH *ssh;
	int sock;
};

/* The connection thread runs wolfSSH_accept() + the RX pump (shell) or the
 * SFTP read loop.  SFTP adds the SFTP protocol + FatFs ops + an 8 KiB R/W
 * buffer on top of the crypto, so size for that (KlaussCPU 64-bit frames). */
#define CONN_STACK_SIZE 16384

/* SFTP serves files rooted here (the SD card). */
#define SFTP_ROOT "/SD:"

static K_THREAD_STACK_ARRAY_DEFINE(conn_stacks, SSH_MAX_CONNS, CONN_STACK_SIZE);
static struct k_thread conn_threads[SSH_MAX_CONNS];
static struct conn_args conn_args_pool[SSH_MAX_CONNS];
static bool conn_busy[SSH_MAX_CONNS];
static struct k_mutex conn_mutex;

static void ssh_conn_entry(void *p1, void *p2, void *p3)
{
	struct conn_args *ca = p1;
	int slot = (int)(uintptr_t)p2;

	ARG_UNUSED(p3);

	WOLFSSH *ssh = ca->ssh;
	bool safe = true;   /* safe to free the session + reuse the slot? */

	/* The wolfSSH I/O context must point at storage that outlives this
	 * thread: the shell-core thread may still send (e.g. a `run` extension)
	 * after we return.  ca is &conn_args_pool[slot] (static), so &ca->sock
	 * is stable; a stack copy would dangle. */
	wolfSSH_SetIOReadCtx(ssh, &ca->sock);
	wolfSSH_SetIOWriteCtx(ssh, &ca->sock);

#ifdef WOLFSSH_SFTP
	/* If the peer opens the SFTP subsystem, wolfSSH_accept() returns
	 * WS_SFTP_COMPLETE instead of WS_SUCCESS; serve the SD card from /SD:. */
	(void)wolfSSH_SFTP_SetDefaultPath(ssh, SFTP_ROOT);
#endif

	int rc = wolfSSH_accept(ssh);

#ifdef WOLFSSH_SFTP
	if (rc == WS_SFTP_COMPLETE) {
		LOG_INF("SFTP session established (slot %d)", slot);

		/* Blocking SFTP server loop: process requests until the peer
		 * disconnects (read returns something other than success/want). */
		do {
			rc = wolfSSH_SFTP_read(ssh);
		} while (rc == WS_SUCCESS || rc == WS_WANT_READ ||
			 rc == WS_WANT_WRITE);

		/* The shell was never claimed, so `safe` stays true and cleanup
		 * frees the session + closes the socket. */
		goto cleanup;
	}
#endif

	if (rc != WS_SUCCESS) {
		LOG_ERR("accept failed rc=%d err=%d", rc,
			wolfSSH_get_error(ssh));
		goto cleanup;
	}

	LOG_INF("SSH session established (slot %d)", slot);

	/* Greet while this thread is still the only writer, then hand the
	 * session to its shell instance (the shell core thread takes over all
	 * output from here). */
	wolfSSH_stream_send(ssh, (uint8_t *)SSH_BANNER,
			    (word32)strlen(SSH_BANNER));
	(void)ssh_shell_claim(slot, ssh);

	/* Sole reader: drain decrypted bytes into the shell's RX ring until the
	 * peer closes or errors. */
	uint8_t buf[64];
	int n;

	while ((n = wolfSSH_stream_read(ssh, buf, sizeof(buf))) > 0) {
		ssh_shell_feed(slot, buf, (size_t)n);
	}

	safe = ssh_shell_release(slot);

cleanup:
	LOG_INF("SSH session ended (slot %d)", slot);

	if (safe) {
		wolfSSH_free(ssh);
		zsock_close(ca->sock);
		k_mutex_lock(&conn_mutex, K_FOREVER);
		conn_busy[slot] = false;
		k_mutex_unlock(&conn_mutex);
	} else {
		/* An extension ignored stdin EOF and is still running on the
		 * shell thread.  Freeing the WOLFSSH object or reusing the slot
		 * would be a use-after-free, so the session object is leaked and
		 * the slot stays busy until reboot.
		 *
		 * BUT free the *network* resources so the rest of the system
		 * keeps working: close the socket (releases its net_conn
		 * context — otherwise repeated leaks exhaust CONFIG_NET_MAX_CONN
		 * and ALL new TCP fails) and point the dead session's I/O at
		 * fd -1, so the still-running extension's sends error out
		 * harmlessly instead of hitting a reused fd.  (A well-behaved
		 * `run` program terminates when getchar() returns EOF.) */
		LOG_ERR("slot %d retired: extension ignored EOF "
			"(session object leaked; socket reclaimed)", slot);
		zsock_close(ca->sock);
		ca->sock = -1;   /* ssh_send/ssh_recv now fail cleanly */
	}
}

/* ── Listener thread ──────────────────────────────────────────────────────── */

static WOLFSSH_CTX *s_ctx;

#define LISTEN_STACK_SIZE 4096
static K_THREAD_STACK_DEFINE(listen_stack, LISTEN_STACK_SIZE);
static struct k_thread listen_thread;

static void ssh_listener_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	sshkeys_persist();

	int listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (listen_sock < 0) {
		LOG_ERR("socket() failed");
		return;
	}

	int yes = 1;

	zsock_setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes,
			 sizeof(yes));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(SSH_PORT),
		.sin_addr = {.s_addr = INADDR_ANY},
	};

	if (zsock_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) <
		    0 ||
	    zsock_listen(listen_sock, SSH_MAX_CONNS) < 0) {
		LOG_ERR("bind/listen failed");
		zsock_close(listen_sock);
		return;
	}
	LOG_INF("SSH listening on port %d", SSH_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client = zsock_accept(listen_sock,
					  (struct sockaddr *)&client_addr,
					  &addr_len);

		if (client < 0) {
			continue;
		}

		LOG_INF("SSH connection accepted");

		/* Find a free connection slot */
		int slot = -1;

		k_mutex_lock(&conn_mutex, K_FOREVER);
		for (int i = 0; i < SSH_MAX_CONNS; i++) {
			if (!conn_busy[i]) {
				conn_busy[i] = true;
				slot = i;
				break;
			}
		}
		k_mutex_unlock(&conn_mutex);

		if (slot < 0) {
			LOG_WRN("Max connections reached, rejecting");
			zsock_close(client);
			continue;
		}

		WOLFSSH *ssh = wolfSSH_new(s_ctx);

		if (!ssh) {
			LOG_ERR("wolfSSH_new() failed");
			zsock_close(client);
			k_mutex_lock(&conn_mutex, K_FOREVER);
			conn_busy[slot] = false;
			k_mutex_unlock(&conn_mutex);
			continue;
		}

		conn_args_pool[slot].ssh = ssh;
		conn_args_pool[slot].sock = client;

		k_thread_create(&conn_threads[slot], conn_stacks[slot],
				CONN_STACK_SIZE, ssh_conn_entry,
				&conn_args_pool[slot],
				(void *)(uintptr_t)slot, NULL, SSH_PRIO, 0,
				K_NO_WAIT);

		char tname[16];

		snprintf(tname, sizeof(tname), "sshc%d", slot);
		k_thread_name_set(&conn_threads[slot], tname);
	}
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int ssh_server_start(void)
{
	k_mutex_init(&conn_mutex);
	llext_loader_init();
	ssh_shell_transport_init();

	if (wolfSSH_Init() != WS_SUCCESS) {
		LOG_ERR("wolfSSH_Init failed");
		return -1;
	}

	if (wolfssl_hw_init() != 0) {
		LOG_WRN("HW crypto init failed — using software");
	}

	if (sshkeys_init() != 0) {
		LOG_ERR("Host key init failed");
		return -1;
	}

	s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
	if (!s_ctx) {
		LOG_ERR("wolfSSH_CTX_new failed");
		return -1;
	}

	wolfSSH_SetIORecv(s_ctx, ssh_recv);
	wolfSSH_SetIOSend(s_ctx, ssh_send);
	wolfSSH_SetUserAuth(s_ctx, wolfssh_auth_cb);

	const uint8_t *priv = sshkeys_get_private();
	size_t priv_len = sshkeys_get_private_len();

	if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, priv, (word32)priv_len,
					     WOLFSSH_FORMAT_ASN1) !=
	    WS_SUCCESS) {
		LOG_ERR("UsePrivateKey failed");
		wolfSSH_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	k_thread_create(&listen_thread, listen_stack, LISTEN_STACK_SIZE,
			ssh_listener_entry, NULL, NULL, NULL, SSH_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&listen_thread, "ssh_listen");

	LOG_INF("SSH server started");
	return 0;
}
