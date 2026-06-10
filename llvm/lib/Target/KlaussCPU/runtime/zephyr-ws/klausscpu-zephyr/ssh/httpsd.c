/*
 * httpsd.c — HTTPS file server for KlaussCPU Zephyr (HTTP over TLS, :443).
 *
 * wolfSSL is already linked for wolfSSH (SSH), and CONFIG_WOLFSSL builds the
 * full TLS layer (ssl.c/tls.c/tls13.c), so HTTPS is mostly wiring: present a
 * self-signed ECDSA P-256 cert (tlscert.c), wrap each accepted TCP socket in a
 * WOLFSSL session, and run the shared transport-agnostic request dispatch
 * (httpd_serve) over wolfSSL_read/wolfSSL_write instead of raw sockets.
 *
 * Supports TLS 1.2 + 1.3 (wolfSSLv23_server_method, min TLS 1.2).  Cipher
 * suites are ECDHE-ECDSA-AES-GCM (1.2) / TLS_AES_*_GCM (1.3) — all ECC, so no
 * RSA/DH is required (matching the SSH-tuned wolfSSL config).
 *
 * user_settings.h sets WOLFSSL_NO_SOCK + WOLFSSL_USER_IO, so the encrypted
 * bytes move over the socket via custom I/O callbacks (net_recv/net_send),
 * exactly as ssh_server.c does for wolfSSH.
 *
 * Single connection at a time (one worker thread, blocking accept loop) — same
 * model as the plain :80 server; the board is a single-user dev target.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <errno.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>

#include "httpd.h"
#include "httpsd.h"
#include "tlscert.h"

LOG_MODULE_REGISTER(httpsd, LOG_LEVEL_INF);

#define HTTPS_PORT        443
#define XFER_SZ           2048

/* TLS handshakes on KlaussCPU's 64-bit stack frames are heavy — the wolfSSH
 * connection threads use 16 KiB for the same reason (see ssh_server.c). */
#define HTTPSD_STACK_SIZE 16384
#define HTTPSD_PRIO       7

static K_THREAD_STACK_DEFINE(httpsd_stack, HTTPSD_STACK_SIZE);
static struct k_thread httpsd_thread;

/* Transfer buffer — one connection served at a time, separate from httpd's. */
static char xfer[XFER_SZ];

static WOLFSSL_CTX *s_ctx;

/* ── wolfSSL <-> socket I/O callbacks (NO_SOCK / USER_IO) ─────────────────── */

static int net_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	ARG_UNUSED(ssl);
	int sock = *(int *)ctx;
	int n = zsock_recv(sock, buf, sz, 0);

	if (n == 0) {
		return WOLFSSL_CBIO_ERR_CONN_CLOSE;
	}
	if (n < 0) {
		return WOLFSSL_CBIO_ERR_GENERAL;
	}
	return n;
}

static int net_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	ARG_UNUSED(ssl);
	int sock = *(int *)ctx;
	int n = zsock_send(sock, buf, sz, 0);

	if (n < 0) {
		return WOLFSSL_CBIO_ERR_GENERAL;
	}
	return n;
}

/* ── httpd_conn transport over a TLS session ─────────────────────────────── */

static int tls_recv(void *ctx, void *buf, size_t len)
{
	return wolfSSL_read((WOLFSSL *)ctx, buf, (int)len);
}

static int tls_send(void *ctx, const void *buf, size_t len)
{
	return wolfSSL_write((WOLFSSL *)ctx, buf, (int)len);
}

/* ── server thread ──────────────────────────────────────────────────────── */

static void httpsd_main(void *a, void *b, void *cc)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(cc);

	int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (srv < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return;
	}

	int yes = 1;

	(void)zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(HTTPS_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind :%d failed: %d", HTTPS_PORT, -errno);
		(void)zsock_close(srv);
		return;
	}
	if (zsock_listen(srv, 2) < 0) {
		LOG_ERR("listen failed: %d", -errno);
		(void)zsock_close(srv);
		return;
	}

	LOG_INF("HTTPS file server on :%d (root /SD:)", HTTPS_PORT);

	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = zsock_accept(srv, (struct sockaddr *)&peer, &plen);

		if (cfd < 0) {
			continue;
		}

		/* Bound blocking recv (handshake + request reads) so a stalled
		 * client can't hang this single-threaded worker indefinitely —
		 * e.g. a PUT that sends Expect: 100-continue and then no body. */
		struct zsock_timeval tv = { .tv_sec = 15, .tv_usec = 0 };

		(void)zsock_setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv,
				       sizeof(tv));

		WOLFSSL *ssl = wolfSSL_new(s_ctx);

		if (ssl == NULL) {
			LOG_ERR("wolfSSL_new failed");
			(void)zsock_close(cfd);
			continue;
		}

		/* cfd is stable for this iteration; the handshake + serve run
		 * synchronously here (unlike SSH, which hands off to a thread). */
		wolfSSL_SetIOReadCtx(ssl, &cfd);
		wolfSSL_SetIOWriteCtx(ssl, &cfd);

		int rc = wolfSSL_accept(ssl);

		if (rc != WOLFSSL_SUCCESS) {
			LOG_WRN("TLS handshake failed: err=%d",
				wolfSSL_get_error(ssl, rc));
		} else {
			struct httpd_conn conn = {
				.io_ctx = ssl,
				.io_recv = tls_recv,
				.io_send = tls_send,
				.xfer = xfer,
				.xfer_sz = sizeof(xfer),
			};

			/* Serve multiple requests over this one TLS session
			 * (keep-alive) so a page's assets share a single
			 * handshake.  Loop until the client closes, errors, or
			 * the recv timeout fires. */
			while (httpd_serve(&conn)) {
			}
			(void)wolfSSL_shutdown(ssl);
		}

		wolfSSL_free(ssl);
		(void)zsock_close(cfd);
	}
}

/* ── public API ─────────────────────────────────────────────────────────── */

int httpsd_start(void)
{
	/* wolfSSL_Init is idempotent; wolfSSH calls its own init separately. */
	if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
		LOG_ERR("wolfSSL_Init failed");
		return -1;
	}

	/* Board IPv4 (network byte order) for the cert's iPAddress SAN, so
	 * browsers accept https://<ip> without a name-mismatch error.  DHCP is
	 * already bound by the time httpsd_start() runs (see main.c). */
	const uint8_t *ip4 = NULL;
	uint8_t ipbuf[4];
	struct net_if *iface = net_if_get_default();

	if (iface != NULL) {
		struct net_if_config *cfg = net_if_get_config(iface);

		if (cfg != NULL && cfg->ip.ipv4 != NULL) {
			memcpy(ipbuf,
			       &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr.s_addr,
			       4);
			ip4 = ipbuf;
		}
	}

	if (tlscert_init(ip4) != 0) {
		LOG_ERR("TLS cert init failed");
		return -1;
	}

	s_ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
	if (s_ctx == NULL) {
		LOG_ERR("wolfSSL_CTX_new failed");
		return -1;
	}

	/* TLS 1.2 + 1.3 only (NO_OLD_TLS already excludes 1.0/1.1). */
	(void)wolfSSL_CTX_SetMinVersion(s_ctx, WOLFSSL_TLSV1_2);

	wolfSSL_SetIORecv(s_ctx, net_recv);
	wolfSSL_SetIOSend(s_ctx, net_send);

	if (wolfSSL_CTX_use_certificate_buffer(s_ctx, tlscert_get_cert(),
					       (long)tlscert_get_cert_len(),
					       WOLFSSL_FILETYPE_ASN1) !=
	    WOLFSSL_SUCCESS) {
		LOG_ERR("use_certificate_buffer failed");
		wolfSSL_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	if (wolfSSL_CTX_use_PrivateKey_buffer(s_ctx, tlscert_get_key(),
					      (long)tlscert_get_key_len(),
					      WOLFSSL_FILETYPE_ASN1) !=
	    WOLFSSL_SUCCESS) {
		LOG_ERR("use_PrivateKey_buffer failed");
		wolfSSL_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	(void)k_thread_create(&httpsd_thread, httpsd_stack,
			      K_THREAD_STACK_SIZEOF(httpsd_stack),
			      httpsd_main, NULL, NULL, NULL,
			      HTTPSD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&httpsd_thread, "httpsd");

	LOG_INF("HTTPS server started");
	return 0;
}
