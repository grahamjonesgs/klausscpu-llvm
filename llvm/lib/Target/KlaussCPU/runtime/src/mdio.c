// mdio.c — IEEE 802.3 Clause 22 MDIO bit-bang via LiteEth MMIO registers.
//
// Frame structure (32 preamble bits, then ST/OP/PHYAD/REGAD/TA/DATA):
//   PRE:   32 x '1'
//   ST:    0 1
//   OP:    1 0  (read)  /  0 1  (write)
//   PHYAD: 5 bits MSB first
//   REGAD: 5 bits MSB first
//   TA:    read → Z then PHY drives '0'; write → 1 0
//   DATA:  16 bits MSB first

#include "../mmio.h"
#include "mdio.h"

// Half-period delay: ~50 iterations @ 100 MHz ≈ 500 ns → MDC ~1 MHz.
static void mdio_delay(void) {
    for (volatile int i = 0; i < 50; i++) {}
}

// Drive one bit onto MDIO with OE asserted; clock one MDC cycle.
static void mdio_write_bit(int bit) {
    uint32_t w = ETH_MDIO_OE | (bit ? ETH_MDIO_OUT : 0u);
    REG_ETH_MDIO_W = w;                        // MDC low, data stable
    mdio_delay();
    REG_ETH_MDIO_W = w | ETH_MDIO_MDC;        // MDC rising — PHY samples here
    mdio_delay();
    REG_ETH_MDIO_W = w;                        // MDC falling
    mdio_delay();
}

// Release MDIO bus, clock one MDC cycle, return sampled bit.
static int mdio_read_bit(void) {
    REG_ETH_MDIO_W = 0;                        // OE=0: release bus, MDC low
    mdio_delay();
    REG_ETH_MDIO_W = ETH_MDIO_MDC;            // MDC rising — PHY drives here
    mdio_delay();
    int bit = (int)(REG_ETH_MDIO_R & 1u);
    REG_ETH_MDIO_W = 0;                        // MDC falling
    mdio_delay();
    return bit;
}

// Send 32 '1' bits (IEEE 802.3 preamble / bus idle).
static void mdio_preamble(void) {
    for (int i = 0; i < 32; i++) mdio_write_bit(1);
}

uint16_t mdio_read(uint8_t phy_addr, uint8_t reg) {
    mdio_preamble();

    // ST: 01
    mdio_write_bit(0); mdio_write_bit(1);
    // OP: 10 (read)
    mdio_write_bit(1); mdio_write_bit(0);
    // PHYAD (5 bits, MSB first)
    for (int i = 4; i >= 0; i--) mdio_write_bit((phy_addr >> i) & 1);
    // REGAD (5 bits, MSB first)
    for (int i = 4; i >= 0; i--) mdio_write_bit((reg >> i) & 1);
    // Turnaround: ONE cycle only — LAN8720A starts driving D15 on the very
    // next rising edge; a second TA cycle loses the MSB of every read.
    mdio_read_bit();
    // DATA: 16 bits MSB first (PHY drives, we sample)
    uint16_t data = 0;
    for (int i = 15; i >= 0; i--) {
        if (mdio_read_bit()) data |= (uint16_t)(1u << i);
    }
    // Return bus to idle
    REG_ETH_MDIO_W = 0;
    return data;
}

void mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val) {
    mdio_preamble();

    // ST: 01
    mdio_write_bit(0); mdio_write_bit(1);
    // OP: 01 (write)
    mdio_write_bit(0); mdio_write_bit(1);
    // PHYAD (5 bits, MSB first)
    for (int i = 4; i >= 0; i--) mdio_write_bit((phy_addr >> i) & 1);
    // REGAD (5 bits, MSB first)
    for (int i = 4; i >= 0; i--) mdio_write_bit((reg >> i) & 1);
    // Turnaround: 1 0
    mdio_write_bit(1); mdio_write_bit(0);
    // DATA: 16 bits MSB first (we drive)
    for (int i = 15; i >= 0; i--) mdio_write_bit((val >> i) & 1);
    // Return bus to idle
    REG_ETH_MDIO_W = 0;
}
