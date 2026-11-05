/*
 *
 * FocalTech fts TouchScreen driver.
 *
 * Copyright (c) 2012-2021, Focaltech Ltd. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*****************************************************************************
*
* File Name: focaltech_upgrade_ft5260.c
*
* Author: Focaltech Driver Team
*
* Created: 2021-09-29
*
* Abstract:
*
* Reference:
*
*****************************************************************************/
/*****************************************************************************
* 1.Included header files
*****************************************************************************/
#include "../focaltech_flash.h"

#define FTS_DELAY_ERASE_PAGE_2K         80
#define FTS_SIZE_PAGE_2K                2048


static int ft5260_ecc_cal(u32 saddr, u32 len)
{
    int ret = 0;
    int ecc = 0;
    int i = 0;
    u8 wbuf[FTS_CMD_ECC_CAL_LEN] = { 0 };
    u8 val[FTS_CMD_FLASH_STATUS_LEN] = { 0 };
    u16 read_status = 0;

    FTS_INFO( "**********read out checksum**********");

    /* check sum init */
    wbuf[0] = FTS_CMD_ECC_INIT;
    ret = fts_write(wbuf, 1);
    if (ret < 0) {
        FTS_ERROR("ecc init cmd write fail");
        return ret;
    }

    wbuf[0] = FTS_CMD_ECC_CAL;
    wbuf[1] = BYTE_OFF_16(saddr);
    wbuf[2] = BYTE_OFF_8(saddr);
    wbuf[3] = BYTE_OFF_0(saddr);
    wbuf[4] = BYTE_OFF_16(len);
    wbuf[5] = BYTE_OFF_8(len);
    wbuf[6] = BYTE_OFF_0(len);
    ret = fts_write(wbuf, FTS_CMD_ECC_CAL_LEN);
    if (ret < 0) {
        FTS_ERROR("ecc calc cmd write fail");
        return ret;
    }

    fts_msleep(len / 256);

    /* read status */
    for (i = 0; i < FTS_RETRIES_ECC_CAL; i++) {
        wbuf[0] = FTS_CMD_FLASH_STATUS;
        ret = fts_read(&wbuf[0] , 1, val, FTS_CMD_FLASH_STATUS_LEN);
        read_status = (((u16)val[0]) << 8) + val[1];
        /*  FTS_INFO("%x %x", FTS_CMD_FLASH_STATUS_ECC_OK, read_status); */
        if (FTS_CMD_FLASH_STATUS_ECC_OK == read_status) {
            break;
        }
        fts_msleep(FTS_RETRIES_DELAY_ECC_CAL);
    }

    if (i >= FTS_RETRIES_ECC_CAL) {
        FTS_ERROR("ecc flash status read fail");
        return -EIO;
    }

    /* read out check sum */
    wbuf[0] = FTS_CMD_ECC_READ;
    ret = fts_read(wbuf, 1, val, 2);
    if (ret < 0) {
        FTS_ERROR( "ecc read cmd write fail");
        return ret;
    }

    ecc = (int)((u16)(val[0] << 8) + val[1]);
    return ecc;
}

/************************************************************************
* Name: fts_ft5260_upgrade
* Brief:
* Input:
* Output:
* Return: return 0 if success, otherwise return error code
***********************************************************************/
static int fts_ft5260_upgrade(u8 *buf, u32 len)
{
    int ret = 0;
    u32 start_addr = 0;
    u8 cmd[5] = { 0 };
    u32 delay = 0;
    int ecc_in_host = 0;
    int ecc_in_tp = 0;

    if ((NULL == buf) || (len < FTS_MIN_LEN)) {
        FTS_ERROR("buffer/len(%x) is invalid", len);
        return -EINVAL;
    }

    /* enter into upgrade environment */
    ret = fts_fwupg_enter_into_boot();
    if (ret < 0) {
        FTS_ERROR("enter into pramboot/bootloader fail,ret=%d", ret);
        goto fw_reset;
    }
    cmd[0] = 0x02;
    cmd[1] = 0x55;
    cmd[2] = 0xAA;
    cmd[3] = 0x5A;
    cmd[4] = 0xA5;
    ret = fts_write(cmd, 5);
    if (ret < 0) {
        FTS_ERROR("write 02 55 AA 5A A5 fail");
        goto fw_reset;
    }

    cmd[0] = FTS_CMD_APP_DATA_LEN_INCELL;
    cmd[1] = BYTE_OFF_16(len);
    cmd[2] = BYTE_OFF_8(len);
    cmd[3] = BYTE_OFF_0(len);
    ret = fts_write(cmd, FTS_CMD_DATA_LEN_LEN);
    if (ret < 0) {
        FTS_ERROR("data len cmd write fail");
        goto fw_reset;
    }

    cmd[0] = FTS_CMD_FLASH_MODE;
    cmd[1] = FLASH_MODE_UPGRADE_VALUE;
    ret = fts_write(cmd, 2);
    if (ret < 0) {
        FTS_ERROR("upgrade mode(09) cmd write fail");
        goto fw_reset;
    }

    delay = FTS_DELAY_ERASE_PAGE_2K * (len / FTS_SIZE_PAGE_2K);
    ret = fts_fwupg_erase(delay);
    if (ret < 0) {
        FTS_ERROR("erase cmd write fail");
        goto fw_reset;
    }

    /* write app */
    start_addr = upgrade_func_ft5260.appoff;
    ecc_in_host = fts_flash_write_buf(start_addr, buf, len, 1);
    if (ecc_in_host < 0 ) {
        FTS_ERROR("flash write fail");
        goto fw_reset;
    }

    /* ecc */
    ecc_in_tp = ft5260_ecc_cal(start_addr, len);
    if (ecc_in_tp < 0 ) {
        FTS_ERROR("ecc read fail");
        goto fw_reset;
    }

    FTS_INFO("ecc in tp:%x, host:%x", ecc_in_tp, ecc_in_host);
    if (ecc_in_tp != ecc_in_host) {
        FTS_ERROR("ecc check fail");
        goto fw_reset;
    }

    FTS_INFO("upgrade success, reset to normal boot");
    ret = fts_fwupg_reset_in_boot();
    if (ret < 0) {
        FTS_ERROR("reset to normal boot fail");
    }

    fts_msleep(200);
    return 0;

fw_reset:
    FTS_INFO("upgrade fail, reset to normal boot");
    ret = fts_fwupg_reset_in_boot();
    if (ret < 0) {
        FTS_ERROR("reset to normal boot fail");
    }
    return -EIO;
}

struct upgrade_func upgrade_func_ft5260 = {
    .ctype = {0x8B},
    .fwveroff = 0x010E,
    .fwcfgoff = 0x0FB0,
    .appoff = 0x0000,
    .fw_ecc_check_mode = ECC_CHECK_MODE_CRC16,
    .pramboot_supported = false,
    .hid_supported = false,
    .upgrade = fts_ft5260_upgrade,
};
