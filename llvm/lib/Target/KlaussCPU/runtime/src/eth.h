// eth.h — KlaussCPU LiteEth raw L2 driver.
//
// Sits below an IP stack (lwIP ethernetif.c) or is used directly for
// low-level Ethernet tests.  Handles PHY init, TX, and polled RX.
// Does NOT implement IP, ARP, UDP, or TCP.

#ifndef ETH_H
#define ETH_H

#include <stdint.h>

// Initialise the PHY and MAC.  Blocks for ~150 ms (reset + auto-negotiation).
// Call once before rtos_start() or from a dedicated init task.
void eth_init(void);

// Send `len` bytes from `frame` (must include the 14-byte Ethernet II header).
// Blocks until TX_READY, copies frame into next TX slot, then kicks the MAC.
// Returns 0 on success, -1 if len is out of range.
int eth_tx(const void *frame, uint32_t len);

// Polled receive.  If a frame is pending, copies up to `max` bytes into `buf`
// and returns the frame length (≥ 14).  Returns 0 if no frame is available.
// The 14-byte Ethernet header is included; CRC is stripped by the MAC.
uint32_t eth_rx_poll(void *buf, uint32_t max);

// Send a gratuitous ARP request (who-has our_ip, tell our_ip / our_mac).
// our_ip in host byte order.  Useful as the first on-wire test.
int eth_send_arp_request(const uint8_t our_mac[6], uint32_t our_ip,
                         uint32_t target_ip);

// Ethernet frame maximum payload size (no jumbo frames).
#define ETH_MAX_FRAME  1514u   // 14-byte header + 1500-byte payload

// Default MAC address (globally-administered unicast; override per-board).
extern const uint8_t g_eth_mac[6];

#endif // ETH_H
