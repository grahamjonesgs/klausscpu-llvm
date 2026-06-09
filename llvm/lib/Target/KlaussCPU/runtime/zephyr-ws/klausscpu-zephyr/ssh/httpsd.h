/*
 * httpsd.h — HTTPS file server (HTTP over TLS, serves /SD: on port 443).
 *
 * Reuses the transport-agnostic request dispatch in httpd.c (httpd_serve);
 * this layer just wraps each accepted socket in a wolfSSL TLS session.
 */

#ifndef KLAUSSCPU_HTTPSD_H_
#define KLAUSSCPU_HTTPSD_H_

/* Start the HTTPS file-server thread (:443).  Call once after the network is
 * up (DHCP bound) and the SD card is mounted (the self-signed cert is
 * generated/persisted there).  Returns 0 on success. */
int httpsd_start(void);

#endif /* KLAUSSCPU_HTTPSD_H_ */
