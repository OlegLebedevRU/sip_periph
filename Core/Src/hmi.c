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
static uint8_t s_hmi_txbuf[256];
static osMutexId s_hmi_tx_mutex = NULL;
static volatile uint32_t s_hmi_pending_events = 0U;

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
#define DWIN_TEXT_VP_ADDR       0x5200U
#define DWIN_HDR_SIZE_LOCAL     3U
#define DWIN_MIN_KEY_PKT_SIZE   9U
#define DWIN_CMD_VP_READ        0x83U
#define HMI_TX_WAIT_RETRIES     20U
#define HMI_TX_WAIT_DELAY_MS    10U
#define HMI_TX_FRAME_OVERHEAD   6U
#define HMI_DIAG_TEXT_SIZE      12U
#define HMI_MSG_BUF_SIZE        (sizeof(msg_hmi.msg_buf))
#define HMI_MAX_MSG_PSIZE       (HMI_MSG_BUF_SIZE - 1U)
#define HMI_EVT_NONE            0x00000000UL
#define HMI_EVT_PIN_TIMEOUT     0x00000001UL
#define HMI_EVT_MSG_TTL         0x00000002UL
#define HMI_RX_IDLE_WAIT_MS     50U
#define HMI_MSG_IDLE_WAIT_MS    100U

extern void dwin_uart_start(void);

static uint8_t hmi_extract_input_code(const MsgUart_t *uart_msg, uint8_t *code);
static void resetpinbuf(void);
static void resetpindata(void);

static void hmi_raise_event(uint32_t event_mask)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	s_hmi_pending_events |= event_mask;
	if (primask == 0U) {
		__enable_irq();
	}
}

static uint32_t hmi_take_events(uint32_t event_mask)
{
	uint32_t pending;
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	pending = s_hmi_pending_events & event_mask;
	s_hmi_pending_events &= ~event_mask;
	if (primask == 0U) {
		__enable_irq();
	}
	return pending;
}

static void hmi_reset_pin_state(void)
{
	pin.bitlength = 0x00;
	pin.rtype = _TOUCH_KEYPAD;
	memset(pin.rdata, 0xFF, sizeof(pin.rdata));
}

static void hmi_reset_msg_state(void)
{
	memset(&msg_hmi, 0, sizeof(msg_hmi));
	memset(msg_hmi.msg_buf, 0xFF, sizeof(msg_hmi.msg_buf));
}

static void hmi_clear_display_if_unlocked(void)
{
	if (msg_hmi.hmi_lock == UNLOCKED) {
		dwin_text_output(DWIN_TEXT_VP_ADDR, &pin.rdata[0], sizeof(pin.rdata));
	}
}

static void hmi_start_input_timeout(uint32_t timeout_ms)
{
	osTimerStart(myTimerHmiTimeoutHandle, timeout_ms);
}

static uint8_t hmi_extract_input_code(const MsgUart_t *uart_msg, uint8_t *code)
{
	if (uart_msg == NULL || code == NULL || uart_msg->uart_buf == NULL) {
		return 0U;
	}
	if (uart_msg->psize < DWIN_MIN_KEY_PKT_SIZE) {
		return 0U;
	}
	if (uart_msg->uart_buf[0] != 0x5AU || uart_msg->uart_buf[1] != 0xA5U) {
		return 0U;
	}
	if (uart_msg->uart_buf[2] < (DWIN_MIN_KEY_PKT_SIZE - DWIN_HDR_SIZE_LOCAL)) {
		return 0U;
	}
	if (uart_msg->uart_buf[3] != DWIN_CMD_VP_READ) {
		return 0U;
	}
	*code = uart_msg->uart_buf[8];
	return 1U;
}

static void hmi_process_input_events(void)
{
	if ((hmi_take_events(HMI_EVT_PIN_TIMEOUT) & HMI_EVT_PIN_TIMEOUT) != 0U) {
		resetpindata();
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
	}
}

static void hmi_process_message_events(void)
{
	if ((hmi_take_events(HMI_EVT_MSG_TTL) & HMI_EVT_MSG_TTL) != 0U) {
		hmi_reset_msg_state();
		s_auth_result_display_active = 0U;
		dwin_text_output(DWIN_TEXT_VP_ADDR, &msg_hmi.msg_buf[0], sizeof(msg_hmi.msg_buf));
	}
}

static uint32_t hmi_wait_for_input_packet(MsgUart_t *uart_msg)
{
	return (uint32_t)xQueueReceive(myQueueHMIRecvRawHandle, uart_msg, HMI_RX_IDLE_WAIT_MS);
}

static uint32_t hmi_wait_for_host_message(void)
{
	return (uint32_t)xQueueReceive(myQueueHmiMsgHandle, &msg_hmi, HMI_MSG_IDLE_WAIT_MS);
}

static void hmi_ensure_tx_mutex(void)
{
	if (s_hmi_tx_mutex == NULL) {
		osMutexDef(hmiTxMutex);
		s_hmi_tx_mutex = osMutexCreate(osMutex(hmiTxMutex));
	}
}

//----------------------
READER_t* get_pin_data() {
	return &pin;
}

void dwin_text_output(const uint16_t inaddr, const uint8_t *text_to_hmi,
		size_t elen) {
	size_t len = 0U;
	uint16_t addr = inaddr;
	HAL_StatusTypeDef tx_status;

	if (text_to_hmi == NULL) {
		return;
	}

	hmi_ensure_tx_mutex();
	if (s_hmi_tx_mutex == NULL) {
		return;
	}
	if (osMutexWait(s_hmi_tx_mutex, osWaitForever) != osOK) {
		return;
	}

	if (elen == 0U) {
		len = strlen((const char*) text_to_hmi);
	} else {
		len = elen;
	}
	if (len > (sizeof(s_hmi_txbuf) - HMI_TX_FRAME_OVERHEAD)) {
		len = sizeof(s_hmi_txbuf) - HMI_TX_FRAME_OVERHEAD;
	}

	for (uint8_t r = 0U; r < HMI_TX_WAIT_RETRIES; r++) {
		if (huart2.gState == HAL_UART_STATE_READY) {
			break;
		}
		osDelay(HMI_TX_WAIT_DELAY_MS);
	}

	{
		const uint8_t pbyte[] = { 0x5A, 0xA5, (uint8_t)(len + 3U), 0x82,
				(uint8_t) ((addr >> 8) & 0xFF), (uint8_t) (addr & 0xFF) };
		memcpy(s_hmi_txbuf, pbyte, sizeof(pbyte));
	}
	memcpy(s_hmi_txbuf + HMI_TX_FRAME_OVERHEAD, (const void*) text_to_hmi, len);
	tx_status = HAL_UART_Transmit_IT(&huart2, s_hmi_txbuf, len + HMI_TX_FRAME_OVERHEAD);
	(void)tx_status;
	osMutexRelease(s_hmi_tx_mutex);
}

static void resetpinbuf(void) {
	/* Reset PIN buffer and state only — does NOT touch the display */
	hmi_reset_pin_state();
	osTimerStop(myTimerHmiTimeoutHandle);
}

static void resetpindata(void) {
	/* Full session reset: clear buffer AND display */
	resetpinbuf();
	hmi_clear_display_if_unlocked();
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
	dwin_text_output(DWIN_TEXT_VP_ADDR, (const uint8_t *)auth_buf, (size_t)written);
}

void StartTaskHmi(void const *argument) {
	MsgUart_t uart_msg;
	(void)argument;
	hmi_ensure_tx_mutex();
	osDelay(100);
	resetpindata();
	
	/* Kick off the two-phase DWIN receive FSM (lives in HAL_UART_RxCpltCallback) */
	dwin_uart_start();

	for (;;) {
		uint8_t code = 0U;
		hmi_process_input_events();
		/* Poll with short timeout so timer-originated UI work executes in task context */
		BaseType_t event = hmi_wait_for_input_packet(&uart_msg);
		if (event != pdPASS) {
			continue;
		}

		if (hmi_extract_input_code(&uart_msg, &code) == 0U) {
			dwin_buf_free(uart_msg.uart_buf);
			continue;
		}

		/* ---- LOCKED state: only clear command passes through ---- */
		if (msg_hmi.hmi_lock == LOCKED) {
			if (code == HMI_CODE_CLEAR_ASCII || code == HMI_CODE_CLEAR_LEGACY) {
				resetpindata();
			}
			dwin_buf_free(uart_msg.uart_buf);   /* пул вместо free() */
			continue;
		}

		/* ---- UNLOCKED: full input processing ---- */
		if (code == HMI_CODE_CLEAR_ASCII || code == HMI_CODE_CLEAR_LEGACY) {
			/* '*' / 240: clear buffer and display */
			resetpindata();
		}
		else if (code == HMI_CODE_END_ASCII || code == HMI_CODE_END_LEGACY) {
			/* '#' / 241: submit PIN to ESP */
			uint8_t saved_len = pin.bitlength;   /* save BEFORE reset */
			if (saved_len > 0U) {
				/* Snapshot pin into a stable buffer BEFORE resetpindata()
				 * clears it — StartTaskRxTxI2c1 will memcpy from this. */
				memcpy(&pin_snapshot, &pin, sizeof(READER_t));

				I2cPacketToMaster_t pckt;
				pckt.payload = (uint8_t*) &pin_snapshot;
				pckt.len     = saved_len + 2U;    /* rtype + bitlength + rdata */
				pckt.type    = PACKET_PIN_HMI;
				pckt.ttl     = uid_ttl;
				xQueueSend(myQueueToMasterHandle, &pckt, 0);

				/* Visual + session feedback */
				HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
				HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
				osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
				hmi_start_input_timeout((uint32_t)input_interval_sec * 1000U);
				osTimerStart(myTimerHmiTtlHandle, (uint32_t)hmi_autodelete_sec * 1000U);

				resetpinbuf();   /* clear buffer only — display stays until timer or '*' */
			}
			/* if saved_len == 0: '#' on empty input — ignore */
		}
		else if (pin.bitlength < (PINUMLENGTH * 2 - 1)) {
			/* Regular digit — accumulate */
			pin.rdata[pin.bitlength] = code;
			pin.bitlength++;
			hmi_start_input_timeout((uint32_t)input_interval_sec * 1000U);

			/* Update display */
			dwin_text_output(DWIN_TEXT_VP_ADDR, &pin.rdata[0], pin.bitlength + 1U);
		}
		else {
			/* Buffer overflow — clear */
			resetpindata();
		}

		dwin_buf_free(uart_msg.uart_buf);   /* пул вместо free() */
	}
}
void StartTaskHmiMsg(void const *argument) {
	(void)argument;
	hmi_reset_msg_state();
	s_auth_result_display_active = 0U;
	resetpinbuf();

	uint8_t diag_index = 0U;
	const uint8_t diag_count = app_i2c_slave_diag_line_count();
	char diag_buf[HMI_DIAG_TEXT_SIZE];

	for (;;) {
		hmi_process_message_events();
		if (msg_hmi.hmi_lock == LOCKED) {
			resetpinbuf();
			osDelay(10);
			continue;
		}

		/* Try to receive an ESP/auth message with short timeout for TTL/diag rotation */
		BaseType_t event = hmi_wait_for_host_message();

		if (event == pdPASS) {
			if (s_auth_result_display_active != 0U) {
				continue;
			}
			/* Got a real message from queue — display it */
			osTimerStart(myTimerHmiTtlHandle, (uint32_t)msg_hmi.msg_ttl * 1000U);
			if (msg_hmi.psize > 0U && msg_hmi.psize <= HMI_MAX_MSG_PSIZE) {
				dwin_text_output(DWIN_TEXT_VP_ADDR, &msg_hmi.msg_buf[0], msg_hmi.psize);
			}
		} else {
			static uint32_t diag_tick = 0U;
			uint32_t now = HAL_GetTick();
			if ((now - diag_tick) < 2000U) {
				continue;
			}
			diag_tick = now;
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
				dwin_text_output(DWIN_TEXT_VP_ADDR, (const uint8_t *)diag_buf, strlen(diag_buf));
				if (diag_count > 0U) {
					diag_index++;
					if (diag_index >= diag_count) {
						diag_index = 0U;
					}
				}
			}
		}
	}
}
void cb_Hmi_Pin_Timeout(void const *argument) {
	(void)argument;
	/* Defer session timeout handling to StartTaskHmi task context */
	hmi_raise_event(HMI_EVT_PIN_TIMEOUT);
}
void cb_Hmi_Ttl(void const *argument) {
	(void)argument;
	/* Defer HMI TTL clear handling to StartTaskHmiMsg task context */
	hmi_raise_event(HMI_EVT_MSG_TTL);
}