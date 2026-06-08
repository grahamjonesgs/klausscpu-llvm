/* wallclock.h — minimal software wall-clock for KlaussCPU.
 *
 * KlaussCPU has no RTC, only an uptime counter.  This keeps a Unix-epoch base
 * (set once from SNTP at boot) and derives the current time as base + uptime.
 * It backs FatFs's get_fattime(), so files created/modified after the clock is
 * set get real timestamps (which `ls` then displays).
 */

#ifndef WALLCLOCK_H
#define WALLCLOCK_H

#include <stdint.h>

/* Set the wall clock from a Unix epoch (seconds), e.g. an SNTP result. */
void wallclock_set(uint64_t unix_seconds);

/* Current Unix epoch seconds, or 0 if the clock has never been set. */
uint64_t wallclock_now(void);

/* Format the current UTC time as "YYYY-MM-DD HH:MM:SS" into out.
 * Returns 0 on success, -1 if the clock has not been set (e.g. SNTP failed). */
int wallclock_str(char *out, size_t n);

#endif /* WALLCLOCK_H */
