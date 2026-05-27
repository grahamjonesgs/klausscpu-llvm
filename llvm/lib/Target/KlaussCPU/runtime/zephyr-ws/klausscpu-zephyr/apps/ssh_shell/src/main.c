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
#include <diskio.h>
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

	/* Test FatFS directly — dump validate() fields */
	{
		static DIR dj;
		static FILINFO fno;
		FRESULT fr;
		static volatile char tb;
		const char *hex = "0123456789abcdef";

		fr = f_opendir(&dj, "SD:/");
		LOG_INF("f_opendir: %d", (int)fr);

		if (fr == FR_OK) {
			/* Dump the validate() fields manually */
			unsigned obj_id = dj.obj.id;
			unsigned fs_id = dj.obj.fs ? dj.obj.fs->id : 0xDEAD;
			unsigned fs_type = dj.obj.fs ? dj.obj.fs->fs_type : 0;

			/* Print via direct UART: "obj.id=XXXX fs->id=XXXX fs_type=XX" */
			#define TX(c) do { tb = (c); __builtin_klausscpu_txcharmemr((const void *)&tb); } while(0)
			#define TXH(v) do { TX(hex[((v)>>4)&0xF]); TX(hex[(v)&0xF]); } while(0)

			TX('o'); TX('i'); TX('d'); TX('=');
			TXH(obj_id >> 8); TXH(obj_id);
			TX(' '); TX('f'); TX('i'); TX('d'); TX('=');
			TXH(fs_id >> 8); TXH(fs_id);
			TX(' '); TX('f'); TX('t'); TX('=');
			TXH(fs_type);
			TX(' '); TX('f'); TX('s'); TX('=');
			/* Print fs pointer low 16 bits */
			unsigned long fsp = (unsigned long)dj.obj.fs;
			TXH((fsp >> 8) & 0xFF); TXH(fsp & 0xFF);
			TX('\r'); TX('\n');

			/* Check disk_status before readdir */
			{
				extern DSTATUS disk_status(BYTE pdrv);
				DSTATUS ds = disk_status(0);
				TX('D'); TX('S'); TX('=');
				TXH(ds);
				TX('\r'); TX('\n');
			}

			fr = f_readdir(&dj, &fno);
			LOG_INF("f_readdir: %d", (int)fr);

			/* Dump again after readdir */
			obj_id = dj.obj.id;
			fs_id = dj.obj.fs ? dj.obj.fs->id : 0xDEAD;
			TX('A'); TX('F'); TX(':');
			TX('o'); TX('i'); TX('d'); TX('=');
			TXH(obj_id >> 8); TXH(obj_id);
			TX(' '); TX('f'); TX('i'); TX('d'); TX('=');
			TXH(fs_id >> 8); TXH(fs_id);
			TX('\r'); TX('\n');

			f_closedir(&dj);
			#undef TX
			#undef TXH
		}
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

	/* 0. SD diagnostics — removed (data verified OK) */

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
