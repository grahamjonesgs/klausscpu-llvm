/*
 * main.c — KlaussCPU Zephyr SSH shell application.
 *
 * Boot: SD card mount → DHCP → NTP time → SSH server → shell on UART.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/sntp.h>
#include <zephyr/logging/log.h>

#include "wallclock.h"

#ifdef CONFIG_KLAUSSCPU_SSH_SERVER
#include "ssh_server.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* No RTC on this board, so NTP is the time source for FatFs file timestamps.
 * pool.ntp.org rotates servers, so each sntp_simple() re-resolves to a fresh
 * one — retrying rides past a dud/unreachable pool entry or a dropped reply. */
#define NTP_SERVER       "pool.ntp.org"
#define NTP_TIMEOUT_MS   3000
#define NTP_BOOT_TRIES   3

/* ── FatFS mount ─────────────────────────────────────────────────────────── */

static FATFS fat_fs;

static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

static int mount_sd(void)
{
	int rc = disk_access_init("SD");

	if (rc != 0) {
		LOG_ERR("SD init failed: %d", rc);
		return rc;
	}

	rc = fs_mount(&fatfs_mnt);
	if (rc != 0) {
		LOG_ERR("FatFS mount failed: %d", rc);
		return rc;
	}

	LOG_INF("SD card mounted at /SD:/");
	return 0;
}

/* ── DHCP ────────────────────────────────────────────────────────────────── */

static struct net_mgmt_event_callback dhcp_cb;
static struct k_sem dhcp_sem;

static void dhcp_handler(struct net_mgmt_event_callback *cb,
			 uint32_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
		return;
	}

	char addr_str[NET_IPV4_ADDR_LEN];
	struct net_if_config *cfg = net_if_get_config(iface);

	if (cfg && cfg->ip.ipv4) {
		net_addr_ntop(AF_INET,
			      &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr,
			      addr_str, sizeof(addr_str));
		LOG_INF("DHCP: %s", addr_str);
	}

	k_sem_give(&dhcp_sem);
}

static int wait_for_dhcp(void)
{
	k_sem_init(&dhcp_sem, 0, 1);

	net_mgmt_init_event_callback(&dhcp_cb, dhcp_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);

	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface");
		return -1;
	}

	net_dhcpv4_start(iface);

	if (k_sem_take(&dhcp_sem, K_SECONDS(10)) != 0) {
		LOG_WRN("DHCP timeout");
		return -1;
	}
	return 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
	LOG_INF("KlaussCPU Zephyr SSH Shell");

	int sd_ok = (mount_sd() == 0);
	int net_ok = (wait_for_dhcp() == 0);

	/* Best-effort time at boot; the `ntp` shell command can re-sync, and
	 * `date <unix_epoch>` sets it manually. */
	if (net_ok) {
		struct sntp_time ts;
		bool got = false;

		for (int i = 0; i < NTP_BOOT_TRIES; i++) {
			if (sntp_simple(NTP_SERVER, NTP_TIMEOUT_MS, &ts) == 0) {
				wallclock_set(ts.seconds);
				LOG_INF("NTP: time set from %s", NTP_SERVER);
				got = true;
				break;
			}
		}

		if (!got) {
			LOG_WRN("NTP failed at boot; use `ntp [server]` or "
				"`date <unix_epoch>`");
		}
	}

#ifdef CONFIG_KLAUSSCPU_SSH_SERVER
	if (net_ok && sd_ok) {
		if (ssh_server_start() == 0) {
			LOG_INF("SSH ready on port 22");
		} else {
			LOG_ERR("SSH server failed");
		}
	}
#endif

	LOG_INF("System ready");

	while (1) {
		k_sleep(K_SECONDS(60));
	}
	return 0;
}
