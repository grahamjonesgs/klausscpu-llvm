/*
 * httpd.h — HTTP file server (serves /SD:).  The request dispatch is
 * transport-agnostic so it can run over a plain TCP socket (httpd, :80) or a
 * TLS session (httpsd, :443) — see struct httpd_conn.
 */

#ifndef KLAUSSCPU_HTTPD_H_
#define KLAUSSCPU_HTTPD_H_

#include <stddef.h>
#include <stdbool.h>

/* One established connection, abstracted over its transport.  The caller binds
 * recv/send to its transport (raw socket or WOLFSSL session) and supplies a
 * scratch transfer buffer (caller-owned; only one request is served per call,
 * so a per-server static buffer is fine). */
struct httpd_conn {
	void *io_ctx;
	/* recv/send follow BSD semantics: >0 bytes, 0 = orderly close, <0 error */
	int (*io_recv)(void *ctx, void *buf, size_t len);
	int (*io_send)(void *ctx, const void *buf, size_t len);
	char  *xfer;
	size_t xfer_sz;
	bool   head;        /* request was HEAD: send headers, suppress the body */
	bool   keepalive;   /* serve another request on this connection?          */
};

/* Serve one HTTP request/response on an established connection.  Returns true
 * if the connection should stay open for another request (HTTP keep-alive),
 * false if the caller should close it. */
bool httpd_serve(struct httpd_conn *c);

/* Send a complete response (status line + Content-Type/Length + Connection +
 * body).  Honours c->keepalive and c->head.  Used by the web API (webapi.c). */
void httpd_send(struct httpd_conn *c, const char *status,
		const char *content_type, const char *body);

/* Start the plain HTTP file-server thread (:80).  Call once after DHCP. */
void httpd_start(void);

#endif /* KLAUSSCPU_HTTPD_H_ */
