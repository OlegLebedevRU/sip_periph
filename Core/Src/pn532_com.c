/*
 * pn532_com.c
 *
 *  Created on: Apr 10, 2025
 *      Author: oleg_
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "cmsis_os.h"
#include "pn532_com.h"
#define I2C_ADDRESS 0x48   // Example I2C address for the PN532 RFID module
extern I2C_HandleTypeDef hi2c2;
typedef struct {
    uint8_t preamble;
    uint8_t start_code1;
    uint8_t start_code2;
    uint8_t length;
    uint8_t length_checksum;
    uint8_t frame_data[256];
    uint8_t data_checksum;
    uint8_t postamble;
} pn532_frame_t;


#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00

#define PN532_HOSTTOPN532   0xD4
#define PN532_TFI_RECEIVE   0xD5
#define PN532_ACK_FRAME    {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00}


int pn532_is_pn532killer(pn532_t *pn532)
{
    int ret;

    if ((ret = pn532_send_command(pn532, checkPn532Killer, NULL, 0))>0) return ret;
    if ((ret = pn532_wait_response(pn532, checkPn532Killer))>0) return ret;

    if (ret == 0) return 1;
    return 0;
}

int pn532_set_normal_mode(pn532_t *pn532)
{
    int ret;
    uint8_t cmd[14] = {0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
//uint8_t cmdwup[]={0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD4, 0x14, 0x01, 0x17, 0x00};
    if ((ret = pn532_write(pn532, cmd, sizeof(cmd)))>0) return ret;
    osDelay(10);
   // pn532_read(pn532, cmd, 7);
    osDelay(100);
    cmd[0] = 0x01;
    if ((ret = pn532_send_command(pn532, SAMConfiguration, cmd, 1))>0) return ret;
    osDelay(10);
    if (pn532_read(pn532, cmd, 7) < 1) return -1;

    return 0;
}

/* Function to write data to PN532 */
int pn532_write(pn532_t *pn532, uint8_t *data, size_t len) {
   // ssize_t written = write(pn532->fd, data, len);
	uint32_t tm = len/10+1;
	lock_i2c2(100);
    if (HAL_I2C_Master_Transmit(&hi2c2, I2C_ADDRESS, data, len,tm)!=HAL_OK) {
     //   perror("Write error");
    	unlock_i2c2();
        return -1;
    }
    unlock_i2c2();
    return 0;
}

/* Function to read data from PN532 */
int pn532_read(pn532_t *pn532, uint8_t *buffer, size_t len) {
 //   ssize_t read_bytes = 0;
uint32_t tm = len/10+1;
lock_i2c2(100);
    if(HAL_I2C_Master_Receive(&hi2c2, I2C_ADDRESS,buffer, len,tm) !=HAL_OK){
    	unlock_i2c2();
    	return 0;
    }
    unlock_i2c2();
    return (int)len;
}

/* Function to send command and get response */
int pn532_send_command(pn532_t *pn532, uint8_t cmd, uint8_t *data, size_t data_len) {
    uint8_t packet[data_len + 10];
    uint8_t checksum, len_byte;
    size_t idx = 0, i;

    packet[idx++] = PN532_PREAMBLE;
    packet[idx++] = PN532_STARTCODE1;
    packet[idx++] = PN532_STARTCODE2;

    len_byte = 2 + data_len;
    packet[idx++] = len_byte;
    packet[idx++] = ~len_byte + 1;

    packet[idx++] = PN532_HOSTTOPN532;
    packet[idx++] = cmd;

    checksum = PN532_HOSTTOPN532 + cmd;
    for (i = 0; i < data_len; i++) {
        packet[idx++] = data[i];
        checksum += data[i];
    }

    packet[idx++] = ~checksum + 1;
    packet[idx++] = PN532_POSTAMBLE;

    if (pn532_write(pn532, packet, idx) < 0) return -1;

    return 0;
}

// response should be uint8_t buffer[260];
//  0 Success
// -1 Read error
// -2
// -3 Length checksum error
// -4 Data checksum error
// -5 Postamble error
// -6 Data frame TFI error
// -7 Data frame length error
int pn532_read_response(pn532_t *pn532, pn532_result_t *response)
{
    int i, ret = 0;
    uint8_t data_checksum;
    pn532_frame_t frame;

    if (!response) response = &pn532->result;

    do {
        if (pn532_read(pn532, &frame.preamble, 1) != 1)
            return -1;
    }
    while (frame.preamble != PN532_PREAMBLE);

    if (pn532_read(pn532, &frame.start_code1, 3) != 3)
        return -1;

    if (pn532_read(pn532, &frame.length_checksum, frame.length+1) != frame.length+1)
        return -1;

    if (frame.length && ((frame.length + frame.length_checksum) & 0xFF)  != 0) {
        return -3;    // Length checksum error
    }

    if (frame.length)
    {
        for (i=0, data_checksum = 0; i < frame.length; i++) {
            data_checksum += frame.frame_data[i];
        }

        if (pn532_read(pn532, &frame.data_checksum, 1) != 1)
            return -1;

        if (((frame.data_checksum + data_checksum) & 0xFF) != 0)
            return -4; // Data checksum error
    }

    if (pn532_read(pn532, &frame.postamble, 1) != 1)
        return -1;

    if (frame.postamble != 0)
        return -5; // Postamble error

    if (ret || frame.length == 0) return ret;

    if (frame.frame_data[0] != PN532_TFI_RECEIVE)
        return -6;

    if (frame.length < 2) return -7;

    response->cmd = frame.frame_data[1] - 1;
    response->status = PN_SUCCESS;
    response->len = frame.length-2;
    memcpy(response->data, frame.frame_data+2, response->len);

    if ((response->cmd == InCommunicateThru || response->cmd == InDataExchange) && frame.length > 2) {
        response->status = frame.frame_data[2];
        if (frame.frame_data[2] == 0 && frame.length > 16)
            memcpy(response->data, frame.frame_data+3, --response->len);
    }

    return 0;
}

int pn532_wait_response(pn532_t *pn532, uint8_t cmd)
{
    int ret;

    pn532->result.cmd = 0;
    while ((ret = pn532_read_response(pn532, NULL)) == 0) {
        if (pn532->result.cmd == cmd) break;
    }

    return ret;
}

char *pn532_strerror(int ret)
{
    switch (ret)
    {
    case  0: return "Success";
    case -1: return "Read/Write error";
    case -3: return "Length checksum error";
    case -4: return "Data checksum error";
    case -5: return "Postamble error";
    case -6: return "Data frame TFI error";
    case -7: return "Data frame length error";
    default: return "Unknown error";
    }
}
