/*
 * leo4_stm32_bus.c
 *
 *  Created on: 25 04 2025
 *      Author: oleg_
 */
#include "board_pins_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "l4_def.h"
#include "l4_frontend.h"
#include "l4_mqtt5.h"
#include "l4_cloud_codec.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tca6408a.h"
#include <time.h>
#include "l4_i2c_master.h"
#include "l4_stm32_bus.h"
#include "l4_sip.h"
// === Configuration ===
#define I2C_HEADER_LEN         (0x02U)
#define STM32_I2C_DEV_ADDR     (1)           // Logical device ID for l4_i2c_dev
#define PACKET_QUEUE_LENGTH    4
#define STM32_QUEUE_EVENT_I2C  ((uint32_t)1U)
#define STM32_CFG_CACHE_LEN    16U
#define STM32_GM810_PAYLOAD_LEN 12U
#define TASK_STACK_SIZE        4096
#define TASK_PRIORITY          10
#define ESP_INTR_FLAG_DEFAULT 0
/* Anti back-to-back sequencing for STM32 bus stability (wire protocol unchanged). */
#define STM32_I2C_PACE_AFTER_TYPE_READ_MS       2U
#define STM32_I2C_PACE_AFTER_PAYLOAD_READ_MS    2U
#define STM32_I2C_PACE_BEFORE_COUNTER_WRITE_MS  5U
#define STM32_I2C_PACE_BEFORE_CFG_WRITE_MS      5U
#define STM32_I2C_PACE_GENERIC_WRITE_MS         5U
#include "l4_cfg_repo.h"
/*
 * STM32 I2C BUS packet type interface
 */
typedef enum {
	PACKET_NULL,
	PACKET_UID_532,
	PACKET_PIN,
	PACKET_WIEGAND,
	PACKET_HMI,
	PACKET_TIME,
	PACKET_PIN_HMI,
	PACKET_ACK,
	PACKET_NACK,
	PACKET_ERROR,
	PACKET_QR_GM810,
	PACKET_MAX
} I2cPacketType_t;

typedef struct {
	uint8_t data_len;
	uint8_t flags;
	uint8_t chunk_index;
	uint8_t chunk_total;
	uint8_t data[STM32_GM810_PAYLOAD_LEN];
} Stm32Gm810Packet_t;
//
/* private config*/
static uint8_t cfg_stm32_0xE0_0xEF[16];

static const char *TAG = "STM32_bus";
static uint8_t count1[2] = {0, 3};
static uint8_t data[16] = {};
static QueueHandle_t xQueueSTM32PackedReceived;
static portMUX_TYPE s_cfg_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_irq_drop_count = 0;
static volatile uint32_t s_irq_drop_count_logged = 0;

#define TIME_SYNC_NTP_VALID_EPOCH       ((time_t)1704067200) /* 2024-01-01 00:00:00 local guard */
#define TIME_SYNC_FALLBACK_MIN_EPOCH    ((time_t)1773961200) /* 2026-03-20 00:00:00 local */
#define TIME_SYNC_MAX_AGE_SEC           5

static int32_t s_time_age_sec = -1;
static char s_time_age_sec_str[16] = "-1";

static uint8_t bcd_to_u8(const uint8_t bcd)
{
	return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0F));
}

static void stm32_bus_pacing_delay_ms(const uint32_t delay_ms)
{
	TickType_t ticks = pdMS_TO_TICKS(delay_ms);
	if (ticks == 0) {
		ticks = 1;
	}
	vTaskDelay(ticks);
}

static bool is_bcd_byte_valid(const uint8_t bcd, const uint8_t max_dec)
{
	const uint8_t hi = (uint8_t)(bcd >> 4);
	const uint8_t lo = (uint8_t)(bcd & 0x0F);
	if ((hi > 9U) || (lo > 9U)) {
		return false;
	}
	return bcd_to_u8(bcd) <= max_dec;
}

static bool parse_stm32_time_packet(const uint8_t *packet, time_t *out_ts)
{
	if ((packet == NULL) || (out_ts == NULL)) {
		return false;
	}
	if (!is_bcd_byte_valid(packet[0], 59) || !is_bcd_byte_valid(packet[1], 59)
		|| !is_bcd_byte_valid(packet[2], 23) || !is_bcd_byte_valid(packet[3], 7)
		|| !is_bcd_byte_valid(packet[4], 31) || !is_bcd_byte_valid(packet[5], 12)
		|| !is_bcd_byte_valid(packet[6], 99)) {
		return false;
	}

	const uint8_t weekday = bcd_to_u8(packet[3]);
	const uint8_t day = bcd_to_u8(packet[4]);
	const uint8_t month = bcd_to_u8(packet[5]);
	if ((weekday < 1U) || (weekday > 7U) || (day < 1U) || (month < 1U)) {
		return false;
	}

	struct tm stm32_local = {0};
	stm32_local.tm_sec = (int)bcd_to_u8(packet[0]);
	stm32_local.tm_min = (int)bcd_to_u8(packet[1]);
	stm32_local.tm_hour = (int)bcd_to_u8(packet[2]);
	stm32_local.tm_mday = (int)day;
	stm32_local.tm_mon = (int)month - 1;
	stm32_local.tm_year = (int)bcd_to_u8(packet[6]) + 100; /* 20YY */
	stm32_local.tm_isdst = -1;

	const time_t ts = mktime(&stm32_local);
	if (ts == (time_t)-1) {
		return false;
	}
	*out_ts = ts;
	return true;
}

static void set_time_age_metric(const int32_t age_sec)
{
	s_time_age_sec = age_sec;
	if (age_sec < 0) {
		strcpy(s_time_age_sec_str, "-1");
		return;
	}
	(void)snprintf(s_time_age_sec_str, sizeof(s_time_age_sec_str), "%ld", (long)age_sec);
}

static const char *stm32_time_age_sec_str_getter(void)
{
	return s_time_age_sec_str;
}

typedef enum {
	GM810_FLAG_FROM_PROTOCOL_MODE = (1U << 0),
	GM810_FLAG_RESERVED_CHUNKED   = (1U << 1),
	GM810_FLAG_ERROR_OVERSIZE     = (1U << 2),
	GM810_FLAG_ERROR_NON_ASCII    = (1U << 3),
} Stm32Gm810Flags_t;

static bool stm32_gm810_payload_is_ascii(const uint8_t *data, const size_t len)
{
	if (data == NULL) {
		return false;
	}
	for (size_t i = 0; i < len; ++i) {
		if ((data[i] < 0x20U) || (data[i] > 0x7EU)) {
			return false;
		}
	}
	return true;
}

static bool stm32_gm810_packet_is_normal(const Stm32Gm810Packet_t *packet)
{
	if (packet == NULL) {
		return false;
	}
	if ((packet->flags & (GM810_FLAG_ERROR_OVERSIZE | GM810_FLAG_ERROR_NON_ASCII
			| GM810_FLAG_RESERVED_CHUNKED)) != 0U) {
		return false;
	}
	if ((packet->data_len == 0U) || (packet->data_len > STM32_GM810_PAYLOAD_LEN)) {
		return false;
	}
	if ((packet->chunk_index != 0U) || (packet->chunk_total != 1U)) {
		return false;
	}
	return stm32_gm810_payload_is_ascii(packet->data, packet->data_len);
}
//

/* Binary semaphore used by stm32_bus_wait_next_int() to let a caller
 * (e.g. FW-update task) block until the next EVENT_INT_PIN edge.
 * Created lazily on first call; the ISR gives it on every interrupt. */
static SemaphoreHandle_t s_int_notify_sem = NULL;

static void stm32_bus_copy_cfg_cache(uint8_t *dst)
{
	if (dst == NULL) {
		return;
	}
	taskENTER_CRITICAL(&s_cfg_cache_lock);
	memcpy(dst, cfg_stm32_0xE0_0xEF, STM32_CFG_CACHE_LEN);
	taskEXIT_CRITICAL(&s_cfg_cache_lock);
}

esp_err_t stm32_bus_reload_cfg_cache(void)
{
	uint8_t cfg_buf[STM32_CFG_CACHE_LEN] = {0};
	esp_err_t ret = get_nvs_cfg_stm32(cfg_buf);

	taskENTER_CRITICAL(&s_cfg_cache_lock);
	memcpy(cfg_stm32_0xE0_0xEF, cfg_buf, sizeof(cfg_stm32_0xE0_0xEF));
	taskEXIT_CRITICAL(&s_cfg_cache_lock);

	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "STM32 cfg cache loaded from NVS");
	} else {
		ESP_LOGW(TAG, "STM32 cfg cache refreshed with defaults/fallbacks, NVS read status: %s",
				 esp_err_to_name(ret));
	}
	return ret;
}

static void stm32_bus_log_irq_drops_if_needed(void)
{
	const uint32_t drop_count = s_irq_drop_count;
	if (drop_count != s_irq_drop_count_logged) {
		s_irq_drop_count_logged = drop_count;
		ESP_LOGW(TAG, "STM32 IRQ events dropped in ISR: %" PRIu32, drop_count);
	}
}

static esp_err_t stm32_bus_enqueue_event(int32_t id, TickType_t timeout_ticks)
{
	const uint32_t event_id = (uint32_t)id;
	if (xQueueSTM32PackedReceived == NULL) {
		ESP_LOGW(TAG, "STM32 packet queue is not ready yet");
		return ESP_ERR_INVALID_STATE;
	}
	if (xQueueSend(xQueueSTM32PackedReceived, &event_id, timeout_ticks) != pdTRUE) {
		ESP_LOGW(TAG, "STM32 packet queue is full, dropping event %" PRId32, id);
		return ESP_ERR_TIMEOUT;
	}
	return ESP_OK;
}

// ISR handler
void IRAM_ATTR gpio_isr_handler(void *arg) {
	const uint32_t gpio_num = (uint32_t)arg;
	BaseType_t high_task_awoken = pdFALSE;
	if (gpio_num == EVENT_INT_PIN) {
		const uint32_t event_id = STM32_QUEUE_EVENT_I2C;
		if (xQueueSTM32PackedReceived != NULL) {
			if (xQueueSendFromISR(xQueueSTM32PackedReceived, &event_id, &high_task_awoken) != pdTRUE) {
				s_irq_drop_count++;
			}
		}
		/* Notify FW-update task if it is waiting for bus quiesce */
		SemaphoreHandle_t sem = s_int_notify_sem;
		if (sem != NULL) {
			BaseType_t woken = pdFALSE;
			xSemaphoreGiveFromISR(sem, &woken);
			if (woken) high_task_awoken = pdTRUE;
		}
	}
	// Если высокоприоритетная задача была разбужена
	if (high_task_awoken == pdTRUE) {
		portYIELD_FROM_ISR();
	}
}

esp_err_t stm32_bus_sync_interrupt_state(void) {
	if (gpio_get_level(EVENT_INT_PIN) != 0) {
		return ESP_OK;
	}

	ESP_LOGW(TAG, "EVENT_INT_PIN already LOW; enqueue synthetic STM32 event to recover missed negedge");
	return stm32_bus_enqueue_event(STM32_QUEUE_EVENT_I2C, 0);
}

esp_err_t stm32_act_result_write_bus(const InputAuthResult_t result) {
	esp_err_t res = ESP_OK;
	uint8_t ram_addr = I2C_REG_HMI_ACT_ADDR;
	// const int addr_len = sizeof(ram_addr);
	uint8_t rmsg[7] = {
		ram_addr,
		5,
		(uint8_t)result.result,
		(uint8_t)(result.res_register_1 >> 24),
		(uint8_t)(result.res_register_1 >> 16),
		(uint8_t)(result.res_register_1 >> 8),
		(uint8_t)(result.res_register_1 & 0xFF),
	};
//	i2c_master_probe(stm32_bus_handle, 0x11, -1);
	stm32_bus_pacing_delay_ms(STM32_I2C_PACE_GENERIC_WRITE_MS);
	res = l4_i2c_dev_write_bytes(1,ram_addr, rmsg[1],&rmsg[2]);
//	res = i2c_master_transmit(stm32_dev_handle, rmsg, 7, 100);
	if (result.msg[0] > 0 && result.msg[0] < 16) {
		ram_addr = I2C_REG_HMI_MSG_ADDR;
		uint8_t hmi[32];
		hmi[0] = ram_addr;
		hmi[1] = result.msg[0] + 3;
		memcpy(&hmi[2], result.msg, result.msg[0] + 3);
		stm32_bus_pacing_delay_ms(STM32_I2C_PACE_GENERIC_WRITE_MS);
		res |= l4_i2c_dev_write_bytes(1,ram_addr,hmi[1], &hmi[2]);
	}
	return res;
}
esp_err_t stm32_hmi_msg_write_bus_ex(const char *text, uint8_t ttl_sec, uint8_t hmi_lock) {
	if (text == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	size_t slen = strlen(text);
	if (slen == 0 || slen > 11) {
		return ESP_ERR_INVALID_ARG;
	}
	const uint8_t ram_addr = I2C_REG_HMI_MSG_ADDR;
	/* Register layout: 0x50=msg_len, 0x51=msg_ttl, 0x52=hmi_lock, 0x53..0x5E=msg_buf[12] */
	uint8_t buf[15]; /* 3 header + 12 text buffer */
	buf[0] = (uint8_t)slen;
	buf[1] = ttl_sec;
	buf[2] = hmi_lock;
	memcpy(&buf[3], text, slen);
	memset(&buf[3 + slen], ' ', 12 - slen);
	stm32_bus_pacing_delay_ms(STM32_I2C_PACE_GENERIC_WRITE_MS);
	return l4_i2c_dev_write_bytes(1, ram_addr, 15, buf);
}

esp_err_t stm32_hmi_msg_write_bus(const char *text) {
	return stm32_hmi_msg_write_bus_ex(text, 5, 0);
}

static void stm32_task(void *arg) {
	I2cPacketType_t ptype = PACKET_NULL;
	uint32_t event_id = 0;
	uint8_t pckt_type = PACKET_NULL;
	uint8_t ram_addr = I2C_PACKET_TYPE_ADDR;
	uint8_t cfg_cache_local[STM32_CFG_CACHE_LEN] = {0};
	(void)arg;

	while (1) {
		bool payload_read_ok = false;
		memset(data, 0, 16);
		xQueueReceive(xQueueSTM32PackedReceived, &event_id, portMAX_DELAY);
		if (event_id != STM32_QUEUE_EVENT_I2C) {
			ESP_LOGW(TAG, "Skip unknown STM32 queue event: %" PRIu32, event_id);
			continue;
		}
		stm32_bus_log_irq_drops_if_needed();
		tca6408a_set_out_bit(LED_BLUE);  // LED on during processing
		vTaskDelay(1 / portTICK_PERIOD_MS);
		memset(data, 0, sizeof(data));
		//vTaskDelay(10 / portTICK_PERIOD_MS);
		ram_addr = I2C_PACKET_TYPE_ADDR;
			if (l4_i2c_dev_read_bytes(1,ram_addr,1,&pckt_type)== ESP_OK) {
				stm32_bus_pacing_delay_ms(STM32_I2C_PACE_AFTER_TYPE_READ_MS);
				ptype = (I2cPacketType_t)pckt_type;
			if (ptype > PACKET_NULL && ptype < PACKET_MAX) {
			} else {
		//		gpio_intr_enable(EVENT_INT_PIN);
				ESP_LOGI(TAG, "+++++++++++ STM32_I2C READ PACKET TYPE error");
				continue;
			}
			if (ptype == PACKET_UID_532) {
				ESP_LOGI(TAG, "+++++++++++ STM32_I2C READ PACKET PACKET_UID_532");
				ram_addr = I2C_REG_532_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 15,data) == ESP_OK) {
					payload_read_ok = true;
					if (frontend_input_enqueue(PN532_READER,
										  (const FrontendSourceInputLenght_t)data[0],
										  &data[1]) != ESP_OK) {
						ESP_LOGW(TAG, "drop PN532 input: worker busy");
					}
				}
			} else if (ptype == PACKET_PIN) {
				ram_addr = I2C_REG_MATRIX_PIN_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 13,data) == ESP_OK) {
					payload_read_ok = true;
					if (frontend_input_enqueue(MATRIX_KEYBOARD,
										  (const FrontendSourceInputLenght_t)data[1],
										  &data[2]) != ESP_OK) {
						ESP_LOGW(TAG, "drop MATRIX input: worker busy");
					}
				}
			} else if (ptype == PACKET_WIEGAND) {
				ram_addr = I2C_REG_WIEGAND_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 15,data) == ESP_OK) {
					payload_read_ok = true;
					if (frontend_input_enqueue(
						WIEGAND_PIN_READER,
						(const FrontendSourceInputLenght_t)data[1],
						&data[2]) != ESP_OK) {
						ESP_LOGW(TAG, "drop WIEGAND input: worker busy");
					}
				}
			} else if (ptype == PACKET_PIN_HMI) {
				ram_addr = I2C_REG_HMI_PIN_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 15,data) == ESP_OK) {
					payload_read_ok = true;
					if (frontend_input_enqueue(TOUCH_KEYPAD,
										  (const FrontendSourceInputLenght_t)data[1],
										  &data[2]) != ESP_OK) {
						ESP_LOGW(TAG, "drop TOUCH input: worker busy");
					}
				}
			} else if (ptype == PACKET_QR_GM810) {
				Stm32Gm810Packet_t gm810_packet = {0};
				ram_addr = I2C_REG_QR_GM810_ADDR;
				if (l4_i2c_dev_read_bytes(1, ram_addr, sizeof(gm810_packet),
								 (uint8_t *)&gm810_packet) == ESP_OK) {
					payload_read_ok = true;
					if (!stm32_gm810_packet_is_normal(&gm810_packet)) {
						ESP_LOGW(TAG,
							 "reject GM810 packet: len=%u flags=0x%02X chunk=%u/%u protocol=%u",
							 (unsigned)gm810_packet.data_len,
							 (unsigned)gm810_packet.flags,
							 (unsigned)gm810_packet.chunk_index,
							 (unsigned)gm810_packet.chunk_total,
							 (unsigned)((gm810_packet.flags
									& GM810_FLAG_FROM_PROTOCOL_MODE) != 0U));
					} else if (frontend_input_enqueue(
							GM810_READER,
							(const FrontendSourceInputLenght_t)gm810_packet.data_len,
							gm810_packet.data) != ESP_OK) {
						ESP_LOGW(TAG, "drop GM810 input: worker busy");
					}
				}
			} else if (ptype == PACKET_TIME) {
				uint8_t time_packet[8] = {0};
				ram_addr = I2C_REG_HW_TIME_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr,8, time_packet) == ESP_OK) {
					payload_read_ok = true;
					time_t stm32_ts = 0;
					const bool stm32_time_valid = parse_stm32_time_packet(time_packet, &stm32_ts);
					const time_t esp_now = time(NULL);
					const bool ntp_time_valid = (esp_now >= TIME_SYNC_NTP_VALID_EPOCH);

					if (ntp_time_valid && stm32_time_valid) {
						const int32_t age_sec = (int32_t)llabs((long long)difftime(esp_now, stm32_ts));
						set_time_age_metric(age_sec);
						if (age_sec > TIME_SYNC_MAX_AGE_SEC) {
							ESP_LOGW(TAG, "STM32 time age exceeded: %ld sec", (long)age_sec);
						}
					} else if (!ntp_time_valid && stm32_time_valid && (stm32_ts > TIME_SYNC_FALLBACK_MIN_EPOCH)) {
						/* NTP is not valid yet: trust STM32/DS3231M if timestamp passed fallback threshold. */
						set_time_age_metric(0);
					} else {
						set_time_age_metric(-1);
					}

					if (ntp_time_valid) {
						struct tm timi;
						localtime_r(&esp_now, &timi);
						/* Contract: Monday=1 ... Sunday=7 */
						const uint8_t weekday_mon1 = (uint8_t)((timi.tm_wday == 0) ? 7 : timi.tm_wday);
						const BYTE time_bcd[7]= {
							convertToBcd(timi.tm_sec),
							convertToBcd(timi.tm_min),
							convertToBcd(timi.tm_hour),
							convertToBcd(weekday_mon1),
							convertToBcd(timi.tm_mday),
							convertToBcd(timi.tm_mon + 1),
							convertToBcd((timi.tm_year + 1900) % 100)
						};
						ESP_LOGD(TAG, "Time sync local %02d:%02d age=%s",
								 timi.tm_hour, timi.tm_min, s_time_age_sec_str);
						stm32_bus_pacing_delay_ms(STM32_I2C_PACE_GENERIC_WRITE_MS);
					l4_i2c_dev_write_bytes(1, I2C_REG_HW_TIME_ADDR + 8, 7, &time_bcd[0]);
				}
			}
			/* --- SIP call state → HMI display on every TIME tick --- */
			{
				static uint8_t s_sip_hmi_tick = 0;
				static l4_sip_call_state_t s_sip_hmi_prev = L4_SIP_CALL_IDLE;
				static const char *s_dot_frames[] = {".", "..", "..."};

				const l4_sip_call_state_t cs = l4_sip_get_call_state();
				if (cs != s_sip_hmi_prev) {
					s_sip_hmi_tick = 0;
					s_sip_hmi_prev = cs;
				}
				if (cs != L4_SIP_CALL_IDLE) {
					const char *label = l4_sip_call_state_label(cs);
					if (label != NULL) {
						const bool transient = l4_sip_call_state_is_transient(cs);
						char hmi_text[12];
						if (!transient) {
							/* Persistent state: animate dots + digit for alive */
							const uint8_t digit = (uint8_t)((s_sip_hmi_tick % 9U) + 1U);
							const char *dots = s_dot_frames[s_sip_hmi_tick % 3U];
							snprintf(hmi_text, sizeof(hmi_text), "%s %u%s",
									 label, (unsigned)digit, dots);
						} else {
							/* Transient state: show label once */
							snprintf(hmi_text, sizeof(hmi_text), "= %s =", label);
						}
						const uint8_t ttl  = transient ? (uint8_t)2 : (uint8_t)1;
						const uint8_t lock = transient ? (uint8_t)1 : (uint8_t)0;
						stm32_hmi_msg_write_bus_ex(hmi_text, ttl, lock);
						s_sip_hmi_tick++;
						if (transient) {
							l4_sip_call_state_clear_if(cs);
						}
					}
				}
			}
		} else if (ptype == PACKET_ERROR) {
				ram_addr = I2C_REG_STM32_ERROR_ADDR;
				/*
				* TODO: error from stm32
				*/
			}
				if (payload_read_ok) {
					stm32_bus_pacing_delay_ms(STM32_I2C_PACE_AFTER_PAYLOAD_READ_MS);
				}
				ram_addr = I2C_REG_COUNTER_ADDR;
				stm32_bus_pacing_delay_ms(STM32_I2C_PACE_BEFORE_COUNTER_WRITE_MS);
				l4_i2c_dev_write_bytes(1,ram_addr, 2,&count1[0]);

				ram_addr = I2C_REG_CFG_ADDR;
				stm32_bus_copy_cfg_cache(&cfg_cache_local[0]);
				stm32_bus_pacing_delay_ms(STM32_I2C_PACE_BEFORE_CFG_WRITE_MS);
				l4_i2c_dev_write_bytes(1, ram_addr, STM32_CFG_CACHE_LEN, &cfg_cache_local[0]);
		//	gpio_intr_enable(EVENT_INT_PIN);
			}
		stm32_bus_pacing_delay_ms(1U);
		tca6408a_reset_out_bit(LED_BLUE);
		(void)stm32_bus_sync_interrupt_state();
	}
}
// === Wait for next INT (used by FW-update to confirm bus quiesce) ===
esp_err_t stm32_bus_wait_next_int(uint32_t timeout_ms) {
	/* Create semaphore lazily (persists for lifetime of the process) */
	if (s_int_notify_sem == NULL) {
		SemaphoreHandle_t sem = xSemaphoreCreateBinary();
		if (sem == NULL) {
			return ESP_ERR_NO_MEM;
		}
		s_int_notify_sem = sem;
	}
	/* Drain any signal that arrived before this call so we block
	 * until a FRESH interrupt fires. */
	(void)xSemaphoreTake(s_int_notify_sem, 0);
	/* Block until the next EVENT_INT_PIN edge or timeout. */
	if (xSemaphoreTake(s_int_notify_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
		return ESP_OK;
	}
	return ESP_ERR_TIMEOUT;
}

// === Stop Bus (graceful shutdown before restart) ===
esp_err_t stm32_bus_stop(void) {
	ESP_LOGW(TAG, "STM32 I2C bus stopping...");
	/* 1. Remove ISR so no new events are queued */
	gpio_isr_handler_remove(EVENT_INT_PIN);
	gpio_intr_disable(EVENT_INT_PIN);
	/* 2. Wait for any in-flight stm32_task I2C transaction to finish */
	vTaskDelay(pdMS_TO_TICKS(50));
	/* 3. Assert STM32 hardware reset — keeps NRST low until next boot */
	esp_err_t ret = tca6408a_stm32_reset_assert();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "STM32 reset assert during stop failed: %s",
				 esp_err_to_name(ret));
	} else {
		ESP_LOGI(TAG, "STM32 I2C bus stopped, STM32 held in reset");
	}
	return ret;
}

// === Start Bus ===
esp_err_t start_stm32_bus() {
	/* GPIO 34 is input-only on ESP32 and has no internal pull-up/pull-down.
	 * An external pull-up resistor must be present on the board. */
	const gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_NEGEDGE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = (1ULL << EVENT_INT_PIN),
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf);
	gpio_install_isr_service(0);

	xQueueSTM32PackedReceived = xQueueCreate(PACKET_QUEUE_LENGTH, sizeof(uint32_t));
	if (xQueueSTM32PackedReceived == NULL) {
		ESP_LOGE(TAG, "Failed to create STM32 packet queue");
		return ESP_ERR_NO_MEM;
	}
	(void)stm32_bus_reload_cfg_cache();
	l4_msg_get_time_age_sec_str = stm32_time_age_sec_str_getter;
	// Reset STM32
//	tca6408a_set_out_bit(LED_BLUE);
	esp_err_t rst_ret = tca6408a_stm32_reset_assert();
	if (rst_ret != ESP_OK) {
		ESP_LOGE(TAG, "STM32 reset assert failed: %s", esp_err_to_name(rst_ret));
		return rst_ret;
	}
	vTaskDelay(pdMS_TO_TICKS(15));
	rst_ret = tca6408a_stm32_reset_release();
	if (rst_ret != ESP_OK) {
		ESP_LOGE(TAG, "STM32 reset release failed: %s", esp_err_to_name(rst_ret));
		return rst_ret;
	}
	/* Allow STM32 to boot and initialise its I2C slave peripheral
	 * before registering the ISR — prevents premature bus access. */
	vTaskDelay(pdMS_TO_TICKS(50));
	gpio_isr_handler_add(EVENT_INT_PIN, gpio_isr_handler, (void *)EVENT_INT_PIN);
	xTaskCreate(stm32_task, "stm32_task", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
	stm32_bus_pacing_delay_ms(1U);
	(void)stm32_bus_sync_interrupt_state();
	rst_ret = tca6408a_set_out_bit(LED_RED);
	if (rst_ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to set RED_LED after STM32 start: %s", esp_err_to_name(rst_ret));
	}
	ESP_LOGI(TAG, "STM32 I2C bus started");
	return ESP_OK;
}
