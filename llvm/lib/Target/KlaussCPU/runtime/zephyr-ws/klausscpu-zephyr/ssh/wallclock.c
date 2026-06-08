/* wallclock.c — software wall-clock + FatFs get_fattime().  See wallclock.h.
 *
 * Compiled whenever FatFs is present (CONFIG_FAT_FILESYSTEM_ELM): with the
 * vendored zephyr_fatfs_config.h now setting FF_FS_NORTC=0, FatFs requires a
 * get_fattime() to stamp files. */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <ff.h>

#include "wallclock.h"

/* Unix epoch corresponding to uptime 0; valid only once set from SNTP. */
static int64_t epoch_base;
static bool    clock_valid;

void wallclock_set(uint64_t unix_seconds)
{
	epoch_base = (int64_t)unix_seconds - (k_uptime_get() / 1000);
	clock_valid = true;
}

uint64_t wallclock_now(void)
{
	if (!clock_valid) {
		return 0;
	}
	return (uint64_t)(epoch_base + (k_uptime_get() / 1000));
}

/* Break a Unix epoch (UTC) into calendar fields (Howard Hinnant's algorithm). */
static void epoch_to_civil(uint64_t t, unsigned *year, unsigned *mon,
			   unsigned *day, unsigned *hour, unsigned *min,
			   unsigned *sec)
{
	uint32_t secs = (uint32_t)(t % 86400u);

	*hour = secs / 3600u;
	*min  = (secs % 3600u) / 60u;
	*sec  = secs % 60u;

	int64_t z = (int64_t)(t / 86400u) + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t doe = (uint64_t)(z - era * 146097);
	uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint64_t mp  = (5 * doy + 2) / 153;

	*day  = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	*mon  = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
	*year = (unsigned)((int64_t)yoe + era * 400 + (*mon <= 2));
}

int wallclock_str(char *out, size_t n)
{
	uint64_t t = wallclock_now();

	if (t == 0) {
		return -1;
	}

	unsigned y, mo, d, h, mi, s;

	epoch_to_civil(t, &y, &mo, &d, &h, &mi, &s);
	snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u", y, mo, d, h, mi, s);
	return 0;
}

/* FatFs current time, packed:
 *   bits 31..25 year-1980, 24..21 month(1-12), 20..16 day(1-31),
 *   bits 15..11 hour,      10..5  minute,        4..0  second/2.
 * UTC.  Before SNTP sets the clock, fall back to 2022-01-01 (what the old
 * FF_FS_NORTC=1 build hard-coded). */
DWORD get_fattime(void)
{
	uint64_t t = wallclock_now();
	unsigned year, mon, day, hour, min, sec;

	if (t == 0) {
		year = 2022; mon = 1; day = 1; hour = 0; min = 0; sec = 0;
	} else {
		epoch_to_civil(t, &year, &mon, &day, &hour, &min, &sec);
	}

	if (year < 1980) {
		year = 1980;
	}

	return ((DWORD)(year - 1980) << 25) | ((DWORD)mon << 21) |
	       ((DWORD)day << 16) | ((DWORD)hour << 11) |
	       ((DWORD)min << 5) | ((DWORD)(sec >> 1));
}
