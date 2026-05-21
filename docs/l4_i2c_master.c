//
// Created by oleg_ on 18.02.2026.
// Optimized version with safety, readability and performance improvements
//

#include "../include/l4_i2c_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "l4_def.h"
#include "l4_stm32_fw.h"
#include "soc/i2c_reg.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "l4_I2C_BOARD";

static volatile bool s_i2c_timeout_cb_fired = false;
static volatile int64_t s_i2c_timeout_cb_time_us = 0;
static volatile bool s_bl_diag_quiet_success = false;

static void bl_timeout_diag_reset(void)
{
    s_i2c_timeout_cb_fired = false;
    s_i2c_timeout_cb_time_us = 0;
}

static void bl_timeout_diag_log(const char *op, int64_t t0, esp_err_t res)
{
    const int scl = gpio_get_level(L4_I2C_SCL_GPIO);
    const int sda = gpio_get_level(L4_I2C_SDA_GPIO);
    const uint32_t to_reg = REG_READ(I2C_TO_REG(1));
    const uint32_t int_ena = REG_READ(I2C_INT_ENA_REG(1));

    /* Successful bootloader I2C operations are the normal path, whether or not
     * the timeout-suppression callback fired. Keep diagnostics only for actual
     * I2C errors/timeouts; success-path detail is logged by higher STM32 FW
     * code when needed. */
    if (res == ESP_OK) {
        return;
    }

    if (s_i2c_timeout_cb_fired) {
        ESP_LOGI(TAG,
                 "%s diag: cb_fired=yes cb_after=%lld us scl=%d sda=%d to_reg=0x%08"PRIx32" int_ena=0x%08"PRIx32" ret=%s",
                 op,
                 (long long)(s_i2c_timeout_cb_time_us - t0),
                 scl, sda, to_reg, int_ena, esp_err_to_name(res));
    } else {
        ESP_LOGW(TAG,
                 "%s diag: cb_fired=NO scl=%d sda=%d to_reg=0x%08"PRIx32" int_ena=0x%08"PRIx32" ret=%s",
                 op,
                 scl, sda, to_reg, int_ena, esp_err_to_name(res));
    }
}

void l4_i2c_stm32_bl_set_diag_quiet_success(bool quiet)
{
    s_bl_diag_quiet_success = quiet;
}

// Configuration constants
#define I2C_MUTEX_TIMEOUT_MS      1000
#define I2C_TRANSMIT_TIMEOUT_MS   1000
#define I2C_RECEIVE_TIMEOUT_MS    200

// Bus and device handles
static i2c_master_bus_handle_t l4_bus_handle = NULL;
static i2c_master_dev_handle_t l4_i2c_dev_handle[4] = {NULL};
static i2c_master_dev_handle_t l4_stm32_bl_dev_handle = NULL;
static uint8_t l4_stm32_bl_dev_addr = 0;

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
        .scl_wait_us = 0,
    };

    const i2c_device_config_t stm32_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x11,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
    };

    const i2c_device_config_t es8311_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x30 >> 1,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    const i2c_device_config_t es7243x_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20 >> 1,  // 7243E/L
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
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
 * @brief Get the I2C master bus handle (for adding transient devices)
 */
esp_err_t l4_i2c_get_bus_handle(i2c_master_bus_handle_t *out_handle) {
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_initialized || l4_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    *out_handle = l4_bus_handle;
    return ESP_OK;
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
 * @brief Perform I2C bus reset to release a stuck slave.
 *
 * Call this after l4_i2c_board_start() and before communicating with any
 * device when the bus may have been left in an undefined state (e.g. a slave
 * such as STM32 was stopped mid-transaction in a debug-halt).
 * The call is best-effort: a non-OK result is logged but not fatal.
 *
 * Note: ESP-IDF 5.5.2 exposes i2c_master_bus_reset() — there is no
 * i2c_master_bus_recover() in this SDK version.
 */
esp_err_t l4_i2c_bus_recover(void) {
    if (!i2c_initialized || l4_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized, cannot recover");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_master_bus_reset(l4_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset returned: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "I2C bus reset completed");
    }
    return ret;
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


/**
 * @brief Full I2C bus teardown and rebuild.
 *
 * This is the nuclear recovery for the ESP32-specific GPIO corruption bug:
 * when an I2C hardware timeout triggers s_i2c_hw_fsm_reset(true), the
 * driver does bit-bang bus-clear which corrupts the SDA/SCL pin mux.
 * After that, RX always fails with ESP_ERR_INVALID_STATE.
 *
 * The only way to recover is to delete the entire bus and recreate it
 * from scratch.  All device handles (including BL, TCA6408A, codecs)
 * are rebuilt.
 *
 * PRECONDITION: caller must NOT hold i2c_mutex (it will be destroyed
 * and recreated).
 */
esp_err_t l4_i2c_bus_full_reinit(void)
{
    ESP_LOGW(TAG, "=== Full I2C bus reinit (recovering from GPIO corruption) ===");

    /* --- Phase 1: Tear down everything --- */

    /* Remove BL device if present */
    if (l4_stm32_bl_dev_handle != NULL) {
        (void)i2c_master_bus_rm_device(l4_stm32_bl_dev_handle);
        l4_stm32_bl_dev_handle = NULL;
        l4_stm32_bl_dev_addr = 0;
    }

    /* Remove all normal devices */
    for (int i = 0; i < 4; i++) {
        if (l4_i2c_dev_handle[i] != NULL) {
            (void)i2c_master_bus_rm_device(l4_i2c_dev_handle[i]);
            l4_i2c_dev_handle[i] = NULL;
        }
    }

    /* Delete the bus */
    if (l4_bus_handle != NULL) {
        (void)i2c_del_master_bus(l4_bus_handle);
        l4_bus_handle = NULL;
    }

    /* Delete the mutex */
    if (i2c_mutex != NULL) {
        vSemaphoreDelete(i2c_mutex);
        i2c_mutex = NULL;
    }

    /* Reset initialization flags so i2c_init_once() runs again */
    taskENTER_CRITICAL(&i2c_init_lock);
    i2c_initialized = false;
    i2c_init_in_progress = false;
    taskEXIT_CRITICAL(&i2c_init_lock);

    /* --- Phase 2: Recreate from scratch --- */
    esp_err_t ret = i2c_init_once();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Full reinit FAILED: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Full reinit OK — bus and all devices rebuilt");
    }
    return ret;
}


esp_err_t l4_i2c_get_stm32_bl_device_at_addr(uint8_t addr7, i2c_master_dev_handle_t *out_dev)
{
    if (out_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (addr7 == 0 || addr7 > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Ensure I2C bus is initialized first */
    if (!i2c_initialized || l4_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized, cannot create STM32 BL device");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (BL get/create)");
        return ESP_ERR_TIMEOUT;
    }

    /* Reuse existing handle only when address matches requested one. */
    if (l4_stm32_bl_dev_handle != NULL && l4_stm32_bl_dev_addr == addr7) {
        *out_dev = l4_stm32_bl_dev_handle;
        xSemaphoreGive(i2c_mutex);
        return ESP_OK;
    }

    /* Address changed: remove previous BL device before creating a new one. */
    if (l4_stm32_bl_dev_handle != NULL) {
        (void)i2c_master_bus_rm_device(l4_stm32_bl_dev_handle);
        l4_stm32_bl_dev_handle = NULL;
        l4_stm32_bl_dev_addr = 0;
    }

    /* Create new device for STM32 bootloader with detected 7-bit address.
     *
     * CRITICAL (2026-03-30): scl_wait_us sets the I2C hardware SCL timeout.
     * On ESP32, the timeout register (I2C_TIME_OUT_REG) is only 20 bits wide
     * (max 0xFFFFF = 1,048,575 APB cycles).  The value is computed as:
     *     reg = (APB_freq / 1MHz) * scl_wait_us = 80 * scl_wait_us
     * With scl_wait_us = 500000 → reg = 40,000,000 → OVERFLOWS 20-bit field!
     *     actual = 40,000,000 & 0xFFFFF = 154,112 → ~1.9 ms timeout
     * This caused the STM32 BL command/response path to fail: the bootloader
     * may stretch SCL for a few ms while preparing ACK/data frames, but the
     * ~2 ms hardware timeout fires first.
     *
     * Max safe value: 0xFFFFF / 80 = 13,107 μs ≈ 13 ms.
     * We use 13000 μs which gives ~1,040,000 APB cycles (fits in 20 bits).
     * 13 ms is more than sufficient for the BL handshake (<<1 ms typical).
     */
    i2c_device_config_t bl_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = STM32_BL_SPEED_HZ,   /* 100 kHz per STM32 AN2606 Table 230 */
        .scl_wait_us = 13000,                 /* 13 ms — max safe for ESP32 20-bit timeout reg */
    };

    esp_err_t ret = i2c_master_bus_add_device(l4_bus_handle, &bl_cfg, &l4_stm32_bl_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add STM32 bootloader device: %s", esp_err_to_name(ret));
        l4_stm32_bl_dev_handle = NULL;
        l4_stm32_bl_dev_addr = 0;
        xSemaphoreGive(i2c_mutex);
        return ret;
    }

    l4_stm32_bl_dev_addr = addr7;
    *out_dev = l4_stm32_bl_dev_handle;
    xSemaphoreGive(i2c_mutex);
    return ESP_OK;
}

esp_err_t l4_i2c_release_stm32_bl_device(void)
{
    if (l4_stm32_bl_dev_handle == NULL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (BL release)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2c_master_bus_rm_device(l4_stm32_bl_dev_handle);
    if (ret == ESP_OK) {
        l4_stm32_bl_dev_handle = NULL;
        l4_stm32_bl_dev_addr = 0;
    }

    xSemaphoreGive(i2c_mutex);
    return ret;
}


/**
 * @brief Send bytes to STM32 bootloader with I2C mutex protection.
 *
 * Acquires the I2C bus mutex before transmission to prevent conflicts
 * with other I2C devices on the shared bus.
 *
 * @param[in] buf       Pointer to data buffer to send.
 * @param[in] len       Number of bytes to send.
 * @param[in] timeout_ms Timeout in milliseconds.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t l4_i2c_stm32_bl_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (BL transmit)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_transmit(dev, buf, len, timeout_ms);

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK && res != ESP_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "BL transmit failed: %s", esp_err_to_name(res));
    }
    return res;
}

/**
 * @brief esp_timer callback: disable I2C hardware timeout interrupt.
 *
 * On ESP32, the I2C controller does NOT abort a transaction on SCL
 * timeout — it merely fires an interrupt.  The ESP-IDF driver's ISR
 * then gives cmd_semphr, and the command-loop detects TIMEOUT status
 * and calls s_i2c_hw_fsm_reset(true) which destructively bit-bangs
 * the SDA/SCL GPIOs, permanently corrupting the I2C RX path.
 *
 * By masking the timeout interrupt AFTER the driver has enabled all
 * interrupts but BEFORE the 13 ms hardware timeout fires, we let the
 * I2C controller keep waiting for the slave to release SCL.  When the
 * slave finally responds, END_DETECT / MST_COMPLETE fires normally and
 * the transaction completes.  If the slave never responds, the software
 * timeout (xSemaphoreTake in s_i2c_send_commands) fires cleanly without
 * any destructive bus-clear.
 *
 * This callback runs from the esp_timer task (priority 22 on ESP32),
 * so it preempts our BL task (priority 10) reliably.
 */
static void i2c_hw_timeout_int_disable_cb(void *arg)
{
    s_i2c_timeout_cb_fired = true;
    s_i2c_timeout_cb_time_us = esp_timer_get_time();

    /* I2C_NUM_1 is used by this project — clear the timeout interrupt enable bit.
     *
     * We intentionally do NOT modify I2C_TO_REG (the timeout threshold).
     * The hardware will still detect a timeout condition after ~13 ms,
     * but with the interrupt masked the ISR never fires, so the
     * destructive s_i2c_hw_fsm_reset(true) / s_i2c_master_clear_bus()
     * path is never reached.  The I2C FSM keeps waiting for the slave
     * to release SCL, and when it does, END_DETECT fires normally.
     *
     * IMPORTANT: the previous code wrote REG_WRITE(I2C_TO_REG(1), 0)
     * intending to "disable" the timeout counter.  This is WRONG:
     * setting the threshold to 0 means the timeout condition is met
     * IMMEDIATELY (counter value 0 >= threshold 0), which corrupted
     * data reads (0x30 garbage after the first few correct bytes).
     */
    REG_CLR_BIT(I2C_INT_ENA_REG(1), I2C_TIME_OUT_INT_ENA_M);
}

/**
 * @brief Receive from STM32 bootloader with hardware timeout interrupt suppressed.
 *
 * Identical to l4_i2c_stm32_bl_receive, except it uses an esp_timer to
 * disable the I2C hardware timeout interrupt shortly after the ESP-IDF
 * driver enables interrupts.  This allows the STM32 bootloader to stretch
 * SCL for >13 ms (the ESP32 hardware timeout maximum) without triggering
 * the destructive s_i2c_hw_fsm_reset(true) inside the driver.
 *
 * After the receive completes (success or software timeout), the timeout
 * interrupt is re-enabled for normal I2C operation.
 *
 * NOTE (2026-03-30): For handshake A/B tests we try
 * l4_i2c_stm32_bl_write_read_atomic_long_stretch() first, then compare
 * it against this split path under identical reset conditions.
 * Atomic is preferred diagnostically, but is not yet a proven fix.
 */
esp_err_t l4_i2c_stm32_bl_receive_long_stretch(i2c_master_dev_handle_t dev,
                                                 uint8_t *buf, size_t len,
                                                 uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Create a one-shot timer that fires 500 µs after we start.
     * Timeline:
     *   T+0        our code takes i2c_mutex
     *   T+~30 µs   i2c_master_receive → s_i2c_transaction_start enables interrupts
     *   T+~100 µs  hardware commands programmed, transaction started
     *   T+500 µs   *** timer fires: disable timeout interrupt ***
     *   T+~13 ms   hardware timeout condition met — but interrupt is masked!
     *   T+??        STM32 releases SCL → MST_COMPLETE fires → normal completion
     *   T+timeout   or software timeout fires (clean, no GPIO corruption)
     */
    esp_timer_handle_t tmr = NULL;
    const esp_timer_create_args_t tmr_args = {
        .callback = i2c_hw_timeout_int_disable_cb,
        .name = "i2c_noTO",
    };

    esp_err_t tmr_err = esp_timer_create(&tmr_args, &tmr);
    if (tmr_err != ESP_OK) {
        ESP_LOGW(TAG, "BL long-stretch: cannot create timer (%s), falling back",
                 esp_err_to_name(tmr_err));
        return l4_i2c_stm32_bl_receive(dev, buf, len, timeout_ms);
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (BL long-stretch receive)");
        esp_timer_delete(tmr);
        return ESP_ERR_TIMEOUT;
    }

    bl_timeout_diag_reset();

    /* Arm the timer AFTER taking the mutex — guarantees the timer fires
     * while our i2c_master_receive is the active bus user. */
    const int64_t t0 = esp_timer_get_time();
    esp_timer_start_once(tmr, 500);  /* 500 µs */

    esp_err_t res = i2c_master_receive(dev, buf, len, timeout_ms);

    /* Stop and delete the timer (may have already fired — that's fine) */
    esp_timer_stop(tmr);
    esp_timer_delete(tmr);

    /* Re-enable the timeout interrupt for normal I2C operations.
     * The timer callback only masked the interrupt — it did NOT modify
     * the timeout threshold register.  We restore the enable bit
     * unconditionally (the timer may or may not have fired). */
    REG_SET_BIT(I2C_INT_ENA_REG(1), I2C_TIME_OUT_INT_ENA_M);

    bl_timeout_diag_log("BL long-stretch", t0, res);

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK && res != ESP_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "BL long-stretch receive failed: %s", esp_err_to_name(res));
    }
    return res;
}

/**
 * @brief Receive bytes from STM32 bootloader with I2C mutex protection.
 *
 * Acquires the I2C bus mutex before reception to prevent conflicts
 * with other I2C devices on the shared bus.
 *
 * @param[out] buf       Pointer to receive buffer.
 * @param[in]  len       Number of bytes to receive.
 * @param[in]  timeout_ms Timeout in milliseconds.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t l4_i2c_stm32_bl_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (BL receive)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_receive(dev, buf, len, timeout_ms);

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK && res != ESP_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "BL receive failed: %s", esp_err_to_name(res));
    }
    return res;
}

/**
 * @brief Atomic write-then-read to STM32 bootloader under a single mutex lock.
 *
 * Holds the I2C bus mutex for the entire sequence: transmit → optional
 * inter-frame delay → receive.  This prevents other I2C bus users
 * (audio codecs, TCA6408A, etc.) from inserting transactions between
 * write and read, which could leave the I2C FSM in an error state and
 * cause the subsequent read to fail with hardware timeout.
 *
 * Generic helper for diagnostic write-then-read STM32 bootloader exchanges.
 *
 * DEBUGGING NOTES (2026-03-29):
 *   - Device handle (dev) must be valid and belong to the STM32 BL on the bus.
 *   - If RX fails with ESP_ERR_INVALID_STATE, the I2C FSM may be corrupted
 *     by concurrent stm32_task I2C operations before the 100ms quiesce delay.
 *   - If RX times out, verify STM32 is in bootloader mode (BOOT0=1, post-NRST).
 *   - Log output includes device handle pointer for integrity verification.
 *
 * Return semantics for callers that need to distinguish the failure stage:
 *   - returns the original TX error if transmit phase failed;
 *   - returns ESP_ERR_INVALID_RESPONSE if TX succeeded but RX failed.
 */
esp_err_t l4_i2c_stm32_bl_write_then_read(i2c_master_dev_handle_t dev,
                                           const uint8_t *tx_buf, size_t tx_len,
                                           uint8_t *rx_buf, size_t rx_len,
                                           uint32_t inter_delay_ms,
                                           uint32_t timeout_ms)
{
    if (tx_buf == NULL || tx_len == 0 || rx_buf == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        ESP_LOGE(TAG, "BL wtr: device handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "BL wtr: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* Log TX payload */
    if (tx_len <= 8) {
        char hex[25]; /* 8 bytes × 3 chars + NUL */
        int pos = 0;
        for (size_t k = 0; k < tx_len && pos < (int)sizeof(hex) - 3; k++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", tx_buf[k]);
        }
        ESP_LOGI(TAG, "BL wtr: TX[%u]={%s} delay=%"PRIu32"ms tmo=%"PRIu32"ms, dev=%p",
                 (unsigned)tx_len, hex, inter_delay_ms, timeout_ms, (void *)dev);
    } else {
        ESP_LOGI(TAG, "BL wtr: TX[%u] first=0x%02x delay=%"PRIu32"ms tmo=%"PRIu32"ms, dev=%p",
                 (unsigned)tx_len, tx_buf[0], inter_delay_ms, timeout_ms, (void *)dev);
    }

    int64_t t0 = esp_timer_get_time();

    esp_err_t res = i2c_master_transmit(dev, tx_buf, tx_len, timeout_ms);

    int64_t t1 = esp_timer_get_time();

    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BL wtr: TX failed after %lld us: %s (0x%04x), dev=%p",
                 (long long)(t1 - t0), esp_err_to_name(res), (unsigned)res, (void *)dev);
        xSemaphoreGive(i2c_mutex);
        return res;
    }

    ESP_LOGI(TAG, "BL wtr: TX OK in %lld us, dev=%p", (long long)(t1 - t0), (void *)dev);

    if (inter_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(inter_delay_ms));
    }

    /* Clear RX buffer so we can detect stale reads */
    memset(rx_buf, 0, rx_len);

    int64_t t2 = esp_timer_get_time();

    ESP_LOGD(TAG, "BL wtr: RX starting (dev=%p, len=%u, tmo=%"PRIu32"ms)",
             (void *)dev, (unsigned)rx_len, timeout_ms);

    res = i2c_master_receive(dev, rx_buf, rx_len, timeout_ms);

    int64_t t3 = esp_timer_get_time();

    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BL wtr: RX failed after %lld us: %s (0x%04x), "
                 "rx[0]=0x%02x, total_elapsed=%lld us, dev=%p",
                 (long long)(t3 - t2), esp_err_to_name(res), (unsigned)res,
                 rx_buf[0], (long long)(t3 - t0), (void *)dev);
        /* Log additional context for debugging */
        if (res == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "BL wtr: I2C FSM state invalid (possible corruption) — may need bus reset");
        } else if (res == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "BL wtr: RX timeout — STM32 BL may not be ready or I2C bus stuck");
        }
        xSemaphoreGive(i2c_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    } else {
        if (rx_len <= 4) {
            char hex[13]; /* 4 bytes × 3 chars + NUL */
            int pos = 0;
            for (size_t k = 0; k < rx_len && pos < (int)sizeof(hex) - 3; k++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", rx_buf[k]);
            }
            ESP_LOGI(TAG, "BL wtr: RX OK in %lld us, rx[%u]={%s}, total=%lld us, dev=%p",
                     (long long)(t3 - t2), (unsigned)rx_len, hex, (long long)(t3 - t0), (void *)dev);
        } else {
            ESP_LOGI(TAG, "BL wtr: RX OK in %lld us, rx[%u] first=0x%02x, total=%lld us, dev=%p",
                     (long long)(t3 - t2), (unsigned)rx_len, rx_buf[0], (long long)(t3 - t0), (void *)dev);
        }
    }

    xSemaphoreGive(i2c_mutex);
    return res;
}

/**
 * @brief Probe an I2C address on the shared bus with mutex protection.
 *
 * Sends START + addr(W) + STOP to check whether a device ACKs.
 *
 * @param addr7      7-bit I2C address to probe.
 * @param timeout_ms Probe timeout in milliseconds (-1 = default).
 * @return ESP_OK if the device ACKs, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t l4_i2c_probe_addr(uint16_t addr7, int timeout_ms)
{
    if (!i2c_initialized || l4_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized, cannot probe");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "I2C Mutex timeout (probe)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t res = i2c_master_probe(l4_bus_handle, addr7, timeout_ms);
    xSemaphoreGive(i2c_mutex);

    return res;
}

/**
 * @brief Atomic write-then-read using i2c_master_transmit_receive (repeated START).
 *
 * Performs:  START + addr(W) + tx_data + rSTART + addr(R) + rx_data + STOP
 * as a single I2C transaction.  This avoids the STOP/START gap between
 * separate transmit and receive, which can trigger I2C hardware timeouts
 * when the STM32 bootloader stretches SCL.
 *
 * Used as an alternative handshake method alongside write_then_read.
 */
esp_err_t l4_i2c_stm32_bl_write_read_atomic(i2c_master_dev_handle_t dev,
                                              const uint8_t *tx_buf, size_t tx_len,
                                              uint8_t *rx_buf, size_t rx_len,
                                              uint32_t timeout_ms)
{
    if (tx_buf == NULL || tx_len == 0 || rx_buf == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "BL atomic: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* Clear RX buffer so stale data is detectable */
    memset(rx_buf, 0, rx_len);

    int64_t t0 = esp_timer_get_time();

    esp_err_t res = i2c_master_transmit_receive(dev,
                                                 tx_buf, tx_len,
                                                 rx_buf, rx_len,
                                                 timeout_ms);

    int64_t t1 = esp_timer_get_time();

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BL atomic: failed in %lld us: %s (0x%04x), "
                 "tx[0]=0x%02x, rx[0]=0x%02x",
                 (long long)(t1 - t0), esp_err_to_name(res), (unsigned)res,
                 tx_buf[0], rx_buf[0]);
    } else {
        ESP_LOGI(TAG, "BL atomic: OK in %lld us, tx[0]=0x%02x, rx[0]=0x%02x",
                 (long long)(t1 - t0), tx_buf[0], rx_buf[0]);
    }
    return res;
}

/**
 * @brief Atomic write-then-read with hardware timeout suppressed.
 *
 * Combines i2c_master_transmit_receive (repeated START) with the
 * esp_timer trick from receive_long_stretch to mask the I2C hardware
 * timeout interrupt for custom diagnostic transactions.
 *
 *   START + addr(W) + tx_data + rSTART + addr(R) + rx_data + STOP
 *
 * Using repeated START (no STOP between write and read) keeps the bus
 * held throughout the transaction, which can help when a target stretches
 * SCL or expects a contiguous write/read command-response cycle.
 *
 * The timer fires 500 µs after the call, disabling both the timeout
 * interrupt enable bit and the timeout counter register.  This lets
 * the I2C controller wait indefinitely for the slave to release SCL.
 */
esp_err_t l4_i2c_stm32_bl_write_read_atomic_long_stretch(
    i2c_master_dev_handle_t dev,
    const uint8_t *tx_buf, size_t tx_len,
    uint8_t *rx_buf, size_t rx_len,
    uint32_t timeout_ms)
{
    if (tx_buf == NULL || tx_len == 0 || rx_buf == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Create a one-shot timer identical to receive_long_stretch */
    esp_timer_handle_t tmr = NULL;
    const esp_timer_create_args_t tmr_args = {
        .callback = i2c_hw_timeout_int_disable_cb,
        .name = "i2c_noTO_a",
    };

    esp_err_t tmr_err = esp_timer_create(&tmr_args, &tmr);
    if (tmr_err != ESP_OK) {
        ESP_LOGW(TAG, "BL atomic-ls: cannot create timer (%s), falling back",
                 esp_err_to_name(tmr_err));
        /* Fall back to plain atomic (no timeout suppression) */
        return l4_i2c_stm32_bl_write_read_atomic(dev, tx_buf, tx_len,
                                                   rx_buf, rx_len, timeout_ms);
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "BL atomic-ls: mutex timeout");
        esp_timer_delete(tmr);
        return ESP_ERR_TIMEOUT;
    }

    /* Clear RX buffer */
    memset(rx_buf, 0, rx_len);

    bl_timeout_diag_reset();

    /* Arm the timer AFTER taking the mutex */
    const int64_t t0 = esp_timer_get_time();
    esp_timer_start_once(tmr, 500);  /* 500 µs */

    esp_err_t res = i2c_master_transmit_receive(dev,
                                                 tx_buf, tx_len,
                                                 rx_buf, rx_len,
                                                 timeout_ms);

    int64_t t1 = esp_timer_get_time();

    /* Stop and delete the timer */
    esp_timer_stop(tmr);
    esp_timer_delete(tmr);

    /* Restore timeout interrupt for normal I2C operations */
    REG_SET_BIT(I2C_INT_ENA_REG(1), I2C_TIME_OUT_INT_ENA_M);

    bl_timeout_diag_log("BL atomic-ls", t0, res);

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BL atomic-ls: failed in %lld us: %s (0x%04x), "
                 "tx[0]=0x%02x, rx[0]=0x%02x",
                 (long long)(t1 - t0), esp_err_to_name(res), (unsigned)res,
                 tx_buf[0], rx_buf[0]);
    } else {
        ESP_LOGI(TAG, "BL atomic-ls: OK in %lld us, tx[0]=0x%02x, rx[0]=0x%02x",
                 (long long)(t1 - t0), tx_buf[0], rx_buf[0]);
    }
    return res;
}

esp_err_t l4_i2c_stm32_bl_execute_ops_long_stretch(
    i2c_master_dev_handle_t dev,
    i2c_operation_job_t *ops,
    size_t num_ops,
    uint32_t timeout_ms)
{
    if (dev == NULL || ops == NULL || num_ops == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_timer_handle_t tmr = NULL;
    const esp_timer_create_args_t tmr_args = {
        .callback = i2c_hw_timeout_int_disable_cb,
        .name = "i2c_noTO_ops",
    };

    esp_err_t tmr_err = esp_timer_create(&tmr_args, &tmr);
    if (tmr_err != ESP_OK) {
        ESP_LOGW(TAG, "BL ops-ls: cannot create timer (%s)", esp_err_to_name(tmr_err));
        return tmr_err;
    }

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "BL ops-ls: mutex timeout");
        esp_timer_delete(tmr);
        return ESP_ERR_TIMEOUT;
    }

    bl_timeout_diag_reset();
    const int64_t t0 = esp_timer_get_time();
    esp_timer_start_once(tmr, 500);

    esp_err_t res = i2c_master_execute_defined_operations(dev, ops, num_ops, (int)timeout_ms);

    int64_t t1 = esp_timer_get_time();
    esp_timer_stop(tmr);
    esp_timer_delete(tmr);
    REG_SET_BIT(I2C_INT_ENA_REG(1), I2C_TIME_OUT_INT_ENA_M);
    bl_timeout_diag_log("BL ops-ls", t0, res);

    xSemaphoreGive(i2c_mutex);

    if (res != ESP_OK) {
        ESP_LOGW(TAG, "BL ops-ls: failed in %lld us: %s (ops=%u)",
                 (long long)(t1 - t0), esp_err_to_name(res), (unsigned)num_ops);
    } else {
        ESP_LOGI(TAG, "BL ops-ls: OK in %lld us (ops=%u)",
                 (long long)(t1 - t0), (unsigned)num_ops);
    }
    return res;
}

