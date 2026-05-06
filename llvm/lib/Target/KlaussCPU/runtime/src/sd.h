/*
 * sd.h — KlaussCPU SD card SPI driver
 *
 * Hardware: single-byte SPI controller via MMIO.
 * Protocol: SDHC/SDXC only (block-addressed, CMD8 + ACMD41 HCS=1).
 * Thread safety: none — single-threaded use only.
 *
 * Read/write take a caller-supplied 512-byte buffer.  Bytes are transferred
 * directly between SPI and the caller's RAM buffer — the hardware MMIO sector
 * buffer is not used, avoiding any alignment constraints on the caller.
 */
#ifndef SD_H
#define SD_H

#include <stdint.h>

/* ── Return codes (negative = error) ──────────────────────────────────────── */

#define SD_OK           0
#define SD_ERR_NORESP  -1   /* CMD0 got no valid response */
#define SD_ERR_NOTV2   -2   /* CMD8 rejected — not an SDv2 card */
#define SD_ERR_TIMEOUT -3   /* ACMD41 timed out — card stuck in idle */
#define SD_ERR_CMD     -4   /* unexpected R1 response to a command */
#define SD_ERR_TOKEN   -5   /* no data-start token (CMD17/CMD18) */
#define SD_ERR_WRITE   -6   /* card rejected the write data block */

/* ── API ─────────────────────────────────────────────────────────────────── */

int sd_init(void);

/*
 * sd_read_block  — read 512 bytes from block lba into buf.
 * sd_write_block — write 512 bytes from buf to block lba.
 * buf must be at least 512 bytes; no alignment requirement.
 */
int sd_read_block(uint32_t lba, uint8_t *buf);
int sd_write_block(uint32_t lba, const uint8_t *buf);

int sd_read_csd(uint8_t out[16]);
int sd_is_ready(void);

#endif /* SD_H */
