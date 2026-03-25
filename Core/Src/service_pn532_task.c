/*
 * service_pn532_task.c
 *
 * PN532 NFC reader task.
 * Перенесено из main.c (шаг 10 рефакторинга).
 *
 * Владеет:
 *   - StartTask532 task body
 *   - pn532_probe_bounded() helper
 *   - pn532_t pn532 handle
 *   - slaveTxData[64], uid[32], response, pn_i2c_fault
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

/* ---- extern RTOS handles from main.c ----------------------------------- */
extern osSemaphoreId pn532SemaphoreHandle;
extern osMessageQId  myQueueToMasterHandle;
extern osMessageQId  myQueueOLEDHandle;
extern osMessageQId  myQueueHmiMsgHandle;
extern osTimerId     myTimerBuzzerOffHandle;

/* ---- internal state (moved from main.c) -------------------------------- */
static pn532_t        s_pn532;
static pn532_result_t s_response;
static uint8_t        s_uid[32];
static uint8_t        s_slaveTxData[64];
static int            s_pn_i2c_fault = 1;

/* ---- PN532 bounded probe helper ---------------------------------------- */
#define PN532_PROBE_MAX_RETRIES  200U   /* 200 * 1ms = 200ms budget */
#define PN532_PROBE_OK           1U
#define PN532_PROBE_FAIL         0U

static uint8_t pn532_probe_bounded(pn532_t *pn, uint8_t *probe_buf) {
	for (uint16_t i = 0; i < PN532_PROBE_MAX_RETRIES; i++) {
		pn532_read(pn, probe_buf, 1);
		if (probe_buf[0] == 0x01) {
			probe_buf[0] = 0;
			osDelay(1);
			return PN532_PROBE_OK;
		}
		osDelay(1);
	}
	probe_buf[0] = 0;
	return PN532_PROBE_FAIL;
}

/* ---- public API -------------------------------------------------------- */

void service_pn532_init(void)
{
	memset(&s_pn532, 0, sizeof(pn532_t));
	memset(&s_response, 0, sizeof(pn532_result_t));
	memset(s_uid, 0, sizeof(s_uid));
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
	uint8_t probe[1] = { 0 };
	uint8_t pn_ack[32] = { 0 };
	uint8_t sam[32] = { };
	uint8_t stat[32] = { };

	osDelay(50);

	for (;;) {
		const runtime_config_t *cfg = runtime_config_get();
		uint8_t reader_interval = (cfg != NULL) ? cfg->reader_interval_sec
		                                        : READER_INTERVAL_SEC_DEFAULT;

		if (s_pn_i2c_fault) {
			pn532_send_command(&s_pn532, SAMConfiguration, cmd, 1);
			osDelay(1);
			if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
			pn532_read(&s_pn532, pn_ack, 7);
			osDelay(5);
			if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
			pn532_read(&s_pn532, sam, 15);
			osDelay(1);
			pn532_send_command(&s_pn532, GetGeneralStatus, cmd, 0);
			osDelay(1);
			if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
			pn532_read(&s_pn532, pn_ack, 7);
			osDelay(5);
			if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
			pn532_read(&s_pn532, stat, 15);
			s_pn_i2c_fault = 0;
		}

		pn532_send_command(&s_pn532, InListPassiveTarget, cmd, 2);
		memset(&pn_ack[0], 0xCC, 32);
		probe[0] = 0;
		osDelay(1);
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
		if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
		pn532_read(&s_pn532, pn_ack, 7);
		memset(s_slaveTxData, 0x04, 64);
		osDelay(10);
		osSemaphoreWait(pn532SemaphoreHandle, osWaitForever);
		/* Bounded wait for PN532 data ready after semaphore */
		if (!pn532_probe_bounded(&s_pn532, probe)) { s_pn_i2c_fault = 1; osDelay(500); continue; }
		pn532_read(&s_pn532, s_slaveTxData, 32);
		s_pn_i2c_fault = 1;
		MsgHmi_t pn532_msg = { .hmi_lock = LOCKED, .msg_ttl = 1, .msg_buf =
				HMI_MSG_KEY, .psize = strlen(HMI_MSG_KEY),
		};
		xQueueSendToFront(myQueueHmiMsgHandle, &pn532_msg, 1);
		HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
		osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
		/* Queued send card uid to Master */
		I2cPacketToMaster_t pckt;
		pckt.payload = &s_slaveTxData[13];  /* TODO: copy-into-outbox for safety */
		pckt.len = 8;
		pckt.type = PACKET_UID_532;
		pckt.ttl = uid_ttl;
		xQueueSendToFront(myQueueToMasterHandle, &pckt, 1);
		HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
		uint16_t sig1 = 0x01;
		xQueueSend(myQueueOLEDHandle, &sig1, 0);
		osDelay(reader_interval * 1000 + 1);
	}
	/* USER CODE END StartTask532 */
}
