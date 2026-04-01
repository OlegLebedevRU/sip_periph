/*
 * wiegand.c
 *
 *  Created on: 18 нояб. 2025 г.
 *      Author: oleg_
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "cmsis_os.h"
#include "service_time_sync.h"
extern osTimerId myTimerWiegand_pinHandle;
extern osTimerId myTimerWiegand_finHandle;
extern osTimerId myTimerWiegand_lockHandle;
extern osMessageQId myQueueWiegandHandle, myQueueToMasterHandle;
//static uint32_t w_currentMillis=0, w_previousMillis=0;
//static uint16_t w_bits_count=0;
GPIO_InitTypeDef GPIO_InitStructWiegand = { 0 };
__IO uint8_t w_debounce = 1;
//---------------------
uint8_t t2 = 1, nextdigit = 0;

static READER_t readerdata;
static READER_t inputdata;
//----------------------
READER_t* get_wiegand_data() {
	return &inputdata;
}

void resetreaderdata() {
	readerdata.bitlength = 0x00;
	readerdata.rtype = _ZEROREADER;
	for (uint8_t i = 0; i < sizeof(readerdata.rdata); i++)
		readerdata.rdata[i] = 0x00;
}
void resetinputdata() {
	inputdata.bitlength = 0x00;
	inputdata.rtype = _ZEROREADER;
	for (uint8_t i = 0; i < sizeof(inputdata.rdata); i++)
		inputdata.rdata[i] = 0x00;
}

uint8_t wiegand8todigit(uint8_t w8) {
	switch (w8) {
	case 0xE1:
		return 1;
	case 0xD2:
		return 2;
	case 0xC3:
		return 3;
	case 0xB4:
		return 4;
	case 0xA5:
		return 5;
	case 0x96:
		return 6;
	case 0x87:
		return 7;
	case 0x78:
		return 8;
	case 0x69:
		return 9;
	case 0xF0:
		return 0;
	case 0x5A:
		return 0x5A;
	case 0x4B:
		return 0x4B;
	}
	return w8;
}

void cb_WiegandFinTimer(void const *argument) {
	if (t2 == 0)
		return;
	if (readerdata.bitlength < 8) { //error on wiegand line  - short bitlength
		t2 = 1;
		nextdigit = 0;
		resetreaderdata();
		resetinputdata();
		HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
		return;
	} else if (readerdata.bitlength == 8) {
		if (nextdigit < 6) {
			inputdata.rdata[nextdigit] = wiegand8todigit(readerdata.rdata[0]);
			if (inputdata.rdata[nextdigit] == WIEGAND8_ESQ_CODE) {
				//	resetinputdata();
				//	resetreaderdata();
				nextdigit = 0;
				HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
				return;
			}
			inputdata.rtype = _WIEGAND_COLLECT_DIGITS;
			osTimerStart(myTimerWiegand_pinHandle, 1000 * 3);
			nextdigit++;
			inputdata.bitlength = nextdigit;
			//resetreaderdata();
		}
		if (nextdigit == 6) {
			HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
			t2 = 0;
			//osTimerStop(myTimer03Handle);
			osTimerStart(myTimerWiegand_lockHandle, 5000);
//					for (uint8_t c = 0; c < CHANNELS; c++) {
//						if ((memcmp(inputdata.rdata, pinslist[c].cardcode,
//						CARDNUMLENGTH) == 0)
//								&& (pinslist[c].cardlength == PINUMLENGTH)) {
//							osMessageQueuePut(olQueuechimpulsHandle,
//									&pinslist[c].ch, 0U, 0U);
//						}
//					}
		}
		return;
	} // one digit
	else if ((readerdata.bitlength == 26) || (readerdata.bitlength == 34)
			|| (readerdata.bitlength == 50)) {
		HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
		t2 = 0;
		readerdata.rdata[readerdata.bitlength / 8] <<= 6;
		for (uint8_t i = 0; i < CARDNUMLENGTH; i++)
			inputdata.rdata[i] = readerdata.rdata[i];
		readerdata.rtype = _WIEGAND;
		inputdata.rtype = _WIEGAND;
		if(readerdata.bitlength==26)inputdata.bitlength =3;
		if(readerdata.bitlength==34)inputdata.bitlength =4;
		if(readerdata.bitlength==50)inputdata.bitlength =6;
		//inputdata.bitlength = readerdata.bitlength;
		osTimerStart(myTimerWiegand_lockHandle, 10000);
		for (uint8_t i = 0; i < inputdata.bitlength; i++) {
			inputdata.rdata[i] = inputdata.rdata[i] << 1;
			if (inputdata.rdata[i + 1] & 0x80)
				inputdata.rdata[i]++;
		}
		inputdata.rdata[inputdata.bitlength] = 0;
		I2cPacketToMaster_t pckt;

		pckt.payload = (uint8_t *)get_wiegand_data();
		pckt.len = inputdata.bitlength+2; //slaveTxData[13]+1;
		pckt.type = PACKET_WIEGAND;
		pckt.ttl = service_time_sync_get_uptime_sec() + TTL_PACKET_SEC;
		xQueueSendToFront(myQueueToMasterHandle, &pckt, 0);
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
		return;
	} else {
		t2 = 1;
		nextdigit = 0;
		resetreaderdata();
		resetinputdata();
		HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
		return;
	}

}

void cb_WiegandPinTimer(void const *argument) {
	t2 = 1;
	nextdigit = 0;
	resetreaderdata();
	resetinputdata();
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void cb_WiegandLock(void const *argument) {
	t2 = 1;
	nextdigit = 0;
	resetreaderdata();
	resetinputdata();
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
	HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
}

#define READERLASTBITWAITMS 150
void StartTaskWiegand(void const *argument) {
	BaseType_t event;
	uint8_t wmsg;
	uint8_t bytenum = 0;
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

	for (;;) {
		event = xQueueReceive(myQueueWiegandHandle, &wmsg,
		osWaitForever);
		bytenum = readerdata.bitlength / 8;
		readerdata.bitlength += 1;
		if (event == pdPASS) {
			readerdata.rdata[bytenum] <<= 1;
			if (wmsg == 1)
				readerdata.rdata[bytenum] += 1;
		}
		osTimerStart(myTimerWiegand_finHandle, READERLASTBITWAITMS); // start waiting wiegand packet - packet will be ready with callback01
		//	osThreadYield();
	}

}
