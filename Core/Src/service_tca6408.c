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

/* ---- I2C2 recovery policy -------------------------------------------- */
#define I2C2_STARTUP_SETTLE_MS      50U   /* settle time after power-on    */
#define I2C2_COOLDOWN_MS            5U    /* pause between DeInit and Init */
#define I2C2_CLK_RECOVERY_PULSES    9U    /* SCL pulses to unstick slave   */
#define I2C2_HARD_RECOVER_THRESHOLD 3U    /* consec errors before hard rcv */
#define I2C2_GUARD_INTERVAL_MS      1000U /* guard poll period             */
#define I2C2_HEARTBEAT_TIMEOUT_MS   2500U /* max gap between DS3231 ticks  */
#define I2C2_INIT_RETRY_COUNT       3U    /* DS3231 / TCA init retry limit */
#define I2C2_SCL_PIN                GPIO_PIN_10
#define I2C2_SDA_PIN                GPIO_PIN_3
#define I2C2_GPIO_PORT              GPIOB

static uint8_t  s_prev_inputs = 0xFFU;
static uint8_t  s_last_inputs = 0xFFU;
static HAL_StatusTypeDef s_last_hal_status = HAL_OK;
static uint32_t s_last_i2c_error = HAL_I2C_ERROR_NONE;
static uint8_t  s_btn_debounce_active = 0U;
static uint32_t s_btn_debounce_deadline = 0U;
static uint32_t s_btn_suppress_until = 0U;
static uint32_t s_ds_low_latched_at = 0U;
static uint8_t  s_ds_low_seen = 0U;

/* ---- I2C2 recovery state ---------------------------------------------- */
static volatile uint32_t s_i2c2_consec_err = 0U;
static volatile uint32_t s_i2c2_recover_count = 0U;
static volatile uint32_t s_i2c2_hard_recover_count = 0U;
static volatile uint32_t s_last_ds_heartbeat_tick = 0U;

/*
 * i2c2_clk_recovery — 9-bit SCL clock recovery for stuck I2C slave.
 *
 * Called after HAL_I2C_DeInit() so PB10/PB3 are already de-initialized
 * (in analog/floating state).  Drives 9 SCL pulses then a STOP condition
 * to release any slave holding SDA LOW.  GPIO is left reconfigured for
 * subsequent HAL_I2C_Init() call which restores AF4/AF9 via MspInit.
 */
static void i2c2_clk_recovery(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* Configure SCL (PB10) as GPIO output open-drain, drive HIGH */
    gpio.Pin   = I2C2_SCL_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(I2C2_GPIO_PORT, &gpio);
    HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SCL_PIN, GPIO_PIN_SET);

    /* Configure SDA (PB3) as input to observe slave state */
    gpio.Pin  = I2C2_SDA_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(I2C2_GPIO_PORT, &gpio);

    osDelay(1U);

    /* 9 SCL clock pulses to shift out any partial byte held by slave */
    for (uint32_t i = 0U; i < I2C2_CLK_RECOVERY_PULSES; i++) {
        HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SCL_PIN, GPIO_PIN_RESET);
        osDelay(1U);
        HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SCL_PIN, GPIO_PIN_SET);
        osDelay(1U);
    }

    /* Generate STOP condition: SDA LOW → HIGH while SCL is HIGH */
    gpio.Pin   = I2C2_SDA_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(I2C2_GPIO_PORT, &gpio);

    HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SCL_PIN, GPIO_PIN_RESET);
    osDelay(1U);
    HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SDA_PIN, GPIO_PIN_RESET);
    osDelay(1U);
    HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SCL_PIN, GPIO_PIN_SET);
    osDelay(1U);
    HAL_GPIO_WritePin(I2C2_GPIO_PORT, I2C2_SDA_PIN, GPIO_PIN_SET);
    osDelay(1U);
    /* PB10/PB3 left as output OD HIGH; HAL_I2C_Init() → MspInit restores AF */
}

/*
 * i2c2_soft_recover — fast DeInit + cooldown + Init with mutex protection.
 * Clears stale HAL state and BUSY bit via peripheral reset.
 */
static void i2c2_soft_recover(void)
{
    s_i2c2_recover_count++;
    if (osMutexWait(i2c2_MutexHandle, 200U) != osOK) {
        return;
    }
    HAL_I2C_DeInit(&hi2c2);
    osDelay(I2C2_COOLDOWN_MS);
    (void)HAL_I2C_Init(&hi2c2);
    osMutexRelease(i2c2_MutexHandle);
}

/*
 * i2c2_hard_recover — full I2C2 bus reset with 9-bit clock recovery.
 *
 * Sequence:
 *   1. HAL_I2C_DeInit  (stops peripheral, releases GPIO from AF)
 *   2. 9-bit SCL clock recovery  (unstick any slave holding SDA LOW)
 *   3. Cooldown delay
 *   4. SWRST to clear residual BUSY bit in peripheral registers
 *   5. HAL_I2C_Init  (restores peripheral and GPIO AF)
 *   6. Post bootstrap event to re-arm TCA ~INT pipeline
 */
static void i2c2_hard_recover(void)
{
    uint8_t did_recover;

    s_i2c2_hard_recover_count++;
    s_i2c2_consec_err = 0U;

    if (osMutexWait(i2c2_MutexHandle, 500U) != osOK) {
        return;
    }

    /* Step 1: Full DeInit (disables I2C2 clock, releases GPIO from AF mode) */
    HAL_I2C_DeInit(&hi2c2);

    /* Step 2: 9-bit SCL clock recovery (releases any slave holding SDA LOW) */
    i2c2_clk_recovery();

    /* Step 3: Cooldown */
    osDelay(I2C2_COOLDOWN_MS);

    /* Step 4: Enable clock and apply SWRST to clear residual BUSY in peripheral.
     * Physical bus is now free (step 2), so SWRST will fully clear BUSY bit.
     * __HAL_RCC_I2C2_CLK_ENABLE() is required here: DeInit (step 1) disabled the
     * clock, so we must re-enable it before accessing CR1 for SWRST. */
    __HAL_RCC_I2C2_CLK_ENABLE();
    hi2c2.Instance->CR1 |= I2C_CR1_SWRST;
    __NOP(); __NOP(); __NOP();  /* hold for ≥1 peripheral clock cycle */
    hi2c2.Instance->CR1 &= ~I2C_CR1_SWRST;

    /* Step 5: Re-initialize I2C2 (MspInit re-enables clock and restores GPIO AF) */
    did_recover = (HAL_I2C_Init(&hi2c2) == HAL_OK) ? 1U : 0U;

    osMutexRelease(i2c2_MutexHandle);

    /* Step 6: Re-arm TCA pipeline — reading TCA register clears ~INT,
     * allowing EXTI to fire again on the next DS3231 SQW edge */
    if (did_recover != 0U) {
        service_tca6408_post_bootstrap();
    }
}

static void tca_i2c_recover_if_needed(HAL_StatusTypeDef status)
{
    s_last_hal_status = status;
    s_last_i2c_error = HAL_I2C_GetError(&hi2c2);

    if (status == HAL_OK) {
        s_i2c2_consec_err = 0U;
        return;
    }

    s_i2c2_consec_err++;
    if (s_i2c2_consec_err >= I2C2_HARD_RECOVER_THRESHOLD) {
        s_i2c2_consec_err = 0U;
        i2c2_hard_recover();
    } else {
        i2c2_soft_recover();
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
            /* Heartbeat: successful TCA read + DS3231 pulse seen = I2C2 alive */
            s_last_ds_heartbeat_tick = now;
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

    /* Calm startup: allow pull-ups to charge and devices to power on */
    osDelay(I2C2_STARTUP_SETTLE_MS);

    /* Check for stuck BUSY condition before first transaction.
     * BUSY=1 at startup typically means the bus was not released after
     * a power-on glitch or a previous interrupted transaction.
     * Mutex is used to prevent a race with any concurrent I2C2 user. */
    if (osMutexWait(i2c2_MutexHandle, 200U) == osOK) {
        uint8_t bus_busy;
        __HAL_RCC_I2C2_CLK_ENABLE();
        bus_busy = (uint8_t)((hi2c2.Instance->SR2 & I2C_SR2_BUSY) != 0U);
        osMutexRelease(i2c2_MutexHandle);
        if (bus_busy != 0U) {
            i2c2_hard_recover();
        }
    }

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
    s_last_ds_heartbeat_tick = HAL_GetTick();
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

/* ---- I2C2 diagnostics -------------------------------------------------- */

uint32_t service_tca6408_get_hard_recover_count(void)
{
    return s_i2c2_hard_recover_count;
}

/* ---- I2C2 guard task ---------------------------------------------------- */
/*
 * StartTaskI2c2Guard — watchdog for I2C2 bus health.
 *
 * Uses the DS3231 1Hz SQW hardware signal as a liveness heartbeat:
 *   DS3231 SQW (1Hz) → TCA6408A P0 → ~INT → PC13 EXTI →
 *   TCA task reads TCA reg (I2C2) → tca_handle_ds3231 updates heartbeat.
 *
 * If no heartbeat for I2C2_HEARTBEAT_TIMEOUT_MS (2500 ms = 2 DS3231 ticks),
 * the entire I2C2 bus is considered stuck and hard recovery is performed.
 * After recovery, a bootstrap event re-arms the TCA ~INT pipeline.
 */
void StartTaskI2c2Guard(void const *argument)
{
    (void)argument;

    /* Wait for system to fully start up before monitoring */
    osDelay(3000U);

    /* Prime the heartbeat reference after startup delay */
    s_last_ds_heartbeat_tick = HAL_GetTick();

    for (;;) {
        uint32_t now;
        osDelay(I2C2_GUARD_INTERVAL_MS);
        now = HAL_GetTick();
        if ((now - s_last_ds_heartbeat_tick) >= I2C2_HEARTBEAT_TIMEOUT_MS) {
            i2c2_hard_recover();
            s_last_ds_heartbeat_tick = HAL_GetTick();
        }
    }
}
