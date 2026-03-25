/*
 * pn532_com.h
 *
 *  Created on: Apr 10, 2025
 *      Author: oleg_
 *
 *  Low-level I2C driver for PN532 NFC module (STM32/FreeRTOS).
 *  Only the commands actually used by the application are exposed.
 */

#ifndef INC_PN532_COM_H_
#define INC_PN532_COM_H_

#include <stdint.h>
#include <stddef.h>

/* ---- PN532 command codes (only those used by the application) ----------- */
enum Command
{
    GetGeneralStatus    = 0x04,
    SAMConfiguration    = 0x14,
    InListPassiveTarget = 0x4A,
};

/* ---- Named I2C / frame constants --------------------------------------- */
#define PN532_I2C_ADDR          0x48U   /* 0x24 << 1 (7-bit addr in HAL format) */
#define PN532_I2C_TIMEOUT_MS    50U     /* PN532 clock-stretches; 1-2 ms is too low */
#define PN532_READY_BYTE        0x01U

#define PN532_ACK_READ_LEN      7U
#define PN532_RESP_READ_LEN     15U
#define PN532_DATA_READ_LEN     32U

/* ---- Public API -------------------------------------------------------- */

int pn532_write(uint8_t *data, size_t len);
int pn532_read(uint8_t *buffer, size_t len);
int pn532_send_command(uint8_t cmd, uint8_t *data, size_t data_len);

#endif /* INC_PN532_COM_H_ */
