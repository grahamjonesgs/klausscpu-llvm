/*
 * webapi.h — small JSON control/telemetry API for the HTTP file server.
 *
 *   GET /api/status        -> { uptime_ms, switches, leds, seg, perf{...} }
 *   GET /api/leds?v=<n>     -> set LEDs (decimal or 0x hex), returns { leds }
 *   GET /api/seg?v=<n>      -> set 7-seg, returns { seg }
 *
 * Cumulative perf counters are returned raw; the client diffs successive polls
 * for live rates.  No auth — LAN/dev use, same as the file server.
 */
#ifndef KLAUSSCPU_WEBAPI_H_
#define KLAUSSCPU_WEBAPI_H_

#include <stdbool.h>

struct httpd_conn;

/* Handle an "/api/..." request: sends the response and returns true.  Returns
 * false if url is not an API route (caller falls back to file serving). */
bool webapi_handle(struct httpd_conn *c, const char *method, const char *url,
		   const char *query);

#endif /* KLAUSSCPU_WEBAPI_H_ */
