/*
 * l4_input_processing.c
 *
 *  Created on: 24 нояб. 2025 г.
 *      Author: oleg_
 */
#include "audio_player_int_tone.h"
#include "audio_tone_uri.h"
#include "esp_log.h"
//#include "freertos/projdefs.h"
#include "l4_cloud_codec.h"
#include "l4_event_router.h"
#include "l4_frontend.h"
#include "leo4_nvs.h"
#include "l4_cfg_repo.h"
#include "l4_sip_control.h"
#include "l4_stm32_bus.h"
#include "l4_input_processing_test_hooks.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern bool audio_session;

static const char *TAG = "L4_INPUT_PROCESSING";
static FrotnendInput_t current_input;
static char map_data[16] = {0};

#define INPUT_HMI_TEXT_LEN 10
#define INPUT_RESULT_HEADER_LEN 3
#define FRONTEND_INPUT_QUEUE_LEN 16
#define FRONTEND_INPUT_MAX_LEN 16
#define FRONTEND_INPUT_WORKER_STACK 6144
#define FRONTEND_INPUT_WORKER_PRIO 8

typedef struct {
	FrontendSource_t source;
	FrontendSourceInputLenght_t length;
	uint8_t data[FRONTEND_INPUT_MAX_LEN];
} FrontendQueuedInput_t;

static QueueHandle_t s_frontend_input_queue = NULL;
static bool s_frontend_worker_started = false;

static void frontend_input_worker_task(void *arg);
static void frontend_process_input(const FrontendSource_t source,
					const FrontendSourceInputLenght_t length,
					const uint8_t *data,
					InputAuthResult_t *input_dispatch_result);

/*
 * frontend_list.nvs_key - cast reflections from enum FrontendSource_t
 * */
static FrontendCfg_t frontend_list[] = {
	{WIEGAND_READER, "wiegand", NULL},
	{WIEGAND_PIN_READER, "wiegand_pin", NULL},
	{I2C_USER_ID_READER, "i2c_reader", NULL},
	{TOUCH_KEYPAD, "touch_keypad", NULL},
	{API_CODE_INPUT_READER, "api_code_input", NULL},
	{PN532_READER, "pn532", NULL},
	{MATRIX_KEYBOARD, "matrix_key", NULL},
	{DTMF_ID, "dtmf_id", NULL},
	{GM810_READER, "gm810", NULL}
};

static size_t frontend_list_count(void) {
	return sizeof(frontend_list) / sizeof(frontend_list[0]);
}

static bool frontend_source_is_text_input(const FrontendSource_t source) {
	return (source == TOUCH_KEYPAD)
			|| (source == API_CODE_INPUT_READER)
			|| (source == DTMF_ID)
			|| (source == GM810_READER);
}

static bool frontend_flag_enabled(const struct FrontendSettings_s *settings,
								  const FrontendFlags_t flag) {
	return (settings != NULL) && ((settings->flags & flag) != 0);
}

static void reset_input_result(InputAuthResult_t *result) {
	if (result == NULL) {
		return;
	}
	result->result = AUTH_RESULT_FAIL;
	result->res_register_1 = 0;
	result->msg[0] = 0;
	memset(result->msg + 1, 0xFF, sizeof(result->msg) - 1);
}

static void set_result_display(InputAuthResult_t *result, const char *text) {
	if (result == NULL) {
		return;
	}
	result->msg[0] = INPUT_HMI_TEXT_LEN + 1;
	result->msg[1] = 5;
	result->msg[2] = 1;
	memset(result->msg + INPUT_RESULT_HEADER_LEN, ' ', INPUT_HMI_TEXT_LEN);
	if (text != NULL) {
		size_t copy_len = strlen(text);
		if (copy_len > INPUT_HMI_TEXT_LEN) {
			copy_len = INPUT_HMI_TEXT_LEN;
		}
		memcpy(result->msg + INPUT_RESULT_HEADER_LEN, text, copy_len);
	}
	memset(result->msg + INPUT_RESULT_HEADER_LEN + INPUT_HMI_TEXT_LEN,
		   0xFF,
		   sizeof(result->msg) - INPUT_RESULT_HEADER_LEN - INPUT_HMI_TEXT_LEN);
}

static void fill_busy_result(InputAuthResult_t *result) {
	reset_input_result(result);
	result->result = AUTH_RESULT_BUSY;
	set_result_display(result, "-- BUSY --");
}

static void fill_open_door_result(InputAuthResult_t *result) {
	result->result = AUTH_RESULT_SUCCESS;
	result->res_register_1 = 0x00010000;
	set_result_display(result, "-- OTKP --");
}

static void fill_open_cell_result(InputAuthResult_t *result,
						  const uint32_t cell_params) {
	result->result = AUTH_RESULT_SUCCESS;
	result->res_register_1 = cell_params;
	set_result_display(result, "CELL -----");
}

static void fill_no_auth_result(InputAuthResult_t *result) {
	reset_input_result(result);
	set_result_display(result, "NO AUTH");
}

static void clear_current_input(void) {
	current_input.source = NULLREADER;
	current_input.length = ID_LEN_0;
	memset(current_input.input, 0, sizeof(current_input.input));
}

static void resolve_frontend_settings(const FrontendSource_t source,
						  struct FrontendSettings_s *settings) {
	if (settings == NULL) {
		return;
	}
	memset(settings, 0, sizeof(*settings));
	for (size_t i = 0; i < frontend_list_count(); i++) {
		if (frontend_list[i].source == source) {
			if (frontend_list[i].settings != NULL) {
				*settings = *frontend_list[i].settings;
			}
			return;
		}
	}
}

static void normalize_text_input(const FrontendSourceInputLenght_t length,
						 const uint8_t *data,
						 char *output,
						 const size_t output_size) {
	size_t copy_len = (size_t) length;
	const size_t max_copy = output_size - 1;
	if (copy_len > max_copy) {
		ESP_LOGW(TAG, "input_dispatch: text input truncated from %u to %u",
				 (unsigned) copy_len, (unsigned) max_copy);
		copy_len = max_copy;
	}
	if (copy_len > 0) {
		memcpy(output, data, copy_len);
	}
	output[copy_len] = 0;
}

static void normalize_hex_input(const FrontendSourceInputLenght_t length,
						const uint8_t *data,
						char *output,
						const size_t output_size) {
	size_t src_len = (size_t) length;
	const size_t max_src_len = (output_size - 1) / 2;
	size_t out = 0;
	if (src_len > max_src_len) {
		ESP_LOGW(TAG, "input_dispatch: hex input truncated from %u to %u bytes",
				 (unsigned) src_len, (unsigned) max_src_len);
		src_len = max_src_len;
	}
	for (size_t i = 0; i < src_len; i++) {
		if ((data[i] == 0x23) || (data[i] == 0)) {
			break;
		}
		if (snprintf(&output[out], output_size - out, "%02X", data[i]) != 2) {
			break;
		}
		out += 2;
	}
	output[out] = 0;
}

static void normalize_input(const FrontendSource_t source,
					const FrontendSourceInputLenght_t length,
					const uint8_t *data,
					char *output,
					const size_t output_size) {
	if (frontend_source_is_text_input(source)) {
		normalize_text_input(length, data, output, output_size);
	} else {
		normalize_hex_input(length, data, output, output_size);
	}
}

static void fill_input_event(MsgEvent_t *event,
				 const FrontendSource_t source,
				 const FrontendSourceInputLenght_t length,
				 const char *input_value) {
	if (event == NULL) {
		return;
	}
	*event = (MsgEvent_t ) { .msg_event_type = ID_INPUT_EVENT,
			.msg_event_payload = { .input_context = { .user_id_type = source,
					.user_id_lenght = length } } };
	snprintf(event->msg_event_payload.input_context.key_id_str,
			 sizeof(event->msg_event_payload.input_context.key_id_str), "%s",
			 (input_value != NULL) ? input_value : "");
}

static esp_err_t lookup_admin_script(const struct FrontendSettings_s *settings,
					     const char *input_value,
					     char *admin_script,
					     const size_t admin_script_size) {
	if ((settings == NULL) || (input_value == NULL) || (admin_script == NULL)
			|| (admin_script_size == 0)) {
		return ESP_ERR_INVALID_ARG;
	}
	admin_script[0] = '\0';
	if ((!frontend_flag_enabled(settings, FRONTEND_FLAG_AUTH_ADMIN))
			|| (input_value[0] == '\0')) {
		return ESP_ERR_NOT_FOUND;
	}
	esp_err_t ret = get_str_from_db("auth_admin", input_value, admin_script,
							 admin_script_size);
	if ((ret == ESP_OK) && (admin_script[0] != '\0')) {
		return ESP_OK;
	}
	if (strcmp(input_value, "993301") == 0) {
		snprintf(admin_script, admin_script_size, "%s", "display_ip");
		return ESP_OK;
	}
	return ret;
}

static void execute_admin_script(const char *admin_script,
					 InputAuthResult_t *result,
					 InputContext_t *input_context) {
	if ((admin_script == NULL) || (input_context == NULL)) {
		return;
	}
	input_context->user_id_is_admin = true;
	if (strcmp(admin_script, "display_ip") == 0) {
		const char *ip = "";
		char ip_tail[INPUT_HMI_TEXT_LEN + 1] = "----------";
		char **ip_ptr = (l4_msg_get_ip != NULL) ? l4_msg_get_ip() : NULL;
		if ((ip_ptr != NULL) && (*ip_ptr != NULL)) {
			ip = *ip_ptr;
		}
		size_t ip_len = strlen(ip);
		if (ip_len >= INPUT_HMI_TEXT_LEN) {
			memcpy(ip_tail, ip + ip_len - INPUT_HMI_TEXT_LEN,
				   INPUT_HMI_TEXT_LEN);
		} else if (ip_len > 0) {
			memcpy(ip_tail + (INPUT_HMI_TEXT_LEN - ip_len), ip, ip_len);
		}
		set_result_display(result, ip_tail);
		return;
	}
	ESP_LOGW(TAG, "Unknown admin script: %s", admin_script);
	set_result_display(result, "ADMIN CMD!");
}

static esp_err_t lookup_auth_value(const struct FrontendSettings_s *settings,
					   const char *input_value,
					   char *auth_value,
					   const size_t auth_value_size) {
	if ((settings == NULL) || (input_value == NULL) || (auth_value == NULL)
			|| (auth_value_size == 0)) {
		return ESP_ERR_INVALID_ARG;
	}
	auth_value[0] = '\0';
	if ((!frontend_flag_enabled(settings, FRONTEND_FLAG_AUTH_DB))
			|| (settings->auth_ns[0] == '\0')) {
		return ESP_ERR_NOT_FOUND;
	}
	return get_str_from_db(settings->auth_ns, input_value, auth_value,
					  auth_value_size);
}

static void execute_user_script(const struct FrontendSettings_s *settings,
					const char *input_value,
					const char *auth_value,
					InputAuthResult_t *result,
					InputContext_t *input_context) {
	if ((settings == NULL) || (input_value == NULL) || (auth_value == NULL)
			|| (result == NULL) || (input_context == NULL)) {
		return;
	}
	if (strcmp(settings->script, "free_pass") == 0) {
		input_context->user_id_is_valid = true;
		fill_open_door_result(result);
		if ((settings->auth_ns[0] != '\0') && (input_value[0] != '\0')) {
			esp_err_t write_ret = set_nvs_record_str(L4_NVS_PART_DB,
											 settings->auth_ns,
											 input_value,
											 input_value);
			if (write_ret != ESP_OK) {
				ESP_LOGW(TAG, "free_pass cache write failed ns=%s key=%s err=%s",
						 settings->auth_ns,
						 input_value,
						 esp_err_to_name(write_ret));
			}
		}
		return;
	}
	if (strcmp(settings->script, "open_door") == 0) {
		input_context->user_id_is_valid = true;
		fill_open_door_result(result);
		return;
	}
	if (strcmp(settings->script, "open_cell") == 0) {
		uint32_t cell_params = 0;
		if (get_u32_from_db("cell_map", auth_value, &cell_params) == ESP_OK) {
			unsigned long parsed_cell = strtoul(auth_value, NULL, 10);
			input_context->user_id_is_valid = true;
			if (parsed_cell <= UINT16_MAX) {
				input_context->binded_cell_number = (uint16_t) parsed_cell;
			}
			fill_open_cell_result(result, cell_params);
		} else {
			input_context->user_id_is_valid_unbinded = true;
		}
		return;
	}
	if (strcmp(settings->script, "sip_no_map") == 0) {
		input_context->user_id_is_valid = true;
		if (!audio_session) {
			leo4_call(input_value);
		}
		return;
	}
	if (strcmp(settings->script, "sip_map") == 0) {
		if (auth_value[0] == '\0') {
			fill_no_auth_result(result);
			return;
		}
		input_context->user_id_is_valid = true;
		if (!audio_session) {
			leo4_call(auth_value);
			/* HMI display is now driven by SIP call state on every TIME tick */
		}
		return;
	}
	if (strcmp(settings->script, "no_auth") == 0) {
		fill_no_auth_result(result);
		return;
	}

	ESP_LOGW(TAG, "Unknown script '%s' -> no_auth", settings->script);
	fill_no_auth_result(result);
}

void frontend_processing_init() {
	current_input.mutex = xSemaphoreCreateMutex();
	clear_current_input();
	get_nvs_cfg_frontend(frontend_list, frontend_list_count());

	if (s_frontend_input_queue == NULL) {
		s_frontend_input_queue = xQueueCreate(FRONTEND_INPUT_QUEUE_LEN,
									 sizeof(FrontendQueuedInput_t));
		if (s_frontend_input_queue == NULL) {
			ESP_LOGE(TAG, "Failed to create frontend input queue");
			return;
		}
	}
	if (!s_frontend_worker_started) {
		BaseType_t task_ok = xTaskCreate(frontend_input_worker_task,
										 "frontend_input_worker",
										 FRONTEND_INPUT_WORKER_STACK,
										 NULL,
										 FRONTEND_INPUT_WORKER_PRIO,
										 NULL);
		if (task_ok != pdPASS) {
			ESP_LOGE(TAG, "Failed to create frontend input worker task");
			return;
		}
		s_frontend_worker_started = true;
	}
}

#if CONFIG_L4_ENABLE_DTMF_SMOKE_TEST
static uint32_t s_dtmf_postprocess_calls = 0;
static InputAuthResult_t s_last_dtmf_result = {0};
#endif

static void postprocess_dtmf_result(const InputAuthResult_t *result) {
	if (result != NULL) {
#if CONFIG_L4_ENABLE_DTMF_SMOKE_TEST
		s_dtmf_postprocess_calls++;
		s_last_dtmf_result = *result;
#endif
		if ((result->msg[0] > 0) && (result->msg[0] < 17)) {
			if (stm32_act_result_write_bus(*result) != ESP_OK) {
				ESP_LOGW(TAG, "DTMF postprocess: failed to write result to STM32 bus");
			}
		}
	}
	leo4_hangup();
}

static void postprocess_frontend_result(const FrontendSource_t source,
						const InputAuthResult_t *result) {
	if (result == NULL) {
		return;
	}

	if (source == DTMF_ID) {
		postprocess_dtmf_result(result);
		return;
	}

	if ((source == PN532_READER) || (source == MATRIX_KEYBOARD)
			|| (source == GM810_READER)
			|| (source == WIEGAND_PIN_READER)) {
		if (stm32_act_result_write_bus(*result) != ESP_OK) {
			ESP_LOGW(TAG, "postprocess: failed to write result to STM32 bus");
		}
		return;
	}

	if (source == TOUCH_KEYPAD) {
		if ((result->msg[0] > 0) && (result->msg[0] < 17)) {
			if (stm32_act_result_write_bus(*result) != ESP_OK) {
				ESP_LOGW(TAG, "postprocess: failed to write HMI result to STM32 bus");
			}
		}
	}
}

#if CONFIG_L4_ENABLE_DTMF_SMOKE_TEST
void l4_input_processing_reset_dtmf_smoke_counters(void) {
	s_dtmf_postprocess_calls = 0;
	memset(&s_last_dtmf_result, 0, sizeof(s_last_dtmf_result));
}

uint32_t l4_input_processing_get_dtmf_postprocess_calls(void) {
	return s_dtmf_postprocess_calls;
}

bool l4_input_processing_get_last_dtmf_result(InputAuthResult_t *out_result) {
	if (out_result == NULL) {
		return false;
	}
	*out_result = s_last_dtmf_result;
	return true;
}
#endif

static void frontend_process_input(const FrontendSource_t source,
					const FrontendSourceInputLenght_t length,
					const uint8_t *data,
					InputAuthResult_t *input_dispatch_result) {
	struct FrontendSettings_s settings = {0};
	MsgEvent_t event_input = {0};
	char admin_script[16] = {0};
	char *ptr_data = map_data;
	esp_err_t ret = ESP_ERR_NOT_FOUND;
	bool locked = false;
	InputContext_t *input_context = NULL;

	if (input_dispatch_result == NULL) {
		ESP_LOGW(TAG, "frontend_process_input: input_dispatch_result is NULL");
		return;
	}
	reset_input_result(input_dispatch_result);

	if ((data == NULL) && (length > 0)) {
		ESP_LOGW(TAG, "frontend_process_input: data is NULL, length=%u", (unsigned) length);
		return;
	}
	if (current_input.mutex == NULL) {
		ESP_LOGW(TAG, "frontend_process_input: mutex is not initialized");
		return;
	}

	if (xSemaphoreTake(current_input.mutex, 0) != pdTRUE) {
		ESP_LOGW(TAG, "frontend_process_input: busy, ignore new input source=%d", source);
		fill_busy_result(input_dispatch_result);
		return;
	}
	locked = true;

	current_input.source = source;
	current_input.length = length;
	memset(current_input.input, 0, sizeof(current_input.input));
	normalize_input(source, length, data, current_input.input,
				sizeof(current_input.input));
	ESP_LOGI(TAG,
			 "pin, card, wiegand... source= %d, length=%d, payload= %s",
			 source, length, current_input.input);

	fill_input_event(&event_input, source, length, current_input.input);
	input_context = &event_input.msg_event_payload.input_context;
	resolve_frontend_settings(source, &settings);

	/*
	 * settings examples, format string (json-like) = :
	 * {"auth_ns":"auth_sip", "flags":124, "script":"sip_map"}
	 * {"auth_ns":"auth_vend", "flags":124, "script":"cell_sku_data"}
	 * {"auth_ns":"auth_lockers", "flags":124, "script":"sip_no_map"}
	 *-> settings.auth_ns = "auth_lockers";
	 *-> settings.script = "sip_no_map";
	 *-> settings.flags=124;
	 * example flags 124 = 0x7C -> translate event to mqtt, ws,udp; search by
	 * auth_admin; search by auth_... (auth_... by frontend_list[x].settings.auth_ns) -
	 * =|0|UDP|WS|MQTT|auth_admin|auth_db|0|0|= 0x00 - 0x7C
	 * auth_admin - flag for activate search by ns=auth_admin: if ns=auth_admin.nvs-key==current_input.input exist, get str value and
	 * использовать это value as admin_script - заранее определенная функция-процессинг административных команд
	 * если не найдено в auth_admin, то искать в auth_ns, если найдено, то выполнить скрипт, который прописан в settings.script для этого источника
	 */
	if (lookup_admin_script(&settings, current_input.input, admin_script,
						 sizeof(admin_script)) == ESP_OK) {
		execute_admin_script(admin_script, input_dispatch_result, input_context);
		goto finalize;
	}

	map_data[0] = '\0';
	if (strcmp(settings.script, "free_pass") == 0) {
		execute_user_script(&settings, current_input.input, current_input.input,
					   input_dispatch_result, input_context);
		goto finalize;
	}

	ret = lookup_auth_value(&settings, current_input.input, ptr_data,
				      sizeof(map_data));
	ESP_LOGI(TAG, "NS = %s, script = %s, data = %s", settings.auth_ns,
			 settings.script, ptr_data);
	if (ret == ESP_OK) {
		execute_user_script(&settings, current_input.input, ptr_data,
					   input_dispatch_result, input_context);
	} else {
		if (settings.fallback[0] != '\0') {
			struct FrontendSettings_s fallback_settings = settings;
			snprintf(fallback_settings.script,
					 sizeof(fallback_settings.script),
					 "%s",
					 settings.fallback);
			execute_user_script(&fallback_settings,
							current_input.input,
							current_input.input,
							input_dispatch_result,
							input_context);
		} else {
			struct FrontendSettings_s no_auth_settings = settings;
			snprintf(no_auth_settings.script,
					 sizeof(no_auth_settings.script),
					 "%s",
					 "no_auth");
			execute_user_script(&no_auth_settings,
							current_input.input,
							current_input.input,
							input_dispatch_result,
							input_context);
		}
	}

finalize:
	send_event_router(&event_input, settings);
	postprocess_frontend_result(source, input_dispatch_result);
	clear_current_input();
	if (locked) {
		xSemaphoreGive(current_input.mutex);
	}
	ESP_LOGI(
		TAG,
		"***************  END input "
		"processing--------------------------****************************");
}

static void frontend_input_worker_task(void *arg) {
	(void)arg;
	FrontendQueuedInput_t item = {0};
	for (;;) {
		if (xQueueReceive(s_frontend_input_queue, &item, portMAX_DELAY) == pdTRUE) {
			InputAuthResult_t result = {0};
			frontend_process_input(item.source, item.length, item.data, &result);
		}
	}
}

esp_err_t frontend_input_enqueue(const FrontendSource_t source,
							 const FrontendSourceInputLenght_t length,
							 const uint8_t *data) {
	if ((length > 0) && (data == NULL)) {
		return ESP_ERR_INVALID_ARG;
	}
	if ((size_t)length > FRONTEND_INPUT_MAX_LEN) {
		ESP_LOGW(TAG, "frontend_input_enqueue: input too large source=%d len=%u", source, (unsigned)length);
		return ESP_ERR_INVALID_SIZE;
	}
	if (s_frontend_input_queue == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if ((current_input.mutex != NULL)
			&& (xSemaphoreTake(current_input.mutex, 0) != pdTRUE)) {
		ESP_LOGW(TAG, "frontend_input_enqueue: busy session source=%d", source);
		return ESP_ERR_TIMEOUT;
	}
	if (current_input.mutex != NULL) {
		xSemaphoreGive(current_input.mutex);
	}
	if (uxQueueMessagesWaiting(s_frontend_input_queue) > 0) {
		ESP_LOGW(TAG, "frontend_input_enqueue: pending input exists, drop source=%d", source);
		return ESP_ERR_TIMEOUT;
	}

	FrontendQueuedInput_t item = {
		.source = source,
		.length = length,
		.data = {0},
	};
	if ((length > 0) && (data != NULL)) {
		memcpy(item.data, data, (size_t)length);
	}

	if (xQueueSend(s_frontend_input_queue, &item, 0) != pdTRUE) {
		ESP_LOGW(TAG, "frontend_input_enqueue: queue full source=%d", source);
		return ESP_ERR_TIMEOUT;
	}
	return ESP_OK;
}
