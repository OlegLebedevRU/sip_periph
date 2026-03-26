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
#define DWIN_CONSOLE_VP_ADDR   0x6000U
#define DWIN_CONSOLE_LINES     15U
#define DWIN_CONSOLE_TEXT_LEN  32U                  /* visible chars per row           */
#define DWIN_CONSOLE_MAX_TASKS 16U                  /* max tasks queried for watermark */
#define DWIN_CONSOLE_EOL_LEN    2U                  /* \r\n                            */
#define DWIN_CONSOLE_ROW_LEN   (DWIN_CONSOLE_TEXT_LEN + DWIN_CONSOLE_EOL_LEN)  /* 34  */
#define DWIN_CONSOLE_BUF_LEN   (DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 2U) /* +2 for 0xFFFF terminator */
/* Max payload per dwin_text_output call: DWIN_TX_BUF_SIZE(256) - 6 = 250 bytes.
 * We send 7 rows per chunk = 7*34 = 238 bytes < 250. */
#define DWIN_CONSOLE_CHUNK_ROWS   7U
#define DWIN_CONSOLE_CHUNK_BYTES  (DWIN_CONSOLE_CHUNK_ROWS * DWIN_CONSOLE_ROW_LEN)
#define DWIN_CONSOLE_PACE_MS      30U   /* inter-chunk delay so DWIN can absorb data */

/* ---- Magic code detection state ---- */
#define MAGIC_SEQ_LEN     3U
static const uint8_t s_magic_seq[MAGIC_SEQ_LEN] = { '1', '0', '1' };  /* then '*' triggers */
static const uint8_t s_magic2_seq[MAGIC_SEQ_LEN] = { '1', '0', '2' }; /* then '*' triggers page 2 */
static uint8_t s_magic_pos = 0U;          /* how many of s_magic_seq matched so far */
static uint8_t s_magic2_pos = 0U;         /* how many of s_magic2_seq matched so far */
static volatile uint8_t s_diag_oneshot = 0U;   /* 1 = one full diag rotation requested */
static uint8_t s_diag_oneshot_remaining = 0U;  /* lines left in current oneshot cycle */

volatile uint8_t s_console_remain = 0U;

void hmi_notify_1hz_tick(void) {
    if (s_console_remain > 0U) {
        s_console_remain--;
    }
}

/* ---- Diag cancel: set by StartTaskHmi on '*' in idle, read by StartTaskHmiMsg ---- */
static volatile uint8_t s_diag_cancel = 0U;

/* ---- Console state ---- */
static uint8_t s_console_line_idx = 0U;
static char s_console_buf[DWIN_CONSOLE_BUF_LEN];  /* 15×34 + 2 = 512 bytes (rows + 0xFFFF term) */

/* ---- Auth page return (tick-based, polled in StartTaskHmiMsg) ---- */
static volatile uint32_t s_auth_page_return_tick = 0U;  /* 0 = inactive */

/* ---- Diag error sum for change detection ---- */
static uint32_t hmi_diag_error_sum(void)
{
	const app_i2c_slave_diag_t *d = app_i2c_slave_get_diag();
	return d->progress_timeout_count + d->stuck_scl_count
	     + d->stuck_sda_count + d->abort_count
	     + d->hard_recover_count + d->malformed_count
	     + d->recover_fail_count;
}

static uint8_t hmi_extract_input_code(const MsgUart_t *uart_msg, uint8_t *code);
static void resetpinbuf(void);
static void resetpindata(void);
static void dwin_input_output(const uint8_t *text, size_t textlen);

/* Forward declarations for console */
static void hmi_update_console(void);
static void hmi_console_reset_buffer(void);
static void hmi_console_fill_all(void);

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
				s_console_line_idx = 0U;
				s_console_remain = 60U;
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
		if (s_console_remain > 0U) {
			if (was_console == 0U) {
				/* Console just activated — fill all rows once, flush once, switch page */
				was_console = 1U;
				last_console_remain = s_console_remain;
				s_console_line_idx = 0U;
				hmi_console_reset_buffer();
				hmi_console_fill_all();
				dwin_gfx_page_switch(2U);
			} else if (s_console_remain != last_console_remain) {
				/* Console tick — update one line */
				last_console_remain = s_console_remain;
				hmi_update_console();
			}
		} else if (was_console != 0U) {
			was_console = 0U;
			last_console_remain = 0U;
			/* Write 0xFFFF terminator to console base VP so DWIN clears
			 * the text widget before we leave the page. */
			{
				const uint8_t term[2] = { 0xFF, 0xFF };
				dwin_text_output(DWIN_CONSOLE_VP_ADDR, term, sizeof(term));
			}
			osDelay(DWIN_CONSOLE_PACE_MS);  /* let UART+DWIN finish */
			dwin_gfx_page_switch(0);
			/* Force immediate clock redraw after returning from console */
			time_tick = 0U;
		}

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
			
			if (was_console != 0U) { /* Skip diag/clock if console is active */
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

					if (diag_index < diag_hw_count) {
						/* Hardware diag counter lines */
						app_i2c_slave_format_diag_line(diag_index, diag_buf, sizeof(diag_buf));
					} else {
						/* Last line: current time (HH:MM:SS only) */
						const char *ts = service_time_sync_get_datetime_str();
						if (ts != NULL && ts[0] != '\0') {
							size_t tslen = strlen(ts);
							const char *hms = (tslen > 9U) ? &ts[9] : ts;
							size_t hmslen = (tslen > 9U) ? (tslen - 9U) : tslen;
							if (hmslen >= sizeof(diag_buf)) hmslen = sizeof(diag_buf) - 1U;
							memcpy(diag_buf, hms, hmslen);
							diag_buf[hmslen] = '\0';
						} else {
							snprintf(diag_buf, sizeof(diag_buf), "--:--:--");
						}
					}
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
					const char *time_str = service_time_sync_get_datetime_str();
					if (time_str != NULL && time_str[0] != '\0') {
						size_t tlen = strlen(time_str);
						/* "DD.MM.YY-HH:MM:SS" → skip first 9 chars → "HH:MM:SS" */
						if (tlen > 9U) {
							dwin_timer_output(&time_str[9], tlen - 9U);
						} else {
							dwin_timer_output(time_str, tlen);
						}
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

/* ---- Console Output ----------------------------------------------------- */

/* Send total_bytes from s_console_buf to DWIN in safe-sized chunks.
 * Chunks are sent in REVERSE order (highest VP first, base VP 0x6000 last)
 * because DWIN text widgets refresh on a write to their base VP address.
 * Writing the base address last ensures all data is in place before render. */
static void hmi_console_flush(uint16_t total_bytes)
{
    /* Calculate number of chunks */
    uint16_t n_chunks = (total_bytes + DWIN_CONSOLE_CHUNK_BYTES - 1U)
                      / DWIN_CONSOLE_CHUNK_BYTES;
    if (n_chunks == 0U) return;

    /* Send from last chunk to first (base VP last triggers DWIN render).
     * Pace with osDelay between chunks so DWIN can absorb each one. */
    for (uint16_t ci = n_chunks; ci > 0U; ci--) {
        uint16_t idx = ci - 1U;
        uint16_t offset = idx * DWIN_CONSOLE_CHUNK_BYTES;
        uint16_t remain = total_bytes - offset;
        uint16_t chunk = (remain > DWIN_CONSOLE_CHUNK_BYTES)
                       ? DWIN_CONSOLE_CHUNK_BYTES : remain;
        uint16_t word_off = offset / 2U;
        uint16_t addr = (uint16_t)(DWIN_CONSOLE_VP_ADDR + word_off);
        dwin_text_output(addr, (uint8_t*)&s_console_buf[offset], chunk);

        /* Pace: give DWIN time to process before sending next chunk */
        if (ci > 1U) {
            osDelay(DWIN_CONSOLE_PACE_MS);
        }
    }
}

/* Send the entire buffer (all 15 rows + 0xFF terminator) to DWIN. */
static void hmi_console_flush_all(void)
{
    hmi_console_flush((uint16_t)sizeof(s_console_buf));
}

/* Pre-fill RAM buffer with spaces+CRLF and 0xFF terminator (no DWIN write). */
static void hmi_console_reset_buffer(void)
{
    for (uint8_t i = 0U; i < DWIN_CONSOLE_LINES; i++) {
        size_t base = (size_t)i * DWIN_CONSOLE_ROW_LEN;
        memset(&s_console_buf[base], ' ', DWIN_CONSOLE_TEXT_LEN);
        s_console_buf[base + DWIN_CONSOLE_TEXT_LEN + 0U] = '\r';
        s_console_buf[base + DWIN_CONSOLE_TEXT_LEN + 1U] = '\n';
    }
    s_console_buf[DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 0U] = (char)0xFF;
    s_console_buf[DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 1U] = (char)0xFF;
}

/* Format one row into s_console_buf at position row_idx (no DWIN send).
 *
 * Row map (15 lines):
 *  0  HEADER            7  I2C RELISTEN
 *  1  UPTIME / LEFT     8  I2C STUCK SCL
 *  2  HEAP FREE         9  I2C RECOVERED
 *  3  HEAP MIN FREE    10  NO PROGRESS s
 *  4  LIBC HEAP USED   11  TOP-1 task (min watermark)
 *  5  MSP USED/SIZE    12  TOP-2 task
 *  6  .BSS+.DATA       13  TOP-3 task
 *                      14  RTC
 */
static void hmi_console_format_row(uint8_t row_idx)
{
    const app_i2c_slave_diag_t *d = app_i2c_slave_get_diag();
    const char *rtc = service_time_sync_get_datetime_str();
    uint32_t uptime = HAL_GetTick() / 1000U;
    uint32_t h = uptime / 3600U;
    uint32_t m = (uptime % 3600U) / 60U;
    uint32_t s = uptime % 60U;
    uint32_t since_progress = 0U;
    int written = 0;
    char tmp[DWIN_CONSOLE_TEXT_LEN + 1U];

    if (h > 999U) h = 999U;
    if (d->last_progress_tick != 0U) {
        uint32_t now = HAL_GetTick();
        if (now >= d->last_progress_tick)
            since_progress = (now - d->last_progress_tick) / 1000U;
    }
    if (rtc == NULL || rtc[0] == '\0')
        rtc = "--.--.-- --:--:--";

    /* ---- Rows 11-13: top-3 heaviest tasks (lowest stack watermark) ---- */
    if (row_idx >= 11U && row_idx <= 13U) {
        TaskStatus_t task_arr[DWIN_CONSOLE_MAX_TASKS];
        UBaseType_t n_tasks = uxTaskGetSystemState(task_arr, DWIN_CONSOLE_MAX_TASKS, NULL);

        uint8_t rank = row_idx - 11U;  /* 0, 1, or 2 */
        const char *name = "---";
        uint32_t wm = 0U;
        for (uint8_t r = 0U; r <= rank; r++) {
            uint16_t min_wm = 0xFFFFU;
            UBaseType_t min_idx = 0U;
            uint8_t found = 0U;
            for (UBaseType_t i = 0U; i < n_tasks; i++) {
                if (task_arr[i].usStackHighWaterMark < min_wm) {
                    min_wm = task_arr[i].usStackHighWaterMark;
                    min_idx = i;
                    found = 1U;
                }
            }
            if (found) {
                if (r == rank) {
                    name = task_arr[min_idx].pcTaskName;
                    wm = (uint32_t)task_arr[min_idx].usStackHighWaterMark;
                }
                task_arr[min_idx].usStackHighWaterMark = 0xFFFFU;
            }
        }
        /* watermark is in StackType_t words (4 bytes on CM4) */
        written = snprintf(tmp, sizeof(tmp), "%-16s stk%5lu", name, wm * 4UL);
    } else {
        switch (row_idx) {
        case 0:
            written = snprintf(tmp, sizeof(tmp), "=== SYSTEM DIAG PAGE 2 ===");
            break;
        case 1:
            written = snprintf(tmp, sizeof(tmp), "UP %03lu:%02lu:%02lu   LEFT %02lu",
                               h, m, s, (uint32_t)s_console_remain);
            break;
        case 2:
            written = snprintf(tmp, sizeof(tmp), "HEAP FREE      %10u",
                               (unsigned)xPortGetFreeHeapSize());
            break;
        case 3:
            written = snprintf(tmp, sizeof(tmp), "HEAP MIN FREE  %10u",
                               (unsigned)xPortGetMinimumEverFreeHeapSize());
            break;
        case 4: {
            /* libc heap used: current sbrk() watermark minus _end */
            extern uint8_t _end;           /* linker symbol: end of .bss */
            extern void *_sbrk(ptrdiff_t); /* sysmem.c */
            uint8_t *brk = (uint8_t *)_sbrk(0);
            uint32_t libc_used = (brk > &_end) ? (uint32_t)(brk - &_end) : 0U;
            written = snprintf(tmp, sizeof(tmp), "LIBC HEAP USED %10lu", libc_used);
            break;
        }
        case 5: {
            /* MSP usage: _estack (top) minus current MSP value */
            extern uint8_t _estack;        /* linker: top of RAM = MSP initial */
            extern uint32_t _Min_Stack_Size; /* linker: reserved MSP area */
            uint32_t msp_top  = (uint32_t)&_estack;
            uint32_t msp_size = (uint32_t)&_Min_Stack_Size;  /* value IS the size */
            uint32_t msp_cur  = __get_MSP();
            uint32_t msp_used = (msp_top > msp_cur) ? (msp_top - msp_cur) : 0U;
            written = snprintf(tmp, sizeof(tmp), "MSP %5lu / %5lu B",
                               msp_used, msp_size);
            break;
        }
        case 6: {
            /* .bss + .data size (global/static variables footprint) */
            extern uint8_t _sdata, _edata; /* linker: .data boundaries */
            extern uint8_t _sbss, _ebss;   /* linker: .bss boundaries */
            uint32_t data_sz = (uint32_t)(&_edata - &_sdata);
            uint32_t bss_sz  = (uint32_t)(&_ebss  - &_sbss);
            written = snprintf(tmp, sizeof(tmp), ".BSS+.DATA     %10lu",
                               data_sz + bss_sz);
            break;
        }
        case 7:
            written = snprintf(tmp, sizeof(tmp), "I2C RELISTEN   %10lu",
                               d->relisten_count);
            break;
        case 8:
            written = snprintf(tmp, sizeof(tmp), "I2C STUCK SCL  %10lu",
                               d->stuck_scl_count);
            break;
        case 9:
            written = snprintf(tmp, sizeof(tmp), "I2C RECOVERED  %10lu",
                               d->hard_recover_count);
            break;
        case 10:
            written = snprintf(tmp, sizeof(tmp), "NO PROGRESS s  %10lu",
                               since_progress);
            break;
        /* rows 11-13 handled above (top-3 tasks) */
        case 14:
            written = snprintf(tmp, sizeof(tmp), "RTC %-28s", rtc);
            break;
        default:
            return;
        }
    }

    if (written < 0) written = 0;
    if ((size_t)written > DWIN_CONSOLE_TEXT_LEN) written = (int)DWIN_CONSOLE_TEXT_LEN;

    char *row = &s_console_buf[(size_t)row_idx * DWIN_CONSOLE_ROW_LEN];
    memcpy(row, tmp, (size_t)written);
    if ((size_t)written < DWIN_CONSOLE_TEXT_LEN) {
        memset(&row[written], ' ', DWIN_CONSOLE_TEXT_LEN - (size_t)written);
    }
    row[DWIN_CONSOLE_TEXT_LEN + 0U] = '\r';
    row[DWIN_CONSOLE_TEXT_LEN + 1U] = '\n';
}

/* Fill all 15 rows into the buffer and send to DWIN once. */
static void hmi_console_fill_all(void)
{
    for (uint8_t r = 0U; r < DWIN_CONSOLE_LINES; r++) {
        hmi_console_format_row(r);
    }
    /* 0xFF terminator is already at the end from reset_buffer */
    hmi_console_flush_all();
}

/* Update one row (round-robin) and send to DWIN. */
static void hmi_update_console(void) {
    hmi_console_format_row(s_console_line_idx);
    hmi_console_flush_all();

    s_console_line_idx++;
    if (s_console_line_idx >= DWIN_CONSOLE_LINES) {
        s_console_line_idx = 0U;
    }
}
