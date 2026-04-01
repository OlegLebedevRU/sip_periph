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
#include "app_uart_dwin_tx.h"
#include "app_i2c_slave.h"
#include "dwin_gfx.h"
#include "hmi_console.h"
#include "hmi_diag_helper.h"
#include "service_time_sync.h"

extern osMessageQId myQueueHMIRecvRawHandle, myQueueToMasterHandle,
		myQueueHmiMsgHandle;
extern osTimerId myTimerHmiTimeoutHandle, myTimerHmiTtlHandle,myTimerReleBeforeHandle,myTimerBuzzerOffHandle;
static volatile uint32_t s_hmi_pending_events = 0U;

/* dwin_uart_start() declared in app_uart_dwin.h */
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
#define DWIN_INPUT_FIELD_LEN   (CARDNUMLENGTH * 2)  /* page0_input_widget VP buffer = 16 bytes */
#define DWIN_TIMER_VP_ADDR     0x5230U  /* page0_time_widget — time & diag */
#define DWIN_HDR_SIZE_LOCAL     3U
#define DWIN_MIN_KEY_PKT_SIZE   9U
#define DWIN_CMD_VP_READ        0x83U
#define HMI_TX_FRAME_OVERHEAD   6U
#define HMI_DIAG_TEXT_SIZE      26U
#define HMI_MSG_BUF_SIZE        (sizeof(msg_hmi.msg_buf))
#define HMI_MAX_MSG_PSIZE       (HMI_MSG_BUF_SIZE - 1U)
#define HMI_EVT_NONE            0x00000000UL
#define HMI_EVT_PIN_TIMEOUT     0x00000001UL
#define HMI_EVT_MSG_TTL         0x00000002UL
#define HMI_EVT_AUTH_PAGE_RET   0x00000004UL  /* return from auth page to page 0 */
#define HMI_RX_IDLE_WAIT_MS     50U
#define HMI_MSG_IDLE_WAIT_MS    100U
#define HMI_AUTH_PAGE_RETURN_MS 5000U  /* 5 seconds on auth page before return */
#define HMI_DIAG_LINE_COUNT_EXT (app_i2c_slave_diag_line_count() + 1U)  /* +1 for time line */

/* ---- Magic code detection state ---- */
#define MAGIC_SEQ_LEN     3U
static const uint8_t s_magic_seq[MAGIC_SEQ_LEN] = { '1', '0', '1' };  /* then '*' triggers */
static const uint8_t s_magic2_seq[MAGIC_SEQ_LEN] = { '1', '0', '2' }; /* then '*' triggers page 2 */
static uint8_t s_magic_pos = 0U;          /* how many of s_magic_seq matched so far */
static uint8_t s_magic2_pos = 0U;         /* how many of s_magic2_seq matched so far */
static volatile uint8_t s_diag_oneshot = 0U;   /* 1 = one full diag rotation requested */
static uint8_t s_diag_oneshot_remaining = 0U;  /* lines left in current oneshot cycle */

/* ---- Diag cancel: set by StartTaskHmi on '*' in idle, read by StartTaskHmiMsg ---- */
static volatile uint8_t s_diag_cancel = 0U;

/* ---- Auth page return (tick-based, polled in StartTaskHmiMsg) ---- */
static volatile uint32_t s_auth_page_return_tick = 0U;  /* 0 = inactive */

static uint8_t hmi_extract_input_code(const MsgUart_t *uart_msg, uint8_t *code);
static void resetpinbuf(void);
static void resetpindata(void);
static void dwin_input_output(const uint8_t *text, size_t textlen);

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
		dwin_input_output(&pin.rdata[0], sizeof(pin.rdata));
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
		/* Only clear auth display flag if auth page return is not pending;
		 * the page-return poll is the authoritative mechanism. */
		if (s_auth_page_return_tick == 0U) {
			s_auth_result_display_active = 0U;
		}
		dwin_input_output(&msg_hmi.msg_buf[0], sizeof(msg_hmi.msg_buf));
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

//----------------------
READER_t* get_pin_data() {
	return &pin;
}

void dwin_text_output(const uint16_t inaddr, const uint8_t *text_to_hmi,
		size_t elen) {
	size_t len = 0U;
	uint16_t addr = inaddr;
	uint8_t frame[DWIN_TX_BUF_SIZE];

	if (text_to_hmi == NULL) {
		return;
	}

	if (elen == 0U) {
		len = strlen((const char*) text_to_hmi);
	} else {
		len = elen;
	}
	if (len > (sizeof(frame) - HMI_TX_FRAME_OVERHEAD)) {
		len = sizeof(frame) - HMI_TX_FRAME_OVERHEAD;
	}

	{
		const uint8_t pbyte[] = { 0x5A, 0xA5, (uint8_t)(len + 3U), 0x82,
				(uint8_t) ((addr >> 8) & 0xFF), (uint8_t) (addr & 0xFF) };
		memcpy(frame, pbyte, sizeof(pbyte));
	}
	memcpy(frame + HMI_TX_FRAME_OVERHEAD, (const void*) text_to_hmi, len);
	dwin_tx_send(frame, (uint16_t)(len + HMI_TX_FRAME_OVERHEAD));
}

/* Fixed-width output to page0_input_widget — pads with 0xFF to prevent
 * DWIN from displaying leftover bytes in VP 0x5200 buffer. */
static void dwin_input_output(const uint8_t *text, size_t textlen)
{
	uint8_t buf[DWIN_INPUT_FIELD_LEN];
	memset(buf, 0xFF, DWIN_INPUT_FIELD_LEN);
	if (textlen > DWIN_INPUT_FIELD_LEN) {
		textlen = DWIN_INPUT_FIELD_LEN;
	}
	if (text != NULL && textlen > 0U) {
		memcpy(buf, text, textlen);
	}
	dwin_text_output(DWIN_TEXT_VP_ADDR, buf, DWIN_INPUT_FIELD_LEN);
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
	/* AUTH=1 (access granted): switch DWIN to page 1, schedule return to page 0.
	 * Other auth values: no HMI output (relay/buzzer handled by caller). */
	if (auth_result == 0x01U) {
		s_auth_result_display_active = 1U;
		osTimerStart(myTimerHmiTtlHandle, HMI_AUTH_PAGE_RETURN_MS);
		dwin_gfx_page_switch(1);
		s_auth_page_return_tick = HAL_GetTick() + HMI_AUTH_PAGE_RETURN_MS;
	}
}

void StartTaskHmi(void const *argument) {
	MsgUart_t uart_msg;
	(void)argument;
	dwin_tx_init();
	osDelay(100);
	/* Ensure DWIN starts on page 0 — it retains its page across STM32 resets,
	 * so if a previous session left it on page 2 (console), input would be dead. */
	dwin_gfx_page_switch(0);
	osDelay(50);
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
			/* '*' / 240: check for magic code before clearing */
			if (s_magic_pos >= MAGIC_SEQ_LEN) {
				/* Magic code 101* detected — enable one-shot diag rotation */
				s_diag_oneshot = 1U;
				s_diag_oneshot_remaining = HMI_DIAG_LINE_COUNT_EXT;
			} else if (s_magic2_pos >= MAGIC_SEQ_LEN) {
				/* Magic code 102* detected — set flag, page switch in StartTaskHmiMsg */
				hmi_console_activate(60U);
			} else if (pin.bitlength == 0U) {
				/* '*' in idle — cancel active diag rotation */
				s_diag_cancel = 1U;
			}
			resetpindata();
			s_magic_pos = 0U;
			s_magic2_pos = 0U;
		}
		else if (code == HMI_CODE_END_ASCII || code == HMI_CODE_END_LEGACY) {
			/* '#' / 241: submit PIN to ESP */
			s_magic_pos = 0U;
			s_magic2_pos = 0U;
			uint8_t saved_len = pin.bitlength;   /* save BEFORE reset */
			if (saved_len > 0U) {
				/* Snapshot pin into a stable buffer BEFORE resetpindata()
				 * clears it — StartTaskRxTxI2c1 will memcpy from this. */
				memcpy(&pin_snapshot, &pin, sizeof(READER_t));

				I2cPacketToMaster_t pckt;
				pckt.payload = (uint8_t*) &pin_snapshot;
				pckt.len     = saved_len + 2U;    /* rtype + bitlength + rdata */
				pckt.type    = PACKET_PIN_HMI;
				pckt.ttl     = service_time_sync_get_uptime_sec() + TTL_PACKET_SEC;
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

			/* Track magic sequence 1-0-1 */
			if (s_magic_pos < MAGIC_SEQ_LEN && code == s_magic_seq[s_magic_pos]) {
				s_magic_pos++;
			} else {
				s_magic_pos = (code == s_magic_seq[0]) ? 1U : 0U;
			}
			
			/* Track magic sequence 1-0-2 */
			if (s_magic2_pos < MAGIC_SEQ_LEN && code == s_magic2_seq[s_magic2_pos]) {
				s_magic2_pos++;
			} else {
				s_magic2_pos = (code == s_magic2_seq[0]) ? 1U : 0U;
			}

			/* Update display */
			dwin_input_output(&pin.rdata[0], pin.bitlength + 1U);
		}
		else {
			/* Buffer overflow — clear */
			s_magic_pos = 0U;
			s_magic2_pos = 0U;
			resetpindata();
		}

		dwin_buf_free(uart_msg.uart_buf);   /* пул вместо free() */
	}
}
/* Fixed-width output to page0_timer_widget — pads with spaces to prevent
 * garbage tail when shorter text follows longer text on same VP.
 * DWIN stores and displays the full VP buffer, so we must clear it all. */
#define DWIN_TIMER_FIELD_LEN  12U   /* page0_time_widget visible area = 12 chars */
static void dwin_timer_output(const char *text, size_t textlen)
{
	uint8_t buf[DWIN_TIMER_FIELD_LEN];
	if (textlen > DWIN_TIMER_FIELD_LEN) {
		textlen = DWIN_TIMER_FIELD_LEN;
	}
	memcpy(buf, text, textlen);
	if (textlen < DWIN_TIMER_FIELD_LEN) {
		memset(&buf[textlen], ' ', DWIN_TIMER_FIELD_LEN - textlen);
	}
	dwin_text_output(DWIN_TIMER_VP_ADDR, buf, DWIN_TIMER_FIELD_LEN);
}

/* Clear the entire timer VP with spaces */
static void dwin_timer_clear(void)
{
	uint8_t buf[DWIN_TIMER_FIELD_LEN];
	memset(buf, ' ', DWIN_TIMER_FIELD_LEN);
	dwin_text_output(DWIN_TIMER_VP_ADDR, buf, DWIN_TIMER_FIELD_LEN);
}

void StartTaskHmiMsg(void const *argument) {
	(void)argument;
	hmi_reset_msg_state();
	s_auth_result_display_active = 0U;
	resetpinbuf();

	uint8_t diag_index = 0U;
	const uint8_t diag_hw_count = app_i2c_slave_diag_line_count();
	char diag_buf[HMI_DIAG_TEXT_SIZE];
	uint8_t was_diag = 0U;           /* previous iteration was diag mode */
	uint32_t diag_err_snapshot = 0U; /* last known error sum — rotation starts on change */
	
	uint8_t was_console = 0U;
	uint8_t last_console_remain = 0U;
	static uint32_t diag_tick = 0U;
	static uint32_t time_tick = 0U;

	for (;;) {
		hmi_process_message_events();

		/* ---- Console page return and update ---- */
		hmi_console_poll(&was_console, &last_console_remain, &time_tick);

		/* ---- Auth page return: poll tick-based deadline ---- */
		if (s_auth_page_return_tick != 0U) {
			uint32_t now_ap = HAL_GetTick();
			if ((int32_t)(now_ap - s_auth_page_return_tick) >= 0) {
				s_auth_page_return_tick = 0U;
				s_auth_result_display_active = 0U;
				dwin_gfx_page_switch(0);
			}
		}

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
				dwin_input_output(&msg_hmi.msg_buf[0], msg_hmi.psize);
			}
		} else {
			uint32_t now = HAL_GetTick();

			/* Skip idle display if user is typing or auth page is showing */
			if (pin.bitlength > 0U || s_auth_result_display_active != 0U) {
				continue;
			}
			
			if (hmi_console_is_active()) { /* Skip diag/clock if console is active */
				continue;
			}

			/* ---- Handle diag cancel from '*' in idle ---- */
			if (s_diag_cancel != 0U) {
				s_diag_cancel = 0U;
				s_diag_oneshot = 0U;
				s_diag_oneshot_remaining = 0U;
				diag_index = 0U;
				/* Snapshot current sum so rotation doesn't restart immediately */
				diag_err_snapshot = hmi_diag_error_sum();
				/* Transition diag → clock: clear VP then force immediate clock update */
				dwin_timer_clear();
				was_diag = 0U;
				time_tick = 0U;  /* force clock redraw on next iteration */
				continue;
			}

			/* Determine if diag should be active:
			 *   - error counters changed since last snapshot
			 *   - oneshot was triggered by magic 101* */
			uint32_t err_sum = hmi_diag_error_sum();
			uint8_t want_diag = 0U;
			if (err_sum != diag_err_snapshot) {
				want_diag = 1U;
			}
			if (s_diag_oneshot != 0U && s_diag_oneshot_remaining > 0U) {
				want_diag = 1U;
			}

			if (want_diag) {
				/* ---- Transition clock → diag: clear VP first ---- */
				if (was_diag == 0U) {
					dwin_timer_clear();
					was_diag = 1U;
					diag_index = 0U;
					diag_tick = 0U;  /* force first line immediately */
				}

				/* ---- Diagnostics rotation (every 2s) ---- */
				if ((now - diag_tick) >= 2000U) {
					diag_tick = now;
					uint8_t total_lines = diag_hw_count + 1U; /* +1 for time line */

					hmi_diag_format_line_ext(diag_index, diag_buf, sizeof(diag_buf));
					/* Output diag to page0_timer_widget with fixed-width padding */
					dwin_timer_output(diag_buf, strlen(diag_buf));

					diag_index++;
					if (diag_index >= total_lines) {
						diag_index = 0U;
						/* Update snapshot — if no new errors, rotation stops next cycle */
						diag_err_snapshot = err_sum;
						/* If oneshot mode, complete the cycle */
						if (s_diag_oneshot != 0U) {
							s_diag_oneshot = 0U;
							s_diag_oneshot_remaining = 0U;
						}
					} else if (s_diag_oneshot != 0U && s_diag_oneshot_remaining > 0U) {
						s_diag_oneshot_remaining--;
					}
				}
			} else {
				/* ---- Transition diag → clock: clear VP first ---- */
				if (was_diag != 0U) {
					dwin_timer_clear();
					was_diag = 0U;
					diag_index = 0U;
					time_tick = 0U;  /* force clock redraw immediately */
				}

				/* No diag active — show time on timer widget (HH:MM:SS only) */
				if ((now - time_tick) >= 1000U) {
					time_tick = now;
					char time_hms[HMI_DIAG_TEXT_SIZE];
					hmi_diag_format_time_hms(time_hms, sizeof(time_hms));
					if (time_hms[0] != '\0') {
						dwin_timer_output(time_hms, strlen(time_hms));
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
