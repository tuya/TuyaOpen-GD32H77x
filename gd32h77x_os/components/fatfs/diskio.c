/**
 * @file diskio.c
 * @brief FatFS disk I/O glue for the GD32H77X SDIO0 card driver.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "ff.h"
#include "diskio.h"

#include <string.h>

#include "gd32h77x_sdcard.h"
#include "gd32h77x_78x_sdio.h"

#define SD_SECTOR_SIZE 512U

static DSTATUS s_disk_status = STA_NOINIT;
static sd_card_info_struct s_card_info;
static uint32_t s_disk_bounce[SD_SECTOR_SIZE / sizeof(uint32_t)] __attribute__((aligned(32)));

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0U) {
        return STA_NOINIT;
    }
    return s_disk_status;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    uint32_t card_state = 0U;

    if (pdrv != 0U) {
        return STA_NOINIT;
    }
    if ((s_disk_status & STA_NOINIT) == 0U) {
        return s_disk_status;
    }
    if (sd_init() != SD_OK ||
        sd_card_information_get(&s_card_info) != SD_OK ||
        sd_card_select_deselect(s_card_info.card_rca) != SD_OK ||
        sd_cardstatus_get(&card_state) != SD_OK ||
        sd_bus_mode_config(SDIO_BUSMODE_4BIT, SD_SPEED_DEFAULT) != SD_OK ||
        sd_transfer_mode_config(SD_POLLING_MODE) != SD_OK) {
        return STA_NOINIT;
    }
    s_disk_status = 0U;
    return s_disk_status;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || buff == NULL || count == 0U) {
        return RES_PARERR;
    }
    if (s_disk_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    for (UINT i = 0U; i < count; ++i) {
        uint32_t *target = (uint32_t *)(void *)buff;
        if (((uintptr_t)buff & (sizeof(uint32_t) - 1U)) != 0U) {
            target = s_disk_bounce;
        }
        if (sd_block_read(target, sector + i, SD_SECTOR_SIZE) != SD_OK) {
            return RES_ERROR;
        }
        if (target == s_disk_bounce) {
            memcpy(buff, s_disk_bounce, SD_SECTOR_SIZE);
        }
        buff += SD_SECTOR_SIZE;
    }
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || buff == NULL || count == 0U) {
        return RES_PARERR;
    }
    if (s_disk_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    for (UINT i = 0U; i < count; ++i) {
        const uint32_t *source = (const uint32_t *)(const void *)buff;
        if (((uintptr_t)buff & (sizeof(uint32_t) - 1U)) != 0U) {
            memcpy(s_disk_bounce, buff, SD_SECTOR_SIZE);
            source = s_disk_bounce;
        }
        if (sd_block_write((uint32_t *)(uintptr_t)source, sector + i,
                           SD_SECTOR_SIZE) != SD_OK) {
            return RES_ERROR;
        }
        buff += SD_SECTOR_SIZE;
    }
    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0U) {
        return RES_PARERR;
    }
    if (s_disk_status & STA_NOINIT) {
        return RES_NOTRDY;
    }

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            if (buff == NULL) {
                return RES_PARERR;
            }
            /* sd_card_capacity_get() reports KiB. */
            *(LBA_t *)buff = (LBA_t)sd_card_capacity_get() * 2U;
            return RES_OK;
        case GET_SECTOR_SIZE:
            if (buff == NULL) {
                return RES_PARERR;
            }
            *(WORD *)buff = SD_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            if (buff == NULL) {
                return RES_PARERR;
            }
            *(DWORD *)buff = 1U;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
