/*
 * diskio.c — FatFs disk I/O layer for KlaussCPU SD card
 *
 * sd_read_block / sd_write_block now take a caller-supplied buffer, so
 * FatFs's window buffers are filled directly from SPI — no intermediate
 * MMIO staging, no alignment constraints.
 */

#include "ff.h"
#include "diskio.h"
#include "sd.h"
#include "../mmio.h"
#include <stddef.h>

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return sd_init() == SD_OK ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!sd_is_ready()) return STA_NOINIT;
    if (!(REG_SD_STATUS & SD_STATUS_CARD)) return STA_NODISK;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !sd_is_ready()) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (sd_read_block((uint32_t)(sector + i), buff + (size_t)i * 512u) != SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !sd_is_ready()) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (sd_write_block((uint32_t)(sector + i), buff + (size_t)i * 512u) != SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || !sd_is_ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    case GET_SECTOR_COUNT: {
        uint8_t csd[16];
        if (sd_read_csd(csd) != SD_OK) return RES_ERROR;
        if (((csd[0] >> 6) & 0x3) != 1) return RES_ERROR;  /* must be CSD v2 */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                          ((uint32_t)csd[8]            <<  8) |
                           (uint32_t)csd[9];
        *(LBA_t *)buff = (LBA_t)(c_size + 1u) * 1024u;
        return RES_OK;
    }
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25) |
           ((DWORD)5             << 21) |
           ((DWORD)6             << 16);
}
