/*
 * httpd.c — minimal HTTP/1.0 file server for the KlaussCPU board.
 *
 * Zephyr's native CONFIG_HTTP_SERVER (3.7 LTS) can't stream arbitrary-size
 * files (its dynamic-GET response is driven by the URL length, capped at ~one
 * buffer), so this is a small BSD-socket server instead.  It serves the SD
 * card (/SD:):
 *
 *   GET  /            -> HTML directory listing of /SD:
 *   GET  /<path>      -> download a file (streamed) or a listing if it's a dir
 *   PUT  /<path>      -> upload (overwrite) a file
 *
 * The request dispatch (httpd_serve) is transport-agnostic: it talks through a
 * struct httpd_conn whose recv/send callbacks are bound either to a raw socket
 * (this file, plain :80) or to a TLS session (httpsd.c, :443).  Both servers
 * share all the GET/PUT/listing code below.
 *
 * Single connection at a time per server (one worker thread, blocking accept
 * loop) — the board is a single-user dev target and this keeps it simple and
 * robust.  Responses are HTTP/1.0 + "Connection: close" (body delimited by
 * close for listings; Content-Length for files).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#include "httpd.h"

LOG_MODULE_REGISTER(httpd, LOG_LEVEL_INF);

#define HTTPD_PORT       80
#define DOC_ROOT         "/SD:"
#define DOC_ROOT_LEN     (sizeof(DOC_ROOT) - 1)
#define REQ_MAX          1024              /* request line + headers we read */
#define XFER_SZ          2048              /* file/dir transfer chunk */
#define URL_MAX          256

#define HTTPD_STACK_SIZE 8192
#define HTTPD_PRIO       7

static K_THREAD_STACK_DEFINE(httpd_stack, HTTPD_STACK_SIZE);
static struct k_thread httpd_thread;

/* Transfer buffer for the plain (:80) server — only one connection is served
 * at a time.  The TLS server (httpsd.c) owns its own separate buffer. */
static char xfer[XFER_SZ];

/* ── small helpers ──────────────────────────────────────────────────────── */

static int send_all(struct httpd_conn *c, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		int n = c->io_send(c->io_ctx, p, len);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int send_str(struct httpd_conn *c, const char *str)
{
	return send_all(c, str, strlen(str));
}

static void send_status(struct httpd_conn *c, const char *status,
			const char *body)
{
	char hdr[200];
	unsigned int blen = (body != NULL) ? (unsigned int)strlen(body) : 0;

	(void)snprintk(hdr, sizeof(hdr),
		       "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n"
		       "Content-Length: %u\r\nConnection: %s\r\n\r\n",
		       status, blen, c->keepalive ? "keep-alive" : "close");
	(void)send_str(c, hdr);
	if (body != NULL && !c->head) {
		(void)send_str(c, body);
	}
}

/* In-place URL decode (%XX and '+' → space). */
static void url_decode(char *s)
{
	char *o = s;

	while (*s != '\0') {
		if (*s == '%' && isxdigit((unsigned char)s[1]) &&
		    isxdigit((unsigned char)s[2])) {
			char h[3] = { s[1], s[2], '\0' };

			*o++ = (char)strtol(h, NULL, 16);
			s += 3;
		} else if (*s == '+') {
			*o++ = ' ';
			s++;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

/* ── directory listing ──────────────────────────────────────────────────── */

static void send_listing(struct httpd_conn *c, const char *fs_path,
			 const char *url)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int rc;

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, fs_path);
	if (rc != 0) {
		send_status(c, "404 Not Found", "no such directory\n");
		return;
	}

	/* The listing length isn't known up front, so it is close-delimited —
	 * which also ends keep-alive for this connection. */
	c->keepalive = false;
	(void)send_str(c,
		"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
		"Connection: close\r\n\r\n");
	if (c->head) {                  /* headers only for HEAD */
		(void)fs_closedir(&dir);
		return;
	}
	(void)snprintk(c->xfer, c->xfer_sz,
		       "<!doctype html><html><head><meta charset=utf-8>"
		       "<title>%s</title></head><body><h2>Index of %s</h2><ul>",
		       url, url);
	(void)send_str(c, c->xfer);

	/* parent link (unless at root) */
	if (strcmp(url, "/") != 0) {
		(void)send_str(c, "<li><a href=\"../\">../</a></li>");
	}

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		bool is_dir = (ent.type == FS_DIR_ENTRY_DIR);

		/* Build href: url + name (+ '/' for dirs).  url ends with '/'. */
		(void)snprintk(c->xfer, c->xfer_sz,
			       "<li><a href=\"%s%s%s\">%s%s</a>%s</li>",
			       url, ent.name, is_dir ? "/" : "",
			       ent.name, is_dir ? "/" : "",
			       is_dir ? "" : "");
		(void)send_str(c, c->xfer);
		if (!is_dir) {
			(void)snprintk(c->xfer, c->xfer_sz,
				       "<li style=\"list-style:none;margin-left:1em;"
				       "color:#888\">%u bytes</li>",
				       (unsigned int)ent.size);
			(void)send_str(c, c->xfer);
		}
	}

	(void)fs_closedir(&dir);
	(void)send_str(c, "</ul></body></html>\r\n");
}

/* ── file download ──────────────────────────────────────────────────────── */

/* Map a filename extension to a Content-Type so browsers render (rather than
 * download) HTML/CSS/JS/images.  Unknown types fall back to octet-stream. */
static const char *mime_type(const char *path)
{
	static const struct {
		const char *ext;
		const char *type;
	} map[] = {
		{ "html", "text/html" },        { "htm",  "text/html" },
		{ "css",  "text/css" },          { "js",   "text/javascript" },
		{ "json", "application/json" },  { "txt",  "text/plain" },
		{ "svg",  "image/svg+xml" },     { "png",  "image/png" },
		{ "jpg",  "image/jpeg" },        { "jpeg", "image/jpeg" },
		{ "gif",  "image/gif" },         { "ico",  "image/x-icon" },
		{ "xml",  "text/xml" },          { "pdf",  "application/pdf" },
		{ "wasm", "application/wasm" },
	};
	const char *dot = strrchr(path, '.');

	if (dot != NULL) {
		const char *ext = dot + 1;

		for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
			const char *a = ext;
			const char *b = map[i].ext;

			while (*b != '\0' &&
			       tolower((unsigned char)*a) == *b) {
				a++;
				b++;
			}
			if (*b == '\0' && *a == '\0') {
				return map[i].type;
			}
		}
	}
	return "application/octet-stream";
}

static void send_file(struct httpd_conn *c, const char *fs_path, size_t size)
{
	struct fs_file_t f;
	char hdr[160];
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, fs_path, FS_O_READ);
	if (rc != 0) {
		send_status(c, "404 Not Found", "cannot open file\n");
		return;
	}

	(void)snprintk(hdr, sizeof(hdr),
		       "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
		       "Content-Length: %u\r\nConnection: %s\r\n\r\n",
		       mime_type(fs_path), (unsigned int)size,
		       c->keepalive ? "keep-alive" : "close");
	if (send_str(c, hdr) == 0 && !c->head) {
		for (;;) {
			ssize_t n = fs_read(&f, c->xfer, c->xfer_sz);

			if (n <= 0) {
				break;          /* EOF or error */
			}
			if (send_all(c, c->xfer, (size_t)n) != 0) {
				break;          /* client gone */
			}
		}
	}

	(void)fs_close(&f);
}

/* Serve a directory: an index.html inside it if present, else the listing.
 * fs_dir has no trailing slash (except the bare mount root); url ends with '/'. */
static void serve_dir(struct httpd_conn *c, const char *fs_dir, const char *url)
{
	char idx[DOC_ROOT_LEN + URL_MAX + 12];
	struct fs_dirent ent;

	(void)snprintk(idx, sizeof(idx), "%s/index.html", fs_dir);
	if (fs_stat(idx, &ent) == 0 && ent.type == FS_DIR_ENTRY_FILE) {
		send_file(c, idx, ent.size);
		return;
	}
	send_listing(c, fs_dir, url);
}

/* ── file upload (PUT) ──────────────────────────────────────────────────── */

static void recv_put(struct httpd_conn *c, const char *fs_path,
		     const char *body, int body_in_req, long content_len)
{
	struct fs_file_t f;
	long remaining = content_len;
	int rc;

	fs_file_t_init(&f);
	rc = fs_open(&f, fs_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		send_status(c, "500 Internal Server Error", "cannot create file\n");
		return;
	}

	/* bytes of the body already sitting in the request buffer */
	if (body_in_req > 0) {
		(void)fs_write(&f, body, body_in_req);
		remaining -= body_in_req;
	}

	while (remaining > 0) {
		int n = c->io_recv(c->io_ctx, c->xfer,
				   (size_t)MIN((long)c->xfer_sz, remaining));

		if (n <= 0) {
			break;
		}
		(void)fs_write(&f, c->xfer, n);
		remaining -= n;
	}

	(void)fs_close(&f);

	if (remaining == 0) {
		send_status(c, "201 Created", "stored\n");
	} else {
		/* Body framing is broken — don't try to read another request. */
		c->keepalive = false;
		send_status(c, "400 Bad Request", "short upload\n");
	}
}

/* Case-insensitive search for a header; returns a pointer just past its
 * colon, or NULL.  (minimal libc has no strcasestr.) */
static const char *find_header(const char *req, const char *key_lc)
{
	for (const char *s = req; *s != '\0'; s++) {
		size_t i = 0;

		while (key_lc[i] != '\0' &&
		       tolower((unsigned char)s[i]) == key_lc[i]) {
			i++;
		}
		if (key_lc[i] == '\0') {
			return s + i;
		}
	}
	return NULL;
}

/* True if the request's Connection header contains token (case-insensitive),
 * scanning only that header's value (up to end of line). */
static bool conn_has(const char *req, const char *token)
{
	const char *v = find_header(req, "connection:");

	if (v == NULL) {
		return false;
	}
	for (; *v != '\0' && *v != '\r' && *v != '\n'; v++) {
		size_t i = 0;

		while (token[i] != '\0' &&
		       tolower((unsigned char)v[i]) == tolower((unsigned char)token[i])) {
			i++;
		}
		if (token[i] == '\0') {
			return true;
		}
	}
	return false;
}

/* ── request dispatch ───────────────────────────────────────────────────── */

bool httpd_serve(struct httpd_conn *c)
{
	char req[REQ_MAX];
	int n;
	char method[8];
	char url[URL_MAX];
	char fs_path[DOC_ROOT_LEN + URL_MAX + 2];
	const char *sp1, *sp2;
	size_t mlen, ulen;

	c->head = false;
	c->keepalive = false;   /* default to close until the version is parsed */

	n = c->io_recv(c->io_ctx, req, sizeof(req) - 1);
	if (n <= 0) {
		return false;       /* client closed, error, or idle timeout */
	}
	req[n] = '\0';

	/* Parse "METHOD SP URL SP HTTP/x.y" from the first line.  A malformed
	 * request line leaves stream framing uncertain, so always close. */
	sp1 = strchr(req, ' ');
	if (sp1 == NULL) {
		send_status(c, "400 Bad Request", NULL);
		return false;
	}
	sp2 = strchr(sp1 + 1, ' ');
	if (sp2 == NULL) {
		send_status(c, "400 Bad Request", NULL);
		return false;
	}
	mlen = (size_t)(sp1 - req);
	ulen = (size_t)(sp2 - (sp1 + 1));
	if (mlen >= sizeof(method) || ulen >= sizeof(url)) {
		send_status(c, "414 URI Too Long", NULL);
		return false;
	}
	memcpy(method, req, mlen);
	method[mlen] = '\0';
	memcpy(url, sp1 + 1, ulen);
	url[ulen] = '\0';

	/* Keep-alive: default on for HTTP/1.1 unless "Connection: close";
	 * default off for HTTP/1.0 unless "Connection: keep-alive". */
	if (strncmp(sp2 + 1, "HTTP/1.1", 8) == 0) {
		c->keepalive = !conn_has(req, "close");
	} else {
		c->keepalive = conn_has(req, "keep-alive");
	}

	/* strip query string, decode, reject path traversal */
	{
		char *q = strchr(url, '?');

		if (q != NULL) {
			*q = '\0';
		}
	}
	url_decode(url);
	if (url[0] != '/' || strstr(url, "..") != NULL) {
		send_status(c, "403 Forbidden", "bad path\n");
		return c->keepalive;
	}

	(void)snprintk(fs_path, sizeof(fs_path), "%s%s", DOC_ROOT, url);
	/* drop a trailing '/' (except the bare mount root) so fs_* is happy */
	{
		size_t fl = strlen(fs_path);

		if (fl > DOC_ROOT_LEN && fs_path[fl - 1] == '/') {
			fs_path[fl - 1] = '\0';
		}
	}

	/* HEAD is served exactly like GET but with the body suppressed
	 * (c->head, honoured in send_file/send_listing). */
	c->head = (strcmp(method, "HEAD") == 0);

	if (c->head || strcmp(method, "GET") == 0) {
		struct fs_dirent ent;

		if (strcmp(url, "/") == 0) {
			serve_dir(c, DOC_ROOT, "/");
			return c->keepalive;
		}
		if (fs_stat(fs_path, &ent) != 0) {
			send_status(c, "404 Not Found", "not found\n");
			return c->keepalive;
		}
		if (ent.type == FS_DIR_ENTRY_DIR) {
			/* ensure the listing href base ends in '/' */
			char base[URL_MAX + 1];

			(void)snprintk(base, sizeof(base), "%s%s", url,
				       url[strlen(url) - 1] == '/' ? "" : "/");
			serve_dir(c, fs_path, base);
		} else {
			send_file(c, fs_path, ent.size);
		}
	} else if (strcmp(method, "PUT") == 0) {
		/* find Content-Length and the body start (after CRLFCRLF) */
		long clen = 0;
		const char *cl = find_header(req, "content-length:");
		const char *bodystart = strstr(req, "\r\n\r\n");
		int body_in_req = 0;

		if (cl != NULL) {
			clen = strtol(cl, NULL, 10);
		}
		if (bodystart != NULL) {
			bodystart += 4;
			body_in_req = n - (int)(bodystart - req);
			if (body_in_req < 0) {
				body_in_req = 0;
			}
		} else {
			bodystart = req + n;
		}
		recv_put(c, fs_path, bodystart, body_in_req, clen);
	} else {
		send_status(c, "405 Method Not Allowed", "GET/HEAD/PUT only\n");
	}

	return c->keepalive;
}

/* ── plain (:80) raw-socket server ──────────────────────────────────────── */

static int plain_recv(void *ctx, void *buf, size_t len)
{
	return zsock_recv(*(int *)ctx, buf, len, 0);
}

static int plain_send(void *ctx, const void *buf, size_t len)
{
	return zsock_send(*(int *)ctx, buf, len, 0);
}

static void httpd_main(void *a, void *b, void *cc)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(cc);

	int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (srv < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(HTTPD_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind :%d failed: %d", HTTPD_PORT, -errno);
		(void)zsock_close(srv);
		return;
	}
	if (zsock_listen(srv, 2) < 0) {
		LOG_ERR("listen failed: %d", -errno);
		(void)zsock_close(srv);
		return;
	}

	LOG_INF("HTTP file server on :%d (root %s)", HTTPD_PORT, DOC_ROOT);

	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = zsock_accept(srv, (struct sockaddr *)&peer, &plen);

		if (cfd < 0) {
			continue;
		}

		/* Bound blocking recv so a client that stalls mid-request can't
		 * hang this single-threaded worker indefinitely. */
		struct zsock_timeval tv = { .tv_sec = 15, .tv_usec = 0 };

		(void)zsock_setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv,
				       sizeof(tv));

		struct httpd_conn conn = {
			.io_ctx = &cfd,
			.io_recv = plain_recv,
			.io_send = plain_send,
			.xfer = xfer,
			.xfer_sz = sizeof(xfer),
		};

		/* Keep serving requests on this connection (HTTP keep-alive)
		 * until the client closes, errors, or the recv timeout fires. */
		while (httpd_serve(&conn)) {
		}
		(void)zsock_close(cfd);
	}
}

void httpd_start(void)
{
	(void)k_thread_create(&httpd_thread, httpd_stack,
			      K_THREAD_STACK_SIZEOF(httpd_stack),
			      httpd_main, NULL, NULL, NULL,
			      HTTPD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&httpd_thread, "httpd");
}
