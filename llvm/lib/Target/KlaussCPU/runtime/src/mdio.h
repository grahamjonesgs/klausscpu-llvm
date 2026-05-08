// mdio.h — IEEE 802.3 Clause 22 MDIO bit-bang driver for LiteEth.
//
// Uses REG_ETH_MDIO_W (MDC/OE/MOUT) and REG_ETH_MDIO_R (MIN) from mmio.h.
// MDC rate ~500 kHz (50 delay iterations @ 100 MHz) — well under the 2.5 MHz limit.

#ifndef MDIO_H
#define MDIO_H

#include <stdint.h>

// Read a 16-bit register from the PHY at `phy_addr` (usually ETH_PHY_ADDR = 1).
uint16_t mdio_read(uint8_t phy_addr, uint8_t reg);

// Write a 16-bit value to a PHY register.
void mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val);

// Standard PHY register numbers (IEEE 802.3 Clause 22)
#define PHY_REG_BMCR    0x00   // Basic Mode Control
#define PHY_REG_BMSR    0x01   // Basic Mode Status
#define PHY_REG_ID1     0x02   // PHY Identifier 1
#define PHY_REG_ID2     0x03   // PHY Identifier 2
#define PHY_REG_ANAR    0x04   // Auto-Negotiation Advertisement
#define PHY_REG_ANLPAR  0x05   // Auto-Negotiation Link Partner Ability

// BMSR bits
#define PHY_BMSR_LINK_UP        (1u << 2)   // link is established
#define PHY_BMSR_AN_COMPLETE    (1u << 5)   // auto-negotiation complete

// BMCR bits
#define PHY_BMCR_AN_ENABLE      (1u << 12)
#define PHY_BMCR_AN_RESTART     (1u << 9)
#define PHY_BMCR_RESET          (1u << 15)

// LAN8720A identifiers (PHY_REG_ID1 = 0x0007)
#define LAN8720A_ID1    0x0007u

#endif // MDIO_H
