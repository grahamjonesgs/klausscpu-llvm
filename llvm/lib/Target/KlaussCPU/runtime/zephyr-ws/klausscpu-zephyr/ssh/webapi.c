/*
 * webapi.c — JSON control/telemetry API for the KlaussCPU HTTP file server.
 * See webapi.h for the routes.  Reuses the board MMIO accessors (board_io.h)
 * and httpd_send() for the response.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <stdlib.h>

#include "httpd.h"
#include "webapi.h"
#include "board_io.h"

/* Extract an unsigned value for `key` from a "k=v&k2=v2" query string.
 * Base 0: accepts decimal or 0x-prefixed hex. */
static bool query_ulong(const char *query, const char *key, unsigned long *out)
{
	size_t klen = strlen(key);

	for (const char *p = query; p != NULL && *p != '\0';) {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			*out = strtoul(p + klen + 1, NULL, 0);
			return true;
		}
		const char *amp = strchr(p, '&');

		if (amp == NULL) {
			break;
		}
		p = amp + 1;
	}
	return false;
}

static void api_status(struct httpd_conn *c)
{
	struct board_perf p;

	board_perf_read(&p);

	(void)snprintk(c->xfer, c->xfer_sz,
		"{\"uptime_ms\":%llu,\"switches\":%u,\"leds\":%u,\"seg\":%u,"
		"\"perf\":{"
		"\"cycles\":%llu,\"instr\":%llu,\"fetch\":%llu,\"exec\":%llu,"
		"\"mul\":%llu,\"div\":%llu,\"intc\":%llu,\"idle\":%llu,"
		"\"alu\":%llu,\"load\":%llu,\"store\":%llu,\"branch\":%llu,"
		"\"taken\":%llu,\"jump\":%llu,\"call\":%llu,\"ind\":%llu,"
		"\"other\":%llu,"
		"\"rh\":%llu,\"rm\":%llu,\"wh\":%llu,\"wm\":%llu,"
		"\"wb\":%llu,\"stall\":%llu}}",
		(unsigned long long)k_uptime_get(),
		(unsigned)board_switches_get(), (unsigned)board_leds_get(),
		(unsigned)board_seg_get(),
		(unsigned long long)p.cycles, (unsigned long long)p.instr,
		(unsigned long long)p.fetch,  (unsigned long long)p.exec,
		(unsigned long long)p.mul,    (unsigned long long)p.div,
		(unsigned long long)p.intc,   (unsigned long long)p.idle,
		(unsigned long long)p.alu,    (unsigned long long)p.load,
		(unsigned long long)p.store,  (unsigned long long)p.branch,
		(unsigned long long)p.taken,  (unsigned long long)p.jump,
		(unsigned long long)p.call,   (unsigned long long)p.ind,
		(unsigned long long)p.other,
		(unsigned long long)p.rh, (unsigned long long)p.rm,
		(unsigned long long)p.wh, (unsigned long long)p.wm,
		(unsigned long long)p.wb, (unsigned long long)p.stall);

	httpd_send(c, "200 OK", "application/json", c->xfer);
}

bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query)
{
	ARG_UNUSED(method);

	if (strncmp(url, "/api/", 5) != 0) {
		return false;
	}

	if (strcmp(url, "/api/status") == 0) {
		api_status(c);
		return true;
	}

	if (strcmp(url, "/api/leds") == 0) {
		unsigned long v;

		if (query_ulong(query, "v", &v)) {
			board_leds_set((uint16_t)v);
		}
		(void)snprintk(c->xfer, c->xfer_sz, "{\"leds\":%u}",
			       (unsigned)board_leds_get());
		httpd_send(c, "200 OK", "application/json", c->xfer);
		return true;
	}

	if (strcmp(url, "/api/seg") == 0) {
		unsigned long v;

		if (query_ulong(query, "v", &v)) {
			board_seg_set((uint32_t)v);
		}
		(void)snprintk(c->xfer, c->xfer_sz, "{\"seg\":%u}",
			       (unsigned)board_seg_get());
		httpd_send(c, "200 OK", "application/json", c->xfer);
		return true;
	}

	httpd_send(c, "404 Not Found", "application/json",
		   "{\"error\":\"unknown api route\"}");
	return true;
}
