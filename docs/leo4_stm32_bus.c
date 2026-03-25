/*
 * leo4_stm32_bus.c
 *
 *  Created on: 25 04 2025
 *      Author: oleg_
 */
#include "board_pins_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
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
#include "audio_mutex.h"
#include <time.h>
#include "l4_i2c_master.h"
#include "l4_stm32_bus.h"
// === Configuration ===
#define I2C_HEADER_LEN         (0x02U)
#define STM32_I2C_DEV_ADDR     (1)           // Logical device ID for l4_i2c_dev
#define PACKET_QUEUE_LENGTH    4
#define TASK_STACK_SIZE        4096
#define TASK_PRIORITY          10
#define ESP_INTR_FLAG_DEFAULT 0
#define BUS_STABILITY_DELAY 5
#include "l4_cfg_repo.h"

// EVENTS
ESP_EVENT_DEFINE_BASE(STM32_EVENT_BASE);
typedef enum {
	STM32_EVENT_I2C,
} stm32_event_id_t;
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
	PACKET_MAX
} I2cPacketType_t;
//
/* private config*/
static uint8_t cfg_stm32_0xE0_0xEF[16];

static const char *TAG = "STM32_bus";
static uint8_t count1[2] = {0, 3};
static uint8_t data[16] = {};
static QueueHandle_t xQueueSTM32PackedReceived;

#define TIME_SYNC_NTP_VALID_EPOCH       ((time_t)1704067200) /* 2024-01-01 00:00:00 local guard */
#define TIME_SYNC_FALLBACK_MIN_EPOCH    ((time_t)1773961200) /* 2026-03-20 00:00:00 local */
#define TIME_SYNC_MAX_AGE_SEC           5

static int32_t s_time_age_sec = -1;
static char s_time_age_sec_str[16] = "-1";

static uint8_t bcd_to_u8(const uint8_t bcd)
{
	return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0F));
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
//

// ISR handler
void IRAM_ATTR gpio_isr_handler(void *arg) {
	const uint32_t gpio_num = (uint32_t)arg;
	BaseType_t high_task_awoken = pdFALSE;
	if (gpio_num == EVENT_INT_PIN) {
	//	gpio_intr_disable(EVENT_INT_PIN);
		esp_err_t err = esp_event_isr_post(STM32_EVENT_BASE, STM32_EVENT_I2C, NULL, 0, &high_task_awoken);
		if (err != ESP_OK) {
			// Обработка ошибки (например, очередь переполнена)
		}
	}
	// Если высокоприоритетная задача была разбужена
	if (high_task_awoken == pdTRUE) {
		portYIELD_FROM_ISR();
	}
}

static void stm32_event_handler(void *handler_arg, esp_event_base_t base,
								int32_t id, void *event_data) {
	// Signal task that STM32 has data ready
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xQueueSendFromISR(xQueueSTM32PackedReceived, &id, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
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
	vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
	res = l4_i2c_dev_write_bytes(1,ram_addr, rmsg[1],&rmsg[2]);
//	res = i2c_master_transmit(stm32_dev_handle, rmsg, 7, 100);
	if (result.msg[0] > 0 && result.msg[0] < 16) {
		ram_addr = I2C_REG_HMI_MSG_ADDR;
		uint8_t hmi[32];
		hmi[0] = ram_addr;
		hmi[1] = result.msg[0] + 3;
		memcpy(&hmi[2], result.msg, result.msg[0] + 3);
		vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
		res |= l4_i2c_dev_write_bytes(1,ram_addr,hmi[1], &hmi[2]);
	}
	return res;
}
static void stm32_task(void *arg) {
	I2cPacketType_t ptype = PACKET_NULL;
	uint8_t dummy;
	uint8_t pckt_type = PACKET_NULL;
	uint8_t ram_addr = I2C_PACKET_TYPE_ADDR;

	while (1) {
		InputAuthResult_t auth_result = {
			.msg = {0}, .result = 0, .res_register_1 = 0};
		memset(data, 0, 16);
		xQueueReceive(xQueueSTM32PackedReceived, &dummy, portMAX_DELAY);
		tca6408a_set_out_bit(LED_BLUE);  // LED on during processing
		vTaskDelay(1 / portTICK_PERIOD_MS);
		memset(data, 0, sizeof(data));
		memset(&auth_result, 0, sizeof(auth_result));
		//vTaskDelay(10 / portTICK_PERIOD_MS);
		ram_addr = I2C_PACKET_TYPE_ADDR;
			if (l4_i2c_dev_read_bytes(1,ram_addr,1,&pckt_type)== ESP_OK) {
				vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
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
					input_dispatch(PN532_READER,
								   (const FrontendSourceInputLenght_t)data[0],
								   &data[1], &auth_result);
					if (stm32_act_result_write_bus(auth_result)!= ESP_OK) {
						ESP_LOGI(TAG, "FAIL 3");
					}
				}
			} else if (ptype == PACKET_PIN) {
				ram_addr = I2C_REG_MATRIX_PIN_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 13,data) == ESP_OK) {
					input_dispatch(MATRIX_KEYBOARD,
								   (const FrontendSourceInputLenght_t)data[1], &data[2],
								   &auth_result);
					stm32_act_result_write_bus(auth_result);
				}
			} else if (ptype == PACKET_WIEGAND) {
				ram_addr = I2C_REG_WIEGAND_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 15,data) == ESP_OK) {
					input_dispatch(
						WIEGAND_PIN_READER,
						(const FrontendSourceInputLenght_t)data[1], &data[2],
						&auth_result);
					stm32_act_result_write_bus(auth_result);
				}
			} else if (ptype == PACKET_PIN_HMI) {
				ram_addr = I2C_REG_HMI_PIN_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr, 15,data) == ESP_OK) {
					input_dispatch(TOUCH_KEYPAD,
								   (const FrontendSourceInputLenght_t)data[1],
								   &data[2], &auth_result);
					if (auth_result.msg[0] > 0 && auth_result.msg[0] < 17)
						stm32_act_result_write_bus(auth_result);
				}
			} else if (ptype == PACKET_TIME) {
				uint8_t time_packet[8] = {0};
				ram_addr = I2C_REG_HW_TIME_ADDR;
				if (l4_i2c_dev_read_bytes(1,ram_addr,8, time_packet) == ESP_OK) {
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
						vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
						l4_i2c_dev_write_bytes(1, I2C_REG_HW_TIME_ADDR + 8, 7, &time_bcd[0]);
					}
				}
			} else if (ptype == PACKET_ERROR) {
				ram_addr = I2C_REG_STM32_ERROR_ADDR;
				/*
				* TODO: error from stm32
				*/
			}
				ram_addr = I2C_REG_COUNTER_ADDR;
				vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
				l4_i2c_dev_write_bytes(1,ram_addr, 2,&count1[0]);

				ram_addr = I2C_REG_CFG_ADDR;
				memset(&cfg_stm32_0xE0_0xEF[0],0x00, 16);
				//l4_i2c_dev_write_bytes(1,ram_addr, 16,&cfg_stm32_0xE0_0xEF[0]);
				//cfg_stm32_0xE0_0xEF
				// read from NVS cfg_stm32
				get_nvs_cfg_stm32(&cfg_stm32_0xE0_0xEF[0]); // Заменить memset и заполнение на вызов функции
				// memset(&cfg_stm32_0xE0_0xEF[5],0xA5, 11);
				vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
				l4_i2c_dev_write_bytes(1, ram_addr, 16, &cfg_stm32_0xE0_0xEF[0]);
		//	gpio_intr_enable(EVENT_INT_PIN);
			}
		vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
		tca6408a_reset_out_bit(LED_BLUE);
	}
}
// === Start Bus ===
esp_err_t start_stm32_bus() {
	const gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_NEGEDGE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = (1ULL << EVENT_INT_PIN),
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf);
	gpio_install_isr_service(0);
	ESP_ERROR_CHECK(esp_event_handler_register(
		STM32_EVENT_BASE, STM32_EVENT_I2C, stm32_event_handler, NULL));

	xQueueSTM32PackedReceived = xQueueCreate(PACKET_QUEUE_LENGTH, sizeof(int32_t));
	if (xQueueSTM32PackedReceived == NULL) {
		ESP_LOGE(TAG, "Failed to create STM32 packet queue");
		return ESP_ERR_NO_MEM;
	}
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
//	vTaskDelay(pdMS_TO_TICKS(50));
	gpio_isr_handler_add(EVENT_INT_PIN, gpio_isr_handler, (void *)EVENT_INT_PIN);
	xTaskCreate(stm32_task, "stm32_task", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
	vTaskDelay(BUS_STABILITY_DELAY / portTICK_PERIOD_MS);
	rst_ret = tca6408a_set_out_bit(LED_RED);
	if (rst_ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to set RED_LED after STM32 start: %s", esp_err_to_name(rst_ret));
	}
	ESP_LOGI(TAG, "STM32 I2C bus started");
	return ESP_OK;
}
