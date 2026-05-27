/*
 * main.c — KlaussCPU Zephyr SSH shell application.
 *
 * Boot sequence: SD card mount → DHCP → shell ready on UART.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_KLAUSSCPU_SSH_SERVER
#include "ssh_server.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define REG(a)       (*(volatile uint32_t *)(unsigned long)(a))
#define REG_LEDS     REG(0xF0040000u)
#define REG_SEG7     REG(0xF0030010u)

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
		LOG_ERR("SD disk_access_init failed: %d", rc);
		return rc;
	}

	rc = fs_mount(&fatfs_mnt);
	if (rc != 0) {
		LOG_ERR("FatFS mount failed: %d", rc);
		return rc;
	}

	LOG_INF("SD card mounted at /SD:/");

	struct fs_dir_t dir;
	struct fs_dirent entry;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, "/SD:/") == 0) {
		int count = 0;

		while (fs_readdir(&dir, &entry) == 0 &&
		       entry.name[0] != '\0') {
			count++;
		}
		fs_closedir(&dir);
		LOG_INF("SD card: %d entries in root", count);
	}

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
		LOG_INF("DHCP bound: %s", addr_str);
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
		LOG_ERR("No network interface found");
		return -1;
	}

	net_dhcpv4_start(iface);
	LOG_INF("DHCP started, waiting for address...");

	if (k_sem_take(&dhcp_sem, K_SECONDS(10)) != 0) {
		LOG_WRN("DHCP timeout — continuing without IP");
		return -1;
	}
	return 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
	REG_LEDS = 0x0001;
	REG_SEG7 = 0x0001;

	LOG_INF("KlaussCPU Zephyr SSH Shell starting...");

	/* 1. Mount SD card */
	REG_LEDS = 0x0003;
	int sd_ok = (mount_sd() == 0);

	if (sd_ok) {
		REG_SEG7 = 0x0002;
	}

	/* 2. Wait for DHCP */
	REG_LEDS = 0x0007;
	int net_ok = (wait_for_dhcp() == 0);

	if (net_ok) {
		REG_SEG7 = 0x0003;
	}

#ifdef CONFIG_KLAUSSCPU_SSH_SERVER
	/* 3. Start SSH server */
	if (net_ok && sd_ok) {
		REG_LEDS = 0x000F;
		if (ssh_server_start() == 0) {
			REG_SEG7 = 0x0004;
			LOG_INF("SSH server ready on port 22 (admin/klausscpu)");
		} else {
			LOG_ERR("SSH server failed to start");
		}
	}
#endif

	REG_LEDS = 0x001F;
	REG_SEG7 = 0x0005;
	LOG_INF("System ready. Shell on UART, SSH on port 22.");

	while (1) {
		k_sleep(K_SECONDS(60));
	}
	return 0;
}
