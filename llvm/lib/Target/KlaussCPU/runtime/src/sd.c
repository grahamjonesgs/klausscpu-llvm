/*
 * sd.c — KlaussCPU SD card SPI driver
 *
 * SPI bytes go directly between the hardware SPI controller and the caller's
 * RAM buffer — no staging through the MMIO sector buffer, so there are no
 * alignment constraints on the caller's pointer.
 */

#include "sd.h"
#include "../mmio.h"

static int s_ready;

/* ── Low-level SPI ───────────────────────────────────────────────────────── */

static uint8_t spi_xfer(uint8_t tx) {
    REG_SD_DATA = tx;
    while (REG_SD_STATUS & SD_STATUS_BUSY) {}
    return (uint8_t)REG_SD_DATA;
}

static void cs_assert(void) {
    REG_SD_CTRL = (REG_SD_CTRL & 0xFFFFu) | SD_CTRL_PWR_ON;
}

static void cs_deassert(void) {
    REG_SD_CTRL = (REG_SD_CTRL & 0xFFFFu) | SD_CTRL_CS_HI | SD_CTRL_PWR_ON;
}

static void set_clk(uint16_t div) {
    REG_SD_CTRL = (REG_SD_CTRL & ~0xFFFFu) | SD_CTRL_CLKDIV(div);
}

/* Send 6-byte SD command; return R1 (poll up to 8 bytes). */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    spi_xfer(0xFF);
    spi_xfer(0x40 | (cmd & 0x3F));
    spi_xfer((uint8_t)(arg >> 24));
    spi_xfer((uint8_t)(arg >> 16));
    spi_xfer((uint8_t)(arg >>  8));
    spi_xfer((uint8_t)(arg      ));
    spi_xfer(crc | 0x01);
    for (int i = 0; i < 8; i++) {
        uint8_t r = spi_xfer(0xFF);
        if ((r & 0x80) == 0) return r;
    }
    return 0xFF;
}

static uint8_t sd_acmd(uint8_t acmd, uint32_t arg) {
    sd_cmd(55, 0, 0x65);
    return sd_cmd(acmd, arg, 0x77);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int sd_init(void) {
    s_ready = 0;
    REG_SD_CTRL = SD_CTRL_CLKDIV(SD_CLK_INIT) | SD_CTRL_CS_HI | SD_CTRL_PWR_ON;

    /* ≥1 ms power-up wait */
    for (volatile int i = 0; i < 100000; i++) {}

    /* ≥74 dummy clocks with CS high */
    for (int i = 0; i < 10; i++) spi_xfer(0xFF);

    cs_assert();

    if (sd_cmd(0, 0, 0x95) != 0x01) { cs_deassert(); return SD_ERR_NORESP; }
    if (sd_cmd(8, 0x000001AAu, 0x87) != 0x01) { cs_deassert(); return SD_ERR_NOTV2; }
    spi_xfer(0xFF); spi_xfer(0xFF); spi_xfer(0xFF); spi_xfer(0xFF);

    for (int tries = 0; tries < 2000; tries++) {
        if (sd_acmd(41, 0x40000000u) == 0x00) goto ready;
        for (volatile int d = 0; d < 500; d++) {}
    }
    cs_deassert();
    return SD_ERR_TIMEOUT;

ready:
    cs_deassert();
    set_clk(SD_CLK_FAST);
    s_ready = 1;
    return SD_OK;
}

int sd_read_block(uint32_t lba, uint8_t *buf) {
    cs_assert();
    if (sd_cmd(17, lba, 0xFF) != 0x00) { cs_deassert(); return SD_ERR_CMD; }

    for (int tries = 0; tries < 200000; tries++) {
        if (spi_xfer(0xFF) == 0xFE) goto got_token;
    }
    cs_deassert();
    return SD_ERR_TOKEN;

got_token:
    /* Receive 512 bytes directly into the caller's buffer. */
    for (int i = 0; i < 512; i++)
        buf[i] = spi_xfer(0xFF);

    spi_xfer(0xFF); spi_xfer(0xFF);   /* discard CRC16 */
    cs_deassert();
    return SD_OK;
}

int sd_write_block(uint32_t lba, const uint8_t *buf) {
    cs_assert();
    if (sd_cmd(24, lba, 0xFF) != 0x00) { cs_deassert(); return SD_ERR_CMD; }

    spi_xfer(0xFF);    /* gap */
    spi_xfer(0xFE);    /* data-start token */

    /* Send 512 bytes directly from the caller's buffer. */
    for (int i = 0; i < 512; i++)
        spi_xfer(buf[i]);

    spi_xfer(0xFF); spi_xfer(0xFF);   /* dummy CRC16 */

    uint8_t resp = spi_xfer(0xFF) & 0x1F;
    if (resp != 0x05) { cs_deassert(); return SD_ERR_WRITE; }

    for (int tries = 0; tries < 200000; tries++) {
        if (spi_xfer(0xFF) != 0x00) break;
    }
    cs_deassert();
    return SD_OK;
}

int sd_read_csd(uint8_t out[16]) {
    cs_assert();
    if (sd_cmd(9, 0, 0xAF) != 0x00) { cs_deassert(); return SD_ERR_CMD; }

    for (int tries = 0; tries < 200000; tries++) {
        if (spi_xfer(0xFF) == 0xFE) goto got_csd;
    }
    cs_deassert();
    return SD_ERR_TOKEN;

got_csd:
    for (int i = 0; i < 16; i++)
        out[i] = spi_xfer(0xFF);
    spi_xfer(0xFF); spi_xfer(0xFF);
    cs_deassert();
    return SD_OK;
}

int sd_is_ready(void) {
    return s_ready;
}
