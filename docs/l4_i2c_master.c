//
// Created by oleg_ on 18.02.2026.
// Optimized version with safety, readability and performance improvements
//

#include "../include/l4_i2c_master.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "l4_def.h"

static const char *TAG = "l4_I2C_BOARD";

// Configuration constants
#define I2C_MUTEX_TIMEOUT_MS      1000
#define I2C_TRANSMIT_TIMEOUT_MS   1000
#define I2C_RECEIVE_TIMEOUT_MS    200

// Bus and device handles
static i2c_master_bus_handle_t l4_bus_handle = NULL;
static i2c_master_dev_handle_t l4_i2c_dev_handle[4] = {NULL};

// Global mutex for bus access
static SemaphoreHandle_t i2c_mutex = NULL;

// Flag to ensure one-time initialization
static bool i2c_initialized = false;
static bool i2c_init_in_progress = false;
static portMUX_TYPE i2c_init_lock = portMUX_INITIALIZER_UNLOCKED;

// Forward declarations
static esp_err_t i2c_init_once(void);

/**
 * @brief Initialize I2C master bus and devices (idempotent)
 */
static esp_err_t i2c_init_once(void) {
    while (1) {
        taskENTER_CRITICAL(&i2c_init_lock);
        if (i2c_initialized) {
            taskEXIT_CRITICAL(&i2c_init_lock);
            return ESP_OK;
        }
        if (!i2c_init_in_progress) {
            i2c_init_in_progress = true;
            taskEXIT_CRITICAL(&i2c_init_lock);
            break;
        }
        taskEXIT_CRITICAL(&i2c_init_lock);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Create mutex first
    i2c_mutex = xSemaphoreCreateMutex();
    if (!i2c_mutex) {
        ESP_LOGE(TAG, "Failed to create I2C mutex");
        taskENTER_CRITICAL(&i2c_init_lock);
        i2c_init_in_progress = false;
        taskEXIT_CRITICAL(&i2c_init_lock);
        return ESP_ERR_NO_MEM;
    }

    // Initialize I2C bus
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_1,
        .sda_io_num = L4_I2C_SDA_GPIO,  // Use defines from l4_def.h
        .scl_io_num = L4_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &l4_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Device configuration
    const i2c_device_config_t tca6408a_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA6408A_ADDR >> 1,
        .scl_speed_hz = 400000,
        .scl_wait_us = 50000,
    };

    const i2c_device_config_t stm32_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x11,
        .scl_speed_hz = 400000,
        .scl_wait_us = 50000,
    };

    const i2c_device_config_t es8311_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x30 >> 1,
        .scl_speed_hz = 100000,
        .scl_wait_us = 50000,
    };

    const i2c_device_config_t es7243x_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20 >> 1,  // 7243E/L
        .scl_speed_hz = 100000,
        .scl_wait_us = 50000,
    };

    // Add devices
    ret = i2c_master_bus_add_device(l4_bus_handle, &tca6408a_cfg, &l4_i2c_dev_handle[0]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TCA6408A device: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = i2c_master_bus_add_device(l4_bus_handle, &stm32_cfg, &l4_i2c_dev_handle[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add STM32 device: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = i2c_master_bus_add_device(l4_bus_handle, &es8311_cfg, &l4_i2c_dev_handle[2]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 device: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = i2c_master_bus_add_device(l4_bus_handle, &es7243x_cfg, &l4_i2c_dev_handle[3]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES7243X device: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    i2c_initialized = true;
    taskENTER_CRITICAL(&i2c_init_lock);
    i2c_init_in_progress = false;
    taskEXIT_CRITICAL(&i2c_init_lock);
    ESP_LOGI(TAG, "I2C bus initialized successfully");
    return ESP_OK;

cleanup:
    // Clean up on error
    if (l4_bus_handle) {
        i2c_del_master_bus(l4_bus_handle);
        l4_bus_handle = NULL;
    }
    if (i2c_mutex) {
        vSemaphoreDelete(i2c_mutex);
        i2c_mutex = NULL;
    }
    taskENTER_CRITICAL(&i2c_init_lock);
    i2c_init_in_progress = false;
    taskEXIT_CRITICAL(&i2c_init_lock);
    return ret;
}

/**
 * @brief Write a single register on an I2C device
 */
esp_err_t l4_i2c_dev_write_reg(const uint8_t dev_num, const uint8_t reg_addr, const uint8_t data) {
    if (dev_num >= 4 || l4_i2c_dev_handle[dev_num] == NULL) {
        ESP_LOGE(TAG, "Invalid device number or unconfigured device: %d", dev_num);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {reg_addr, data};

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (write_reg)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_transmit(l4_i2c_dev_handle[dev_num], buf, 2, pdMS_TO_TICKS(I2C_TRANSMIT_TIMEOUT_MS));
    xSemaphoreGive(i2c_mutex);

    if (res == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Master transmit timeout (device %d, reg 0x%02x)", dev_num, reg_addr);
    } else if (res != ESP_OK) {
        ESP_LOGE(TAG, "Write reg failed: %s", esp_err_to_name(res));
    }

    return res;
}

/**
 * @brief Read a single register from an I2C device
 */
esp_err_t l4_i2c_dev_read_reg(const uint8_t dev_num, const uint8_t reg_addr, uint8_t *data) {
    if (dev_num >= 4 || l4_i2c_dev_handle[dev_num] == NULL) {
        ESP_LOGE(TAG, "Invalid device number or unconfigured device: %d", dev_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (read_reg)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_transmit_receive(l4_i2c_dev_handle[dev_num], &reg_addr, 1, data, 1, pdMS_TO_TICKS(I2C_RECEIVE_TIMEOUT_MS));
    xSemaphoreGive(i2c_mutex);

    if (res == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Master receive timeout (device %d, reg 0x%02x)", dev_num, reg_addr);
    } else if (res != ESP_OK) {
        ESP_LOGE(TAG, "Read reg failed: %s", esp_err_to_name(res));
    }

    return res;
}

/**
 * @brief Get device handle by index
 */
esp_err_t get_i2c_dev_handler(const uint8_t dev_num, i2c_master_dev_handle_t *ret_dev_handler) {
    if (!ret_dev_handler) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev_num >= 4 || l4_i2c_dev_handle[dev_num] == NULL) {
        ESP_LOGE(TAG, "Attempt to get invalid device handler: %d", dev_num);
        return ESP_ERR_INVALID_ARG;
    }

    *ret_dev_handler = l4_i2c_dev_handle[dev_num];
    return ESP_OK;
}

/**
 * @brief Read multiple bytes from a register
 */
esp_err_t l4_i2c_dev_read_bytes(const uint8_t dev_num, const uint8_t reg_addr, const uint8_t data_len, uint8_t *data) {
    if (dev_num >= 4 || l4_i2c_dev_handle[dev_num] == NULL) {
        ESP_LOGE(TAG, "Invalid device number: %d", dev_num);
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len == 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[2] = {reg_addr, data_len};

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (read_bytes)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_transmit_receive(l4_i2c_dev_handle[dev_num], header, 2, data, data_len, pdMS_TO_TICKS(I2C_RECEIVE_TIMEOUT_MS));
    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Read bytes failed: %s", esp_err_to_name(res));
    }

    return res;
}

/**
 * @brief Write multiple bytes to a register
 * Uses stack allocation to avoid malloc/free in RTOS context
 */
esp_err_t l4_i2c_dev_write_bytes(const uint8_t dev_num, const uint8_t reg_addr, const uint8_t data_len, const uint8_t *data) {
    if (dev_num >= 4 || l4_i2c_dev_handle[dev_num] == NULL) {
        ESP_LOGE(TAG, "Invalid device number: %d", dev_num);
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len == 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Stack allocation — safe for small data (max ~100 bytes)
    uint8_t buf[data_len + 2];
    buf[0] = reg_addr;
    buf[1] = data_len;
    memcpy(buf + 2, data, data_len);

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (write_bytes)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_transmit(l4_i2c_dev_handle[dev_num], buf, data_len + 2, pdMS_TO_TICKS(I2C_TRANSMIT_TIMEOUT_MS));
    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Write bytes failed: %s", esp_err_to_name(res));
    }

    return res;
}

/**
 * @brief Start I2C subsystem
 */
esp_err_t l4_i2c_board_start(void) {
    esp_err_t ret = i2c_init_once();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed");
        return ret;
    }

    ESP_LOGI(TAG, "_-_-_-_-_- I2C bus init complete");
    return ESP_OK;
}

/**
 * @brief Optional: Deinitialize I2C bus and free resources
 */
void l4_i2c_board_deinit(void) {
    if (!i2c_initialized) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (l4_i2c_dev_handle[i]) {
            i2c_master_bus_rm_device(l4_i2c_dev_handle[i]);
            l4_i2c_dev_handle[i] = NULL;
        }
    }

    if (l4_bus_handle) {
        i2c_del_master_bus(l4_bus_handle);
        l4_bus_handle = NULL;
    }

    if (i2c_mutex) {
        vSemaphoreDelete(i2c_mutex);
        i2c_mutex = NULL;
    }

    i2c_initialized = false;
    taskENTER_CRITICAL(&i2c_init_lock);
    i2c_init_in_progress = false;
    taskEXIT_CRITICAL(&i2c_init_lock);
    ESP_LOGI(TAG, "I2C bus deinitialized");
}