/*
 * eth_klausscpu.c — Zephyr Ethernet driver for KlaussCPU LiteEth MAC.
 *
 * Wraps the same LiteEth MMIO hardware used by the FreeRTOS lwIP port
 * (runtime/lwip_port/ethernetif.c) into Zephyr's net_if / ethernet API.
 *
 * RX: polling thread (no ETH IRQ support yet).
 * TX: blocking wait on TX_READY, then DMA into slot SRAM.
 */
#define DT_DRV_COMPAT klausscpu_ethernet

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(eth_klausscpu, CONFIG_ETH_KLAUSSCPU_LOG_LEVEL);

/* ── MMIO register definitions (mirrors runtime/mmio.h) ─────────────────── */

#define ETH_CSR_BASE   0xF0060000u
#define ETH_SRAM_BASE  0xF0080000u
#define INTC_BASE      0xF00F0000u

#define REG(a)         (*(volatile uint32_t *)(unsigned long)(a))

#define ETH_CTRL_RESET        REG(ETH_CSR_BASE + 0x0000u)
#define ETH_CTRL_SCRATCH      REG(ETH_CSR_BASE + 0x0004u)
#define ETH_CTRL_BUS_ERRORS   REG(ETH_CSR_BASE + 0x0008u)

#define ETH_PHY_RESET         REG(ETH_CSR_BASE + 0x0800u)
#define ETH_MDIO_W            REG(ETH_CSR_BASE + 0x0804u)
#define ETH_MDIO_R            REG(ETH_CSR_BASE + 0x0808u)

#define MDIO_MDC              (1u << 0)
#define MDIO_OE               (1u << 1)
#define MDIO_OUT              (1u << 2)

#define ETH_RX_SLOT           REG(ETH_CSR_BASE + 0x1000u)
#define ETH_RX_LENGTH         REG(ETH_CSR_BASE + 0x1004u)
#define ETH_RX_ERRORS         REG(ETH_CSR_BASE + 0x1008u)
#define ETH_RX_EV_STATUS      REG(ETH_CSR_BASE + 0x100Cu)
#define ETH_RX_EV_PENDING     REG(ETH_CSR_BASE + 0x1010u)
#define ETH_RX_EV_ENABLE      REG(ETH_CSR_BASE + 0x1014u)

#define ETH_TX_START          REG(ETH_CSR_BASE + 0x1018u)
#define ETH_TX_READY          REG(ETH_CSR_BASE + 0x101Cu)
#define ETH_TX_LEVEL          REG(ETH_CSR_BASE + 0x1020u)
#define ETH_TX_SLOT           REG(ETH_CSR_BASE + 0x1024u)
#define ETH_TX_LENGTH         REG(ETH_CSR_BASE + 0x1028u)
#define ETH_TX_EV_STATUS      REG(ETH_CSR_BASE + 0x102Cu)
#define ETH_TX_EV_PENDING     REG(ETH_CSR_BASE + 0x1030u)
#define ETH_TX_EV_ENABLE      REG(ETH_CSR_BASE + 0x1034u)

#define ETH_SLOT_SIZE         2048u
#define ETH_RX_SLOT_PTR(n)    ((volatile uint8_t *)(unsigned long)(ETH_SRAM_BASE + 0x0000u + (n) * ETH_SLOT_SIZE))
#define ETH_TX_SLOT_PTR(n)    ((volatile uint8_t *)(unsigned long)(ETH_SRAM_BASE + 0x1000u + (n) * ETH_SLOT_SIZE))

#define ETH_MAX_FRAME         1514u

/* ── MDIO bit-bang (LAN8720A PHY) ────────────────────────────────────────── */

#define PHY_ADDR    1u
#define PHY_BMCR    0x00
#define PHY_BMSR    0x01
#define PHY_ID1     0x02

#define BMSR_LINK_UP       (1u << 2)
#define BMSR_AN_COMPLETE   (1u << 5)

static void mdio_delay(void)
{
	for (volatile int i = 0; i < 50; i++) {
	}
}

static void mdio_write_bit(int bit)
{
	uint32_t w = MDIO_OE | (bit ? MDIO_OUT : 0u);

	ETH_MDIO_W = w;
	mdio_delay();
	ETH_MDIO_W = w | MDIO_MDC;
	mdio_delay();
	ETH_MDIO_W = w;
	mdio_delay();
}

static int mdio_read_bit(void)
{
	int bit;

	ETH_MDIO_W = 0;
	mdio_delay();
	ETH_MDIO_W = MDIO_MDC;
	mdio_delay();
	bit = (int)(ETH_MDIO_R & 1u);
	ETH_MDIO_W = 0;
	mdio_delay();
	return bit;
}

static void mdio_preamble(void)
{
	for (int i = 0; i < 32; i++) {
		mdio_write_bit(1);
	}
}

static uint16_t mdio_read(uint8_t phy, uint8_t reg)
{
	uint16_t data = 0;

	mdio_preamble();
	mdio_write_bit(0);
	mdio_write_bit(1); /* ST */
	mdio_write_bit(1);
	mdio_write_bit(0); /* OP read */
	for (int i = 4; i >= 0; i--) {
		mdio_write_bit((phy >> i) & 1);
	}
	for (int i = 4; i >= 0; i--) {
		mdio_write_bit((reg >> i) & 1);
	}
	mdio_read_bit(); /* TA */
	for (int i = 15; i >= 0; i--) {
		if (mdio_read_bit()) {
			data |= (uint16_t)(1u << i);
		}
	}
	ETH_MDIO_W = 0;
	return data;
}

static void mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
	mdio_preamble();
	mdio_write_bit(0);
	mdio_write_bit(1); /* ST */
	mdio_write_bit(0);
	mdio_write_bit(1); /* OP write */
	for (int i = 4; i >= 0; i--) {
		mdio_write_bit((phy >> i) & 1);
	}
	for (int i = 4; i >= 0; i--) {
		mdio_write_bit((reg >> i) & 1);
	}
	mdio_write_bit(1);
	mdio_write_bit(0); /* TA */
	for (int i = 15; i >= 0; i--) {
		mdio_write_bit((val >> i) & 1);
	}
	ETH_MDIO_W = 0;
}

/* ── Volatile SRAM helpers ───────────────────────────────────────────────── */

static void sram_read(uint8_t *dst, volatile const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = src[i];
	}
}

static void sram_write(volatile uint8_t *dst, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = src[i];
	}
}

/* ── Driver data ─────────────────────────────────────────────────────────── */

struct eth_klausscpu_data {
	struct net_if *iface;
	uint8_t mac[6];
	struct k_mutex tx_lock;   /* serialise TX across threads */
	uint8_t txslot;           /* 2-slot ping-pong (0/1)      */
	K_KERNEL_STACK_MEMBER(rx_stack, CONFIG_ETH_KLAUSSCPU_RX_STACK_SIZE);
	struct k_thread rx_thread;
};

/* ── PHY + MAC init ──────────────────────────────────────────────────────── */

static void eth_hw_init(void)
{
	ETH_CTRL_RESET = 1;
	k_busy_wait(10000);
	ETH_CTRL_RESET = 0;

	ETH_PHY_RESET = 1;
	k_busy_wait(30000);
	ETH_PHY_RESET = 0;
	k_busy_wait(50000);

	uint16_t id1 = mdio_read(PHY_ADDR, PHY_ID1);

	LOG_INF("PHY ID1 = 0x%04x (expect 0x0007)", id1);

	mdio_write(PHY_ADDR, PHY_BMCR, 0x3300u);

	for (int i = 0; i < 5; i++) {
		k_busy_wait(100000);
		uint16_t bmsr = mdio_read(PHY_ADDR, PHY_BMSR);

		if (bmsr != 0xFFFFu &&
		    (bmsr & (BMSR_AN_COMPLETE | BMSR_LINK_UP)) ==
			    (BMSR_AN_COMPLETE | BMSR_LINK_UP)) {
			LOG_INF("Link up (BMSR=0x%04x)", bmsr);
			break;
		}
		if (i == 29) {
			LOG_WRN("Link timeout (BMSR=0x%04x)", bmsr);
		}
	}

	ETH_RX_EV_ENABLE = 1;
	ETH_TX_EV_ENABLE = 1;
	ETH_RX_EV_PENDING = 1;
	ETH_TX_EV_PENDING = 1;
}

/* ── TX ──────────────────────────────────────────────────────────────────── */

static int eth_klausscpu_send(const struct device *dev, struct net_pkt *pkt)
{
	struct eth_klausscpu_data *data = dev->data;
	uint16_t total_len = net_pkt_get_len(pkt);

	if (total_len > ETH_MAX_FRAME) {
		LOG_ERR("TX frame too large: %u", total_len);
		return -EMSGSIZE;
	}

	/* Serialise TX: eth_klausscpu_send is called from multiple threads (the RX
	 * thread emitting replies, the net-TX/shell thread emitting requests).  The
	 * TX registers + slot SRAM are shared, so without this lock concurrent
	 * transmits clobber each other (symptom: self-initiated frames never egress
	 * while reply frames do).  Also ping-pong the 2 HW slots so a new frame is
	 * never written into the slot the hardware is still transmitting. */
	k_mutex_lock(&data->tx_lock, K_FOREVER);

	/* This HW transmits reliably only from slot 0 — a 2-slot ping-pong made
	 * every frame that landed on slot 1 silently fail to egress (every-other-
	 * packet loss).  Always use slot 0; the mutex serialises concurrent TX. */
	volatile uint8_t *slot = ETH_TX_SLOT_PTR(0);
	struct net_buf *frag;
	uint32_t offset = 0;

	for (frag = pkt->frags; frag; frag = frag->frags) {
		sram_write(slot + offset, frag->data, frag->len);
		offset += frag->len;
	}

	while (!ETH_TX_READY) {
		k_yield();
	}

	ETH_TX_SLOT = 0;
	ETH_TX_LENGTH = total_len;
	ETH_TX_START = 1;

	k_mutex_unlock(&data->tx_lock);

	return 0;
}

/* ── RX polling thread ───────────────────────────────────────────────────── */

static void eth_klausscpu_rx_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct eth_klausscpu_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		if (!(ETH_RX_EV_PENDING & 1u)) {
			k_sleep(K_MSEC(1));
			continue;
		}

		uint32_t slot = ETH_RX_SLOT;
		uint32_t len = ETH_RX_LENGTH;

		if (len < 14 || len > ETH_SLOT_SIZE) {
			ETH_RX_EV_PENDING = 1;
			continue;
		}

		struct net_pkt *pkt = net_pkt_rx_alloc_with_buffer(
			data->iface, len, AF_UNSPEC, 0, K_NO_WAIT);
		if (!pkt) {
			LOG_ERR("RX: failed to allocate pkt");
			ETH_RX_EV_PENDING = 1;
			continue;
		}

		uint8_t rxbuf[ETH_MAX_FRAME];

		sram_read(rxbuf, ETH_RX_SLOT_PTR(slot), len);
		ETH_RX_EV_PENDING = 1;

		if (net_pkt_write(pkt, rxbuf, len) < 0) {
			LOG_ERR("RX: failed to write pkt data");
			net_pkt_unref(pkt);
			continue;
		}

		int ret = net_recv_data(data->iface, pkt);

		if (ret < 0) {
			LOG_ERR("RX: net_recv_data failed: %d", ret);
			net_pkt_unref(pkt);
		}
	}
}

/* ── Zephyr ethernet API ─────────────────────────────────────────────────── */

static void eth_klausscpu_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct eth_klausscpu_data *data = dev->data;

	data->iface = iface;

	net_if_set_link_addr(iface, data->mac, sizeof(data->mac),
			     NET_LINK_ETHERNET);

	ethernet_init(iface);
}

static enum ethernet_hw_caps eth_klausscpu_get_capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);
	return ETHERNET_LINK_100BASE_T;
}

static const struct ethernet_api eth_klausscpu_api = {
	.iface_api.init = eth_klausscpu_iface_init,
	.get_capabilities = eth_klausscpu_get_capabilities,
	.send = eth_klausscpu_send,
};

/* ── Init ────────────────────────────────────────────────────────────────── */

static int eth_klausscpu_init(const struct device *dev)
{
	struct eth_klausscpu_data *data = dev->data;

	k_mutex_init(&data->tx_lock);
	data->txslot = 0;

	data->mac[0] = 0x00;
	data->mac[1] = 0xAB;
	data->mac[2] = 0xCD;
	data->mac[3] = 0x00;
	data->mac[4] = 0x00;
	data->mac[5] = 0x01;

	eth_hw_init();

	k_thread_create(&data->rx_thread, data->rx_stack,
			CONFIG_ETH_KLAUSSCPU_RX_STACK_SIZE,
			eth_klausscpu_rx_thread, (void *)dev, NULL, NULL,
			K_PRIO_COOP(CONFIG_ETH_KLAUSSCPU_RX_PRIO),
			0, K_NO_WAIT);
	k_thread_name_set(&data->rx_thread, "eth_rx");

	LOG_INF("KlaussCPU Ethernet initialized");
	return 0;
}

static struct eth_klausscpu_data eth_data;

ETH_NET_DEVICE_DT_INST_DEFINE(0,
	eth_klausscpu_init,
	NULL,
	&eth_data,
	NULL,
	CONFIG_ETH_KLAUSSCPU_INIT_PRIORITY,
	&eth_klausscpu_api,
	ETH_MAX_FRAME);
