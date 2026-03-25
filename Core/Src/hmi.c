/*
 * hmi.c
 *
 *  Created on: 21 нояб. 2025 г.
 *      Author: oleg_
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "cmsis_os.h"
#include "app_uart_dwin.h"
#include "app_i2c_slave.h"

extern osMessageQId myQueueHMIRecvRawHandle, myQueueToMasterHandle,
		myQueueHmiMsgHandle;
extern osTimerId myTimerHmiTimeoutHandle, myTimerHmiTtlHandle,myTimerReleBeforeHandle,myTimerBuzzerOffHandle;
extern UART_HandleTypeDef huart2;
uint8_t txbuf[256];

/* dwin_uart_start() объявлена в app_uart_dwin.h */
static READER_t pin;
static READER_t pin_snapshot;   /* stable copy sent to I2C queue */
static MsgHmi_t msg_hmi = { };
static volatile uint8_t s_auth_result_display_active = 0U;
uint8_t input_interval_sec = HMI_INPUT_INTERVAL_SEC_DEFAULT;
uint8_t hmi_autodelete_sec = HMI_AUTODELETE_SEC_DEFAULT;

/* HMI Input Codes (DWIN packet body[5] = uart_buf[8]):
 * ASCII: 0x23='#' - end of PIN input, send to ESP
 * ASCII: 0x2A='*' - clear display and buffer
 * Legacy: 241     - end of PIN input (same as '#')
 * Legacy: 240     - clear display and buffer (same as '*')
 */
#define HMI_CODE_END_ASCII      0x23  /* '#' */
#define HMI_CODE_CLEAR_ASCII    0x2A  /* '*' */
#define HMI_CODE_END_LEGACY     241
#define HMI_CODE_CLEAR_LEGACY   240

extern void dwin_uart_start(void);
//----------------------
READER_t* get_pin_data() {
	return &pin;
}

void dwin_text_output(const uint16_t inaddr, const uint8_t *text_to_hmi,
		size_t elen) {
	size_t len = 0;
	uint16_t addr = inaddr;
	if (elen == 0)
		len = strlen((char*) text_to_hmi);
	else
		len = elen;
	uint8_t pbyte[] = { 0x5A, 0xA5, len + 3, 0x82,
			(uint8_t) ((addr >> 8) & 0xFF), (uint8_t) (addr & 0xFF) };
	MsgUart_t text_output = { .psize = len + 6, .uart_buf = &txbuf[0], };
	memcpy(text_output.uart_buf, pbyte, 6);
	memcpy(text_output.uart_buf + 6, (const void*) text_to_hmi, len);
	HAL_UART_Transmit_IT(&huart2, text_output.uart_buf, text_output.psize);
}
static void resetpinbuf() {
	/* Reset PIN buffer and state only — does NOT touch the display */
	pin.bitlength = 0x00;
	pin.rtype = _TOUCH_KEYPAD;
	memset(pin.rdata, 0xFF, sizeof(pin.rdata));
	osTimerStop(myTimerHmiTimeoutHandle);
}

static void resetpindata() {
	/* Full session reset: clear buffer AND display */
	resetpinbuf();
	if (msg_hmi.hmi_lock == UNLOCKED) {
		dwin_text_output(0x5200, &pin.rdata[0], sizeof(pin.rdata));
	}
}

void hmi_show_auth_result(uint8_t auth_result)
{
	char auth_buf[12];
	int written = snprintf(auth_buf, sizeof(auth_buf), "%u", (unsigned int)auth_result);
	if (written <= 0) {
		return;
	}
	if ((size_t)written >= sizeof(auth_buf)) {
		written = (int)(sizeof(auth_buf) - 1U);
		auth_buf[written] = '\0';
	}
	msg_hmi.hmi_lock = UNLOCKED;
	s_auth_result_display_active = 1U;
	osTimerStart(myTimerHmiTtlHandle, hmi_autodelete_sec * 1000U);
	dwin_text_output(0x5200, (const uint8_t *)auth_buf, (size_t)written);
}

void StartTaskHmi(void const *argument) {
	MsgUart_t uart_msg;
	osDelay(100);
	resetpindata();
	
	/* Kick off the two-phase DWIN receive FSM (lives in HAL_UART_RxCpltCallback) */
	dwin_uart_start();
	
	for (;;) {
		/* Block until a complete DWIN packet arrives */
		BaseType_t event = xQueueReceive(myQueueHMIRecvRawHandle, &uart_msg, osWaitForever);
		if (event != pdPASS) continue;

		/* ---- LOCKED state: only clear command passes through ---- */
		if (msg_hmi.hmi_lock == LOCKED) {
			if (uart_msg.uart_buf[0] == 0x5A && uart_msg.uart_buf[1] == 0xA5
					&& uart_msg.uart_buf[3] == 0x83) {
				uint8_t code = uart_msg.uart_buf[8];
				if (code == HMI_CODE_CLEAR_ASCII || code == HMI_CODE_CLEAR_LEGACY) {
					resetpindata();
				}
			}
			dwin_buf_free(uart_msg.uart_buf);   /* пул вместо free() */
			continue;
		}

		/* ---- UNLOCKED: full input processing ---- */
		if (uart_msg.uart_buf[0] == 0x5A && uart_msg.uart_buf[1] == 0xA5
				&& uart_msg.uart_buf[3] == 0x83) {
			uint8_t code = uart_msg.uart_buf[8];

			if (code == HMI_CODE_CLEAR_ASCII || code == HMI_CODE_CLEAR_LEGACY) {
				/* '*' / 240: clear buffer and display */
				resetpindata();
			}
			else if (code == HMI_CODE_END_ASCII || code == HMI_CODE_END_LEGACY) {
				/* '#' / 241: submit PIN to ESP */
				uint8_t saved_len = pin.bitlength;   /* save BEFORE reset */
				if (saved_len > 0) {
					/* Snapshot pin into a stable buffer BEFORE resetpindata()
					 * clears it — StartTaskRxTxI2c1 will memcpy from this. */
					memcpy(&pin_snapshot, &pin, sizeof(READER_t));

					I2cPacketToMaster_t pckt;
					pckt.payload = (uint8_t*) &pin_snapshot;
					pckt.len     = saved_len + 2;    /* rtype + bitlength + rdata */
					pckt.type    = PACKET_PIN_HMI;
					pckt.ttl     = uid_ttl;
					xQueueSend(myQueueToMasterHandle, &pckt, 0);

					/* Visual + session feedback */
					HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
					HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
					osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
					osTimerStart(myTimerHmiTimeoutHandle, input_interval_sec * 1000);
					osTimerStart(myTimerHmiTtlHandle,     hmi_autodelete_sec * 1000);

					resetpinbuf();   /* clear buffer only — display stays until timer or '*' */
				}
				/* if saved_len == 0: '#' on empty input — ignore */
			}
			else if (pin.bitlength < (PINUMLENGTH * 2 - 1)) {
				/* Regular digit — accumulate */
				pin.rdata[pin.bitlength] = code;
				pin.bitlength++;
				osTimerStart(myTimerHmiTimeoutHandle, hmi_autodelete_sec * 1000);

				/* Update display */
				dwin_text_output(0x5200, &pin.rdata[0], pin.bitlength + 1);
			}
			else {
				/* Buffer overflow — clear */
				resetpindata();
			}
		}

		dwin_buf_free(uart_msg.uart_buf);   /* пул вместо free() */
	}
}
void StartTaskHmiMsg(void const *argument) {
	memset(&msg_hmi, 0, sizeof(msg_hmi));
	memset(msg_hmi.msg_buf,0xFF,sizeof(msg_hmi.msg_buf));
	s_auth_result_display_active = 0U;

	uint8_t diag_index = 0U;
	const uint8_t diag_count = app_i2c_slave_diag_line_count();
	char diag_buf[12];

	for (;;) {
		if (msg_hmi.hmi_lock == LOCKED) {
			pin.bitlength = 0x00;
			pin.rtype = _TOUCH_KEYPAD;
			for (uint8_t i = 0; i < sizeof(pin.rdata); i++)
				pin.rdata[i] = 0xFF;
			osDelay(10);
			continue;
		}

		/* Try to receive an ESP/auth message with 2-sec timeout for diag rotation */
		BaseType_t event = xQueueReceive(myQueueHmiMsgHandle, &msg_hmi, 2000);

		if (event == pdPASS) {
			if (s_auth_result_display_active != 0U) {
				continue;
			}
			/* Got a real message from queue — display it */
			osTimerStart(myTimerHmiTtlHandle, msg_hmi.msg_ttl * 1000);
			if (msg_hmi.psize > 0 && msg_hmi.psize < 12) {
				dwin_text_output(0x5200, &msg_hmi.msg_buf[0], msg_hmi.psize + 1);
			}
		} else {
			/* Timeout — idle mode. If user is typing (pin buffer active), skip diag display */
			if (pin.bitlength > 0U) {
				continue;
			}
			if (s_auth_result_display_active != 0U) {
				continue;
			}
			/* Rotate diag error lines on HMI if any errors exist */
			if (app_i2c_slave_has_errors()) {
				app_i2c_slave_format_diag_line(diag_index, diag_buf, sizeof(diag_buf));
				dwin_text_output(0x5200, (const uint8_t *)diag_buf, strlen(diag_buf));
				diag_index++;
				if (diag_index >= diag_count) {
					diag_index = 0U;
				}
			}
		}
	}
}
void cb_Hmi_Pin_Timeout(void const *argument) {
	/* Session timeout: reset PIN buffer and turn off indicator */
	resetpindata();
	HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
}
void cb_Hmi_Ttl(void const *argument) {
	/* ESP/auth message session timeout: clear message display and buffer */
	memset(&msg_hmi, 0, sizeof(msg_hmi));
	memset(msg_hmi.msg_buf, 0xFF, sizeof(msg_hmi.msg_buf));
	s_auth_result_display_active = 0U;
	/* Clear display */
	dwin_text_output(0x5200, &msg_hmi.msg_buf[0], sizeof(msg_hmi.msg_buf));
}