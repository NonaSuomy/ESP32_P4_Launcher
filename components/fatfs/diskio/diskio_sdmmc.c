/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "diskio_impl.h"
#include "ffconf.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "esp_log.h"
#include "esp_compiler.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF's sdmmc component (sdmmc/sdmmc_common.h, not installed): used by the 1-bit fallback */
esp_err_t sdmmc_send_cmd(sdmmc_card_t* card, sdmmc_command_t* cmd);
esp_err_t sdmmc_send_cmd_set_bus_width(sdmmc_card_t* card, int width);
esp_err_t sdmmc_send_cmd_send_status(sdmmc_card_t* card, uint32_t* out_status);

static sdmmc_card_t* s_cards[FF_VOLUMES] = { NULL };
static bool s_disk_status_check_en[FF_VOLUMES] = { };
static bool s_fell_back[FF_VOLUMES] = { };      /* launcher: now on the 1-bit bus */

static const char* TAG = "diskio_sdmmc";

/* Launcher addition (README_LAUNCHER.md): a card whose 4-bit transfers fail is moved to the 1-bit bus,
 * which needs D0 only, and the transfer is repeated once; it stays 1-bit until the next boot.
 * Seen on a Guition 7" with a v3.2 chip (bad contact on D1-D3): after a multi-block write the card
 * does not answer the controller's automatic STOP, stays in the receive state, and every later write
 * times out. So: STOP by hand, wait for the transfer state, then ACMD6 + the host to 1 bit. No card
 * re-init (CMD0) and no controller reset: the controller is shared with the C6 on SDIO, a reset under
 * its running transfers hangs the board, and a re-init left the controller "hardware locked". */
static bool ff_sdmmc_fall_back(BYTE pdrv, esp_err_t err)
{
    sdmmc_card_t* card = s_cards[pdrv];
    if (s_fell_back[pdrv] || card->log_bus_width == 0 || !card->is_mem || card->is_mmc ||
        (card->host.flags & SDMMC_HOST_FLAG_SPI) ||
        (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_CRC)) {
        return false;
    }
    s_fell_back[pdrv] = true;
    ESP_LOGW(TAG, "%d-bit transfer failed (0x%x): moving the card to the 1-bit bus",
             1 << card->log_bus_width, err);
    uint32_t status = 0;
    int state = -1;
    for (int i = 0; i < 20; i++) {
        if (sdmmc_send_cmd_send_status(card, &status) == ESP_OK) {
            state = MMC_R1_CURRENT_STATE_STATUS(status);
            if (state == MMC_R1_CURRENT_STATE_TRAN && (status & MMC_R1_READY_FOR_DATA)) {
                break;
            }
        }
        if (state != 7 && state != MMC_R1_CURRENT_STATE_TRAN) {   /* 7 = "prg": wait; "rcv"/"data": STOP */
            sdmmc_command_t stop = {
                .opcode = MMC_STOP_TRANSMISSION,
                .flags = SCF_CMD_AC | SCF_RSP_R1B | SCF_WAIT_BUSY,
                .timeout_ms = 1000,
            };
            esp_err_t e = sdmmc_send_cmd(card, &stop);
            ESP_LOGW(TAG, "card state %d (status 0x%" PRIx32 "): STOP -> 0x%x", state, status, e);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        state = -1;
    }
    if (state != MMC_R1_CURRENT_STATE_TRAN) {
        ESP_LOGE(TAG, "card not back in the transfer state (status 0x%" PRIx32 ")", status);
        return false;
    }
    esp_err_t e = sdmmc_send_cmd_set_bus_width(card, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ACMD6 (1 bit) failed (0x%x)", e);
        return false;
    }
    e = card->host.set_bus_width(card->host.slot, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "host 1-bit failed (0x%x)", e);
        return false;
    }
    card->log_bus_width = 0;
    ESP_LOGW(TAG, "SD card on the 1-bit bus now (slower) until the next boot");
    return true;
}

int ff_sdmmc_bus_width(void)
{
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (s_cards[i]) {
            return 1 << s_cards[i]->log_bus_width;
        }
    }
    return 0;
}

bool ff_sdmmc_bus_fell_back(void)
{
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (s_fell_back[i]) {
            return true;
        }
    }
    return false;
}

//Check if SD/MMC card is present
static DSTATUS ff_sdmmc_card_available(BYTE pdrv)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_get_status(card);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "Check status failed (0x%x)", err);
        return STA_NOINIT;
    }
    return 0;
}

/**
*   ff_sdmmc_status() and ff_sdmmc_initialize() return STA_NOINIT when sdmmc_get_status()
*   fails. This error value is checked throughout the FATFS code.
*   Both functions return 0 on success.
*/
static DSTATUS ff_sdmmc_initialize (BYTE pdrv)
{
    return ff_sdmmc_card_available(pdrv);
}

static DSTATUS ff_sdmmc_status(BYTE pdrv)
{
    if (s_disk_status_check_en[pdrv]) {
        return ff_sdmmc_card_available(pdrv);
    }
    return 0;
}

static DRESULT ff_sdmmc_read (BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_read_sectors(card, buff, sector, count);
    if (unlikely(err != ESP_OK) && ff_sdmmc_fall_back(pdrv, err)) {
        err = sdmmc_read_sectors(card, buff, sector, count);
    }
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "sdmmc_read_blocks failed (0x%x)", err);
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT ff_sdmmc_write (BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    esp_err_t err = sdmmc_write_sectors(card, buff, sector, count);
    if (unlikely(err != ESP_OK) && ff_sdmmc_fall_back(pdrv, err)) {
        err = sdmmc_write_sectors(card, buff, sector, count);
    }
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "sdmmc_write_blocks failed (0x%x)", err);
        return RES_ERROR;
    }
    return RES_OK;
}

#if FF_USE_TRIM
static DRESULT ff_sdmmc_trim (BYTE pdrv, DWORD start_sector, DWORD sector_count)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    sdmmc_erase_arg_t arg;

    arg = sdmmc_can_discard(card) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
    esp_err_t err = sdmmc_erase_sectors(card, start_sector, sector_count, arg);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "sdmmc_erase_sectors failed (%d)", err);
        return RES_ERROR;
    }
    return RES_OK;
}
#endif //FF_USE_TRIM

static DRESULT ff_sdmmc_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
    sdmmc_card_t* card = s_cards[pdrv];
    assert(card);
    switch(cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD*) buff) = card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD*) buff) = card->csd.sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;
#if FF_USE_TRIM
        case CTRL_TRIM:
            if (sdmmc_can_trim(card) != ESP_OK) {
                return RES_PARERR;
            }
            return ff_sdmmc_trim (pdrv, *((DWORD*)buff), //start_sector
                    (*((DWORD*)buff + 1) - *((DWORD*)buff) + 1)); //sector_count
#endif //FF_USE_TRIM
    }
    return RES_ERROR;
}

void ff_sdmmc_set_disk_status_check(BYTE pdrv, bool enable)
{
    s_disk_status_check_en[pdrv] = enable;
}

void ff_diskio_register_sdmmc(BYTE pdrv, sdmmc_card_t* card)
{
    static const ff_diskio_impl_t sdmmc_impl = {
        .init = &ff_sdmmc_initialize,
        .status = &ff_sdmmc_status,
        .read = &ff_sdmmc_read,
        .write = &ff_sdmmc_write,
        .ioctl = &ff_sdmmc_ioctl
    };
    s_cards[pdrv] = card;
    s_disk_status_check_en[pdrv] = false;
    ff_diskio_register(pdrv, &sdmmc_impl);
}

BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t* card)
{
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (card == s_cards[i]) {
            return i;
        }
    }
    return 0xff;
}
