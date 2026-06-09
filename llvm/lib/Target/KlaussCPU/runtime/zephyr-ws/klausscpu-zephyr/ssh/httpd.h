/*
 * httpd.h — minimal raw-socket HTTP file server (serves /SD: on port 80).
 */

#ifndef KLAUSSCPU_HTTPD_H_
#define KLAUSSCPU_HTTPD_H_

/* Start the HTTP file-server worker thread.  Call once after the network is
 * up (DHCP bound). */
void httpd_start(void);

#endif /* KLAUSSCPU_HTTPD_H_ */
