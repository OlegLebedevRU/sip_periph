/*
 * service_tca6408.c
 *
 * Выделенный сервис обработки TCA6408A.
 * Читает reg[0] по очередному событию TCA IRQ, вычисляет изменения
 * относительно предыдущего снимка и маршрутизирует:
 *   - DS3231 1Hz tick
 *   - PN532 ready
 *   - внешнюю кнопку с debounce / suppress
 */

#include "main.h"
#include "cmsis_os.h"
#include "tca6408a_map.h"
#include "service_tca6408.h"
#include "service_time_sync.h"
#include "service_runtime_config.h"
#include "service_relay_actuator.h"

extern I2C_HandleTypeDef hi2c2;
extern osMessageQId myQueueTCA6408Handle;
extern osSemaphoreId pn532SemaphoreHandle;
extern osMutexId i2c2_MutexHandle;
extern uint8_t *app_i2c_slave_get_ram(void);

#define TCA_I2C_ADDR               (0x40U)
#define TCA_BOOTSTRAP_EVENT        (1U)
#define TCA_BTN_DEBOUNCE_MS        (60U)
#define TCA_BTN_SUPPRESS_MS        (200U)
#define TCA_DS_LOW_LATCH_WINDOW_MS (400U)

static uint8_t  s_prev_inputs = 0xFFU;
static uint8_t  s_last_inputs = 0xFFU;
static HAL_StatusTypeDef s_last_hal_status = HAL_OK;
static uint32_t s_last_i2c_error = HAL_I2C_ERROR_NONE;
static uint8_t  s_btn_debounce_active = 0U;
static uint32_t s_btn_debounce_deadline = 0U;
static uint32_t s_btn_suppress_until = 0U;
static uint32_t s_ds_low_latched_at = 0U;
static uint8_t  s_ds_low_seen = 0U;

static void tca_i2c_recover_if_needed(HAL_StatusTypeDef status)
{
    s_last_hal_status = status;
    s_last_i2c_error = HAL_I2C_GetError(&hi2c2);

    if ((status == HAL_ERROR) || (status == HAL_TIMEOUT) || (status == HAL_BUSY)) {
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
    }
}

static void tca_handle_pn532(uint8_t curr_inputs)
{
    if ((curr_inputs & TCA_P3_PN532_IRQ) == 0U) {
        osSemaphoreRelease(pn532SemaphoreHandle);
    }
}

/*
 * tca_handle_ds3231 — обработка 1Hz SQW от DS3231M через TCA P0.
 *
 * Логика dedup/latch:
 *   1. P0 == LOW (активная фаза SQW):
 *      - Если s_ds_low_seen == 0 → первый фронт LOW в этом цикле:
 *        вызываем service_time_sync_on_tick() + runtime_config_apply,
 *        ставим s_ds_low_seen = 1 и запоминаем время.
 *      - Повторные LOW-вызовы (bounce TCA IRQ) игнорируются (dedup).
 *   2. P0 == HIGH (SQW вернулся в HIGH):
 *      - Сбрасываем s_ds_low_seen после TCA_DS_LOW_LATCH_WINDOW_MS,
 *        чтобы быть готовым к следующему фронту LOW (через ~500-600ms).
 *
 * ВАЖНО: tick вызывается безусловно при первом LOW, даже если
 * одновременно изменились P2 (кнопка) или P3 (PN532).
 * Ранее mixed_other != 0 блокировал tick — это приводило к потере
 * секундного импульса при совпадении событий.
 */
static void tca_handle_ds3231(uint8_t curr_inputs, uint32_t now)
{
    if ((curr_inputs & TCA_P0_DS3231_1HZ) == 0U) {
        /* P0 LOW — активная фаза SQW 1Hz */
        if (s_ds_low_seen == 0U) {
            uint8_t *iram = app_i2c_slave_get_ram();
            (void)service_time_sync_on_tick();
            runtime_config_apply_from_ram(iram);
            s_ds_low_seen = 1U;
            s_ds_low_latched_at = now;
        }
    } else {
        /* P0 HIGH — SQW вернулся; сбросить latch после окна */
        if ((s_ds_low_seen != 0U) && ((now - s_ds_low_latched_at) >= TCA_DS_LOW_LATCH_WINDOW_MS)) {
            s_ds_low_seen = 0U;
        }
    }
}

static void tca_finish_button_debounce(uint8_t stable_inputs, uint32_t now)
{
    uint8_t button_low = (uint8_t)((stable_inputs & TCA_P2_EXT_BUTTON) == 0U);

    s_btn_debounce_active = 0U;

    if (button_low == 0U) {
        relay_ext_button_release();
        return;
    }

    if (relay_is_relay1_active()) {
        return;
    }

    if ((int32_t)(now - s_btn_suppress_until) < 0) {
        return;
    }

    relay_request_pulse(RELAY_SRC_EXT_BTN);
    s_btn_suppress_until = now + TCA_BTN_SUPPRESS_MS;
}

static void tca_handle_button(uint8_t changed_inputs, uint8_t curr_inputs, uint32_t now)
{
    if ((changed_inputs & TCA_P2_EXT_BUTTON) != 0U) {
        if ((curr_inputs & TCA_P2_EXT_BUTTON) != 0U) {
            relay_ext_button_release();
        }

        if (relay_is_ext_button_flow_blocked()) {
            return;
        }

        if (s_btn_debounce_active == 0U) {
            s_btn_debounce_active = 1U;
            s_btn_debounce_deadline = now + TCA_BTN_DEBOUNCE_MS;
        }
    }

    if ((s_btn_debounce_active != 0U) && ((int32_t)(now - s_btn_debounce_deadline) >= 0)) {
        uint8_t stable_inputs = curr_inputs;
        if (service_tca6408_read_reg(TCA6408A_REG_INPUT, &stable_inputs) == HAL_OK) {
            s_last_inputs = stable_inputs;
            tca_finish_button_debounce(stable_inputs, now);
        } else {
            s_btn_debounce_active = 0U;
        }
    }
}

void service_tca6408_init(void)
{
    uint8_t boot_inputs = 0xFFU;

    s_last_hal_status = HAL_OK;
    s_last_i2c_error = HAL_I2C_ERROR_NONE;
    (void)service_tca6408_write_reg(TCA6408A_REG_CONFIG, TCA6408A_ALL_INPUTS);

    if (service_tca6408_read_reg(TCA6408A_REG_INPUT, &boot_inputs) != HAL_OK) {
        boot_inputs = 0xFFU;
    }

    s_prev_inputs = boot_inputs;
    s_last_inputs = boot_inputs;
    s_btn_debounce_active = 0U;
    s_btn_debounce_deadline = 0U;
    s_btn_suppress_until = 0U;
    s_ds_low_seen = 0U;
    s_ds_low_latched_at = 0U;
}

void service_tca6408_post_bootstrap(void)
{
    uint16_t tca_boot = TCA_BOOTSTRAP_EVENT;
    xQueueSend(myQueueTCA6408Handle, &tca_boot, 0);
}

void service_tca6408_process_irq_event(void)
{
    uint8_t curr_inputs = 0xFFU;
    uint32_t now = HAL_GetTick();

    if (service_tca6408_read_reg(TCA6408A_REG_INPUT, &curr_inputs) != HAL_OK) {
        return;
    }

    s_last_inputs = curr_inputs;

    {
        uint8_t changed = (uint8_t)((s_prev_inputs ^ curr_inputs) & TCA_USED_INPUTS_MASK);

        tca_handle_pn532(curr_inputs);
        tca_handle_button(changed, curr_inputs, now);
        tca_handle_ds3231(curr_inputs, now);
    }

    s_prev_inputs = curr_inputs;
}

HAL_StatusTypeDef service_tca6408_write_reg(uint8_t reg_addr, uint8_t data)
{
    HAL_StatusTypeDef status;
    if (osMutexWait(i2c2_MutexHandle, 100U) != osOK) {
        return HAL_BUSY;
    }
    status = HAL_I2C_Mem_Write(&hi2c2, TCA_I2C_ADDR, (uint16_t)reg_addr,
                               I2C_MEMADD_SIZE_8BIT, &data, 1U, 100U);
    osMutexRelease(i2c2_MutexHandle);
    tca_i2c_recover_if_needed(status);
    return status;
}

HAL_StatusTypeDef service_tca6408_read_reg(uint8_t reg_addr, uint8_t *data)
{
    HAL_StatusTypeDef status;
    if (data == NULL) {
        s_last_hal_status = HAL_ERROR;
        s_last_i2c_error = 0xFFFFFFFFU;  /* NULL param sentinel */
        return HAL_ERROR;
    }
    if (osMutexWait(i2c2_MutexHandle, 100U) != osOK) {
        return HAL_BUSY;
    }
    status = HAL_I2C_Mem_Read(&hi2c2, TCA_I2C_ADDR, (uint16_t)reg_addr,
                              I2C_MEMADD_SIZE_8BIT, data, 1U, 100U);
    osMutexRelease(i2c2_MutexHandle);
    tca_i2c_recover_if_needed(status);
    return status;
}

HAL_StatusTypeDef service_tca6408_get_last_hal_status(void)
{
    return s_last_hal_status;
}

uint32_t service_tca6408_get_last_i2c_error(void)
{
    return s_last_i2c_error;
}

bool service_tca6408_is_button_debounce_active(void)
{
    return s_btn_debounce_active != 0U;
}

uint8_t service_tca6408_get_last_inputs(void)
{
    return s_last_inputs;
}

/* ---- FreeRTOS task body (moved from main.c, step 14) ------------------- */

void StartTasktca6408a(void const *argument)
{
    service_tca6408_init();
    service_tca6408_post_bootstrap();

    for (;;) {
        uint16_t tca;
        xQueueReceive(myQueueTCA6408Handle, &tca, osWaitForever);
        (void)tca;
        service_tca6408_process_irq_event();
        osDelay(1);
    }
}
