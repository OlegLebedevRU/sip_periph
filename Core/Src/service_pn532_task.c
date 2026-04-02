/*
 * service_pn532_task.c
 *
 * PN532 NFC reader task.
 * Перенесено из main.c (шаг 10 рефакторинга).
 *
 * Владеет:
 *   - StartTask532 task body
 *   - bounded PN532 frame-read / IRQ-poll helpers
 *   - slaveTxData[64], pn_i2c_fault
 *
 * Зависимости (extern из main.c / других модулей):
 *   - pn532SemaphoreHandle, myQueueToMasterHandle, myQueueOLEDHandle
 *   - myQueueHmiMsgHandle, myTimerBuzzerOffHandle
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "pn532_com.h"
#include "service_pn532_task.h"
#include "service_runtime_config.h"
#include "service_tca6408.h"
#include "service_time_sync.h"
#include "tca6408a_map.h"

/* ---- extern RTOS handles from main.c ----------------------------------- */
extern osSemaphoreId pn532SemaphoreHandle;
extern osMessageQId  myQueueToMasterHandle;
extern osMessageQId  myQueueOLEDHandle;
extern osMessageQId  myQueueHmiMsgHandle;
extern osTimerId     myTimerBuzzerOffHandle;

/* ---- internal state ---------------------------------------------------- */
static uint8_t        s_slaveTxData[64];
static volatile int   s_pn_i2c_fault = 1;

/* ---- PN532 bounded frame-read helper ----------------------------------- */
#define PN532_PROBE_MAX_RETRIES  200U   /* 200 * 1ms = 200ms budget */
#define PN532_PROBE_OK           1U
#define PN532_PROBE_FAIL         0U
#define PN532_STATUS_FRAME_OVERHEAD 1U
#define PN532_MAX_READY_FRAME_LEN (PN532_DATA_READ_LEN + PN532_STATUS_FRAME_OVERHEAD)
#define PN532_MAX_READY_PAYLOAD_LEN (PN532_MAX_READY_FRAME_LEN - PN532_STATUS_FRAME_OVERHEAD)
#define PN532_UID_PACKET_LEN      8U
#define PN532_TCA_INPUTS_IDLE     0xFFU

/* Semaphore wait parameters for InListPassiveTarget response */
#define PN532_SEM_POLL_MS        100U   /* per-slice semaphore timeout         */
#define PN532_SEM_WAIT_MAX_ITER  150U   /* 150 * 100ms = 15 s total budget     */
#define PN532_FAULT_RETRY_DELAY_MS 500U /* back-off delay after any I2C fault  */

static uint8_t pn532_read_ready_frame_bounded(uint8_t *frame_buf, size_t frame_len)
{
	if (frame_len > PN532_MAX_READY_PAYLOAD_LEN) {
		/* Local guard only: all current callers use fixed PN532_*_READ_LEN values. */
		return PN532_PROBE_FAIL;
	}

	uint8_t rx[PN532_MAX_READY_FRAME_LEN];

	for (uint16_t i = 0; i < PN532_PROBE_MAX_RETRIES; i++) {
		int r = pn532_read(rx, frame_len + PN532_STATUS_FRAME_OVERHEAD);
		if ((r > 0) && (rx[0] == PN532_READY_BYTE)) {
			memcpy(frame_buf, &rx[1], frame_len);
			osDelay(1);
			return PN532_PROBE_OK;
		}
		osDelay(1);
	}
	return PN532_PROBE_FAIL;
}

static uint8_t pn532_irq_ready_poll_once(void)
{
	uint8_t curr_inputs = PN532_TCA_INPUTS_IDLE;

	if (service_tca6408_read_reg(TCA6408A_REG_INPUT, &curr_inputs) != HAL_OK) {
		return 0U;
	}

	return (uint8_t)((curr_inputs & TCA_P3_PN532_IRQ) == 0U);
}

/* ---- public API -------------------------------------------------------- */

void service_pn532_init(void)
{
	memset(s_slaveTxData, 0, sizeof(s_slaveTxData));
	s_pn_i2c_fault = 1;
}

void service_pn532_notify_i2c_fault(void)
{
	s_pn_i2c_fault = 1;
}

uint8_t *service_pn532_get_slaveTxData(void)
{
	return s_slaveTxData;
}

/* ---- FreeRTOS task body ------------------------------------------------ */

void StartTask532(void const *argument)
{
	/* USER CODE BEGIN StartTask532 */
	uint8_t cmd[2] = { 0x01, 0x00 };
	uint8_t pn_ack[32] = { 0 };
	uint8_t sam[32] = { 0 };
	uint8_t stat[32] = { 0 };

	osDelay(50);

	for (;;) {
		const runtime_config_t *cfg = runtime_config_get();
		uint8_t reader_interval = (cfg != NULL) ? cfg->reader_interval_sec
		                                        : READER_INTERVAL_SEC_DEFAULT;

		if (s_pn_i2c_fault) {
			pn532_send_command(SAMConfiguration, cmd, 1);
			osDelay(1);
			if (!pn532_read_ready_frame_bounded(pn_ack, PN532_ACK_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
			osDelay(5);
			if (!pn532_read_ready_frame_bounded(sam, PN532_RESP_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
			osDelay(1);
			pn532_send_command(GetGeneralStatus, cmd, 0);
			osDelay(1);
			if (!pn532_read_ready_frame_bounded(pn_ack, PN532_ACK_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
			osDelay(5);
			if (!pn532_read_ready_frame_bounded(stat, PN532_RESP_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
			s_pn_i2c_fault = 0;
		}

		pn532_send_command(InListPassiveTarget, cmd, 2);
		memset(&pn_ack[0], 0xCC, 32);
		osDelay(1);
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
		if (!pn532_read_ready_frame_bounded(pn_ack, PN532_ACK_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
		memset(s_slaveTxData, 0x04, 64);
		osDelay(10);
		/* P4: drain stale semaphore releases that may have accumulated while
		 * the task was processing the previous card or sleeping. */
		while (osSemaphoreWait(pn532SemaphoreHandle, 0) == osOK) { /* drain */ }

		/* P1: wait for PN532 IRQ signal (via TCA6408A semaphore release),
		 * with 100ms-slice single-ACK fallback to survive a stuck IRQ line.
		 *
		 * Each iteration:
		 *   1. Wait on semaphore up to PN532_SEM_POLL_MS (100ms).
		 *      → TCA detects P3 LOW and releases semaphore on the normal path.
		 *   2. On timeout: direct TCA input poll for PN532 IRQ level.
		 *      → Avoids consuming PN532 status byte/frame before the full read.
		 * Total budget: PN532_SEM_WAIT_MAX_ITER * PN532_SEM_POLL_MS = 15 s. */
		{
			uint8_t sem_ready = 0U;
			for (uint16_t w = 0U; w < PN532_SEM_WAIT_MAX_ITER; w++) {
				if (osSemaphoreWait(pn532SemaphoreHandle, PN532_SEM_POLL_MS) == osOK) {
					sem_ready = 1U;
					break;
				}
				/* Timeout: direct TCA input poll — does not consume PN532 response bytes */
				if (pn532_irq_ready_poll_once() != 0U) {
					sem_ready = 1U;
					break;
				}
			}
			if (!sem_ready) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
		}
		/* Bounded wait for PN532 data ready after semaphore */
		if (!pn532_read_ready_frame_bounded(s_slaveTxData, PN532_DATA_READ_LEN)) { s_pn_i2c_fault = 1; osDelay(PN532_FAULT_RETRY_DELAY_MS); continue; }
		s_pn_i2c_fault = 1;
		MsgHmi_t pn532_msg = { .hmi_lock = LOCKED, .msg_ttl = 1, .msg_buf =
				HMI_MSG_KEY, .psize = strlen(HMI_MSG_KEY),
		};
		xQueueSendToFront(myQueueHmiMsgHandle, &pn532_msg, 1);
		HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
		osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
		/* Queued send card uid to Master */
		I2cPacketToMaster_t pckt;
		pckt.payload = &s_slaveTxData[13];  /* PN532 UID window: uid_len + up to 7 UID bytes */
		pckt.len = PN532_UID_PACKET_LEN;
		pckt.type = PACKET_UID_532;
		pckt.ttl = service_time_sync_get_uptime_sec() + TTL_PACKET_SEC;
		xQueueSendToFront(myQueueToMasterHandle, &pckt, 1);
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
		uint16_t sig1 = 0x01;
		xQueueSend(myQueueOLEDHandle, &sig1, 0);
		osDelay(reader_interval * 1000 + 1);
	}
	/* USER CODE END StartTask532 */
}
