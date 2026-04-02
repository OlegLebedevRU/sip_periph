/*
 * pn532_com.c
 *
 *  Created on: Apr 10, 2025
 *      Author: oleg_
 *
 *  Low-level I2C driver for PN532 NFC module.
 *  Cleaned: removed dead Linux-ported code, fixed I2C timeouts,
 *  replaced VLA with fixed buffer, added mutex-acquire check.
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "pn532_com.h"
#include "service_tca6408.h"

/* ---- HW binding -------------------------------------------------------- */
extern I2C_HandleTypeDef hi2c2;
extern osMutexId         i2c2_MutexHandle;

/* ---- PN532 frame constants (internal) ---------------------------------- */
#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4

/* Max PN532 data field + frame overhead: 255 + 9 = 264 */
#define PN532_MAX_FRAME_SIZE  265U

/* ---- I2C mutex helpers (return 0 on success, -1 on timeout) ------------ */
static inline int i2c2_lock(uint32_t ms) {
    return (osMutexWait(i2c2_MutexHandle, ms) == osOK) ? 0 : -1;
}

static inline void i2c2_unlock(void) {
    osMutexRelease(i2c2_MutexHandle);
}

/* ---- Public API -------------------------------------------------------- */

/* Write raw bytes to PN532 over I2C.
 * Returns  0 on success, -1 on error.  */
int pn532_write(uint8_t *data, size_t len) {
    if (i2c2_lock(100) != 0) return -1;

    HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(
        &hi2c2, PN532_I2C_ADDR, data, (uint16_t)len, PN532_I2C_TIMEOUT_MS);

    i2c2_unlock();
    service_tca6408_i2c2_recover_if_needed(st);
    return (st == HAL_OK) ? 0 : -1;
}

/* Read raw bytes from PN532 over I2C.
 * Returns number of bytes read (== len) on success, 0 on error. */
int pn532_read(uint8_t *buffer, size_t len) {
    if (i2c2_lock(100) != 0) return 0;

    HAL_StatusTypeDef st = HAL_I2C_Master_Receive(
        &hi2c2, PN532_I2C_ADDR, buffer, (uint16_t)len, PN532_I2C_TIMEOUT_MS);

    i2c2_unlock();
    service_tca6408_i2c2_recover_if_needed(st);
    return (st == HAL_OK) ? (int)len : 0;
}

/* Build a PN532 command frame and transmit it.
 * Returns  0 on success, -1 on error. */
int pn532_send_command(uint8_t cmd, uint8_t *data, size_t data_len) {
    if (data_len > 250U) return -1;            /* guard against overflow */

    uint8_t packet[PN532_MAX_FRAME_SIZE];
    size_t idx = 0;

    packet[idx++] = PN532_PREAMBLE;
    packet[idx++] = PN532_STARTCODE1;
    packet[idx++] = PN532_STARTCODE2;

    uint8_t len_byte = (uint8_t)(2U + data_len);
    packet[idx++] = len_byte;
    packet[idx++] = (uint8_t)(~len_byte + 1U);

    packet[idx++] = PN532_HOSTTOPN532;
    packet[idx++] = cmd;

    uint8_t checksum = PN532_HOSTTOPN532 + cmd;
    for (size_t i = 0; i < data_len; i++) {
        packet[idx++] = data[i];
        checksum += data[i];
    }

    packet[idx++] = (uint8_t)(~checksum + 1U);
    packet[idx++] = PN532_POSTAMBLE;

    return pn532_write(packet, idx);
}