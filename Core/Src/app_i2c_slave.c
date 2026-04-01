/*
 * app_i2c_slave.c
 *
 * I2C1 slave protocol wrapper with anti-stuck measures.
 *
 * Ключевые меры против залипания / clock stretching:
 * 1) Жёсткий контроль длины для известных входных регистров.
 * 2) Любая аномалия => reset FSM + re-enable LISTEN.
 * 3) После error/abort/listen complete гарантированно отпускаем EVENT pin,
 *    сбрасываем outbox state и не оставляем slave в промежуточной фазе.
 * 4) При BERR/AF — быстрая реинициализация hi2c1 + LISTEN.
 */

#include <string.h>
#include <stdio.h>
#include "main.h"
#include "cmsis_os.h"
#include "app_i2c_slave.h"
#include "service_time_sync.h"
#include "service_runtime_config.h"
#include "service_relay_actuator.h"

/* ---- externs from main.c ------------------------------------------------ */
extern I2C_HandleTypeDef hi2c1;
extern osMessageQId myQueueToMasterHandle;
extern osMessageQId myQueueHmiMsgHandle;
extern osTimerId myTimerBuzzerOffHandle;

/* ---- recovery policy ----------------------------------------------------- */
#define I2C_SLAVE_HEADER_TIMEOUT_MS      50U
#define I2C_SLAVE_PAYLOAD_TIMEOUT_MS     100U
#define I2C_SLAVE_IDLE_STUCK_TIMEOUT_MS  500U
#define I2C_SLAVE_RECOVERY_COOLDOWN_MS   2U
#define I2C_SLAVE_PUBLISH_PRE_DELAY_MS   10U
#define I2C_SLAVE_PUBLISH_POST_DELAY_MS  10U
#define I2C_SLAVE_OUTBOX_RETRY_DELAY_MS  20U
#define I2C_SLAVE_STUCK_CONFIRM_POLLS    3U
#define I2C_SLAVE_STUCK_CONFIRM_MS       15U

/* ---- local state -------------------------------------------------------- */
static uint8_t s_ram[256];
static struct i2c_test_s s_i2c_test = {0};
static const struct i2c_seq_ctrl_s s_i2c_sec_start_h = {
    .offset = 0,
    .first = 1,
    .second = 0,
    .final = 0,
    .last_base_ram_rcvd_addr = 0,
    .rx_count = 0,
    .tx_count = 0,
};
static struct i2c_seq_ctrl_s s_i2c_sec_ctrl_h = {
    .offset = 0,
    .first = 1,
    .second = 0,
    .final = 0,
    .last_base_ram_rcvd_addr = 0,
    .rx_count = 0,
    .tx_count = 0,
};

static volatile uint8_t s_outbox_busy = 0U;
static volatile uint8_t s_outbox_type = PACKET_NULL;
static volatile uint8_t s_outbox_reg = I2C_PACKET_TYPE_ADDR;
static volatile uint8_t s_outbox_len = 0U;
static volatile uint8_t s_last_tx_reg = 0xFFU;
static volatile uint8_t s_last_tx_len = 0U;
static volatile uint8_t s_event_latched = 0U;
static volatile uint32_t s_phase_deadline_tick = 0U;
static volatile uint32_t s_last_progress_tick = 0U;
static volatile uint8_t s_recovery_pending = 0U;
static volatile uint32_t s_recovery_started_tick = 0U;
static volatile uint8_t s_idle_scl_low_polls = 0U;
static volatile uint8_t s_idle_sda_low_polls = 0U;
static volatile uint32_t s_idle_scl_low_since_tick = 0U;
static volatile uint32_t s_idle_sda_low_since_tick = 0U;
static app_i2c_slave_diag_t s_diag = {0};

/* ---- forward declarations ----------------------------------------------- */
static void process_deferred_actions(void);

/* ---- internal helpers --------------------------------------------------- */
static uint32_t tick_now(void)
{
    return HAL_GetTick();
}

static uint8_t tick_expired(uint32_t now, uint32_t deadline)
{
    return (deadline != 0U) && ((int32_t)(now - deadline) >= 0);
}

static void note_progress(uint32_t timeout_ms)
{
    uint32_t now = tick_now();
    s_last_progress_tick = now;
    s_phase_deadline_tick = now + timeout_ms;
    s_diag.last_progress_tick = now;
}

static void clear_progress_watchdog(void)
{
    s_phase_deadline_tick = 0U;
    s_last_progress_tick = tick_now();
    s_diag.last_progress_tick = s_last_progress_tick;
    s_idle_scl_low_polls = 0U;
    s_idle_sda_low_polls = 0U;
    s_idle_scl_low_since_tick = 0U;
    s_idle_sda_low_since_tick = 0U;
}

static uint8_t i2c1_line_is_low(uint16_t pin)
{
    return (uint8_t)(HAL_GPIO_ReadPin(GPIOB, pin) == GPIO_PIN_RESET);
}

static void note_recovery_duration(void)
{
    uint32_t elapsed;
    if (s_recovery_started_tick == 0U) {
        return;
    }
    elapsed = tick_now() - s_recovery_started_tick;
    s_diag.last_recovery_ms = elapsed;
    if (elapsed > s_diag.max_recovery_ms) {
        s_diag.max_recovery_ms = elapsed;
    }
    s_recovery_started_tick = 0U;
}

static void mark_malformed_and_recover(void)
{
    s_diag.malformed_count++;
    s_recovery_pending = 1U;
}

static uint8_t packet_reg_for_type(I2cPacketType_t type)
{
    switch (type) {
    case PACKET_UID_532:  return I2C_REG_532_ADDR;
    case PACKET_PIN:      return I2C_REG_MATRIX_PIN_ADDR;
    case PACKET_WIEGAND:  return I2C_REG_WIEGAND_ADDR;
    case PACKET_HMI:      return I2C_REG_HMI_MSG_ADDR;
    case PACKET_TIME:     return I2C_REG_HW_TIME_ADDR;
    case PACKET_PIN_HMI:  return I2C_REG_HMI_PIN_ADDR;
    case PACKET_ERROR:    return I2C_REG_STM32_ERROR_ADDR;
    default:              return I2C_PACKET_TYPE_ADDR;
    }
}

static uint8_t packet_len_for_type(I2cPacketType_t type, size_t requested_len)
{
    size_t contract_len = requested_len;
    switch (type) {
    case PACKET_UID_532: contract_len = I2C_PACKET_UID_532_LEN; break;
    case PACKET_PIN:     contract_len = I2C_PACKET_PIN_LEN; break;
    case PACKET_WIEGAND: contract_len = I2C_PACKET_WIEGAND_LEN; break;
    case PACKET_PIN_HMI: contract_len = I2C_PACKET_PIN_HMI_LEN; break;
    case PACKET_TIME:    contract_len = I2C_PACKET_TIME_LEN; break;
    default: break;
    }
    return (uint8_t)((contract_len > 0xFFU) ? 0xFFU : contract_len);
}

static uint8_t strict_rx_len_for_register(uint8_t base)
{
    switch (base) {
    case HOST_MSG_RAM_ADDR:         return 0U; /* variable: msg[0]+3, check later */
    case HOST_AUTH_RESULT_RAM_ADDR: return 5U;
    case I2C_REG_HW_TIME_SET_ADDR:  return I2C_TIME_SYNC_WRITE_LEN;
    case I2C_REG_COUNTER_ADDR:      return 2U;
    case I2C_REG_CFG_ADDR:          return 16U;
    default:                        return 0U; /* unknown/legacy */
    }
}

static uint8_t is_known_register(uint8_t base)
{
    return (base == I2C_PACKET_TYPE_ADDR
         || base == I2C_REG_532_ADDR
         || base == I2C_REG_MATRIX_PIN_ADDR
         || base == I2C_REG_WIEGAND_ADDR
         || base == I2C_REG_COUNTER_ADDR
         || base == I2C_REG_HMI_PIN_ADDR
         || base == I2C_REG_HMI_MSG_ADDR
         || base == HOST_MSG_RAM_ADDR
         || base == HOST_AUTH_RESULT_RAM_ADDR
         || base == I2C_REG_HMI_ACT_ADDR
         || base == I2C_REG_HW_TIME_ADDR
         || base == I2C_REG_HW_TIME_SET_ADDR
         || base == I2C_REG_CFG_ADDR
         || base == I2C_REG_STM32_ERROR_ADDR) ? 1U : 0U;
}

static uint8_t validate_read_request(uint8_t reg, uint8_t len)
{
    if ((len == 0U) || (is_known_register(reg) == 0U)) {
        return 0U;
    }
    return ((uint16_t)reg + (uint16_t)len <= sizeof(s_ram)) ? 1U : 0U;
}

static void force_idle_event_line(void)
{
    HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_SET);
    s_event_latched = 0U;
}

static void reset_fsm_state(void)
{
    s_i2c_test.adr_count = 0U;
    s_i2c_test.rcv_cplt = 0U;
    s_i2c_test.rcv_start = 0U;
    s_i2c_sec_ctrl_h = s_i2c_sec_start_h;
    clear_progress_watchdog();
}

static void outbox_complete_ack(void)
{
    s_outbox_busy = 0U;
    s_outbox_type = PACKET_NULL;
    s_outbox_reg  = I2C_PACKET_TYPE_ADDR;
    s_outbox_len  = 0U;
    s_last_tx_reg = 0xFFU;
    s_last_tx_len = 0U;
    s_ram[I2C_PACKET_TYPE_ADDR] = PACKET_NULL;
    force_idle_event_line();
}

static void recover_after_error(void)
{
    outbox_complete_ack();
    reset_fsm_state();
}

static void restart_listen_if_needed(void)
{
    if (HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_LISTEN) {
        reset_fsm_state();
        if (HAL_I2C_EnableListen_IT(&hi2c1) == HAL_OK) {
            s_diag.relisten_count++;
            note_recovery_duration();
        } else {
            s_diag.recover_fail_count++;
            s_recovery_pending = 1U;
        }
    } else {
        note_recovery_duration();
    }
}

static void hard_recover_bus(void)
{
    recover_after_error();
    s_diag.hard_recover_count++;
    s_recovery_started_tick = tick_now();
    HAL_I2C_DeInit(&hi2c1);
    osDelay(I2C_SLAVE_RECOVERY_COOLDOWN_MS);
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        s_diag.recover_fail_count++;
        return;
    }
    reset_fsm_state();
    if (HAL_I2C_EnableListen_IT(&hi2c1) == HAL_OK) {
        s_diag.relisten_count++;
        note_recovery_duration();
    } else {
        s_diag.recover_fail_count++;
    }
}

static void schedule_abort_recovery(void)
{
    s_diag.abort_count++;
    s_recovery_started_tick = tick_now();
    s_recovery_pending = 1U;
}

static void poll_bus_health(void)
{
    uint32_t now = tick_now();
    uint8_t scl_low = i2c1_line_is_low(GPIO_PIN_6);
    uint8_t sda_low = i2c1_line_is_low(GPIO_PIN_7);
    HAL_I2C_StateTypeDef state = HAL_I2C_GetState(&hi2c1);

    if (tick_expired(now, s_phase_deadline_tick)) {
        s_diag.progress_timeout_count++;
        schedule_abort_recovery();
        clear_progress_watchdog();
        return;
    }

    if (((state & 0xFFU) == HAL_I2C_STATE_LISTEN) && tick_expired(now, s_last_progress_tick + I2C_SLAVE_IDLE_STUCK_TIMEOUT_MS)) {
        if (scl_low != 0U) {
            if (s_idle_scl_low_polls == 0U) {
                s_idle_scl_low_since_tick = now;
            }
            if (s_idle_scl_low_polls < 0xFFU) {
                s_idle_scl_low_polls++;
            }
        } else {
            s_idle_scl_low_polls = 0U;
            s_idle_scl_low_since_tick = 0U;
        }

        if (sda_low != 0U) {
            if (s_idle_sda_low_polls == 0U) {
                s_idle_sda_low_since_tick = now;
            }
            if (s_idle_sda_low_polls < 0xFFU) {
                s_idle_sda_low_polls++;
            }
        } else {
            s_idle_sda_low_polls = 0U;
            s_idle_sda_low_since_tick = 0U;
        }

        if ((s_idle_scl_low_polls >= I2C_SLAVE_STUCK_CONFIRM_POLLS)
         && tick_expired(now, s_idle_scl_low_since_tick + I2C_SLAVE_STUCK_CONFIRM_MS)) {
            s_diag.stuck_scl_count++;
            s_idle_scl_low_polls = 0U;
            s_idle_scl_low_since_tick = 0U;
            schedule_abort_recovery();
            return;
        }
        if ((s_idle_sda_low_polls >= I2C_SLAVE_STUCK_CONFIRM_POLLS)
         && tick_expired(now, s_idle_sda_low_since_tick + I2C_SLAVE_STUCK_CONFIRM_MS)) {
            s_diag.stuck_sda_count++;
            s_idle_sda_low_polls = 0U;
            s_idle_sda_low_since_tick = 0U;
            schedule_abort_recovery();
        }
        return;
    }

    s_idle_scl_low_polls = 0U;
    s_idle_sda_low_polls = 0U;
    s_idle_scl_low_since_tick = 0U;
    s_idle_sda_low_since_tick = 0U;
}
static void execute_pending_recovery(void)
{
    if (s_recovery_pending == 0U) {
        return;
    }

    s_recovery_pending = 0U;
    /* HAL_I2C_Slave_Abort_IT is not available on F4 HAL, falling back to hard reset */
    hard_recover_bus();
}

/* ---- public API --------------------------------------------------------- */
void app_i2c_slave_init(void)
{
    memset(s_ram, 0, sizeof(s_ram));
    memset(&s_diag, 0, sizeof(s_diag));
    recover_after_error();
    reset_fsm_state();
    /* Do NOT set an aggressive deadline here — guard task is already running.
     * Progress watchdog activates on first real I2C addr_callback. */
    clear_progress_watchdog();
}

uint8_t *app_i2c_slave_get_ram(void)
{
    return s_ram;
}

struct i2c_test_s *app_i2c_slave_get_test_state(void)
{
    return &s_i2c_test;
}

struct i2c_seq_ctrl_s *app_i2c_slave_get_seq_state(void)
{
    return &s_i2c_sec_ctrl_h;
}

const app_i2c_slave_diag_t *app_i2c_slave_get_diag(void)
{
    return &s_diag;
}

void app_i2c_slave_poll_recovery(void)
{
    poll_bus_health();
    execute_pending_recovery();
}

/* ---- diag export to I2C RAM 0xF0..0xFF --------------------------------- */
/*
 * Layout (16 bytes at I2C_REG_STM32_ERROR_ADDR = 0xF0):
 *   [0]  progress_timeout_count  (uint8 saturating)
 *   [1]  stuck_scl_count
 *   [2]  stuck_sda_count
 *   [3]  abort_count
 *   [4]  relisten_count
 *   [5]  hard_recover_count
 *   [6]  malformed_count
 *   [7]  recover_fail_count
 *   [8..9]  last_recovery_ms  (uint16 LE)
 *   [10..11] max_recovery_ms  (uint16 LE)
 *   [12] HAL I2C1 state
 *   [13] SCL pin level
 *   [14] SDA pin level
 *   [15] reserved (0)
 */
static uint8_t sat8(uint32_t v) { return (v > 255U) ? 255U : (uint8_t)v; }

void app_i2c_slave_sync_diag_to_ram(void)
{
    uint8_t *base = &s_ram[I2C_REG_STM32_ERROR_ADDR];
    base[0]  = sat8(s_diag.progress_timeout_count);
    base[1]  = sat8(s_diag.stuck_scl_count);
    base[2]  = sat8(s_diag.stuck_sda_count);
    base[3]  = sat8(s_diag.abort_count);
    base[4]  = sat8(s_diag.relisten_count);
    base[5]  = sat8(s_diag.hard_recover_count);
    base[6]  = sat8(s_diag.malformed_count);
    base[7]  = sat8(s_diag.recover_fail_count);
    uint16_t lr = (s_diag.last_recovery_ms > 0xFFFFU) ? 0xFFFFU : (uint16_t)s_diag.last_recovery_ms;
    uint16_t mr = (s_diag.max_recovery_ms  > 0xFFFFU) ? 0xFFFFU : (uint16_t)s_diag.max_recovery_ms;
    base[8]  = (uint8_t)(lr & 0xFF);
    base[9]  = (uint8_t)(lr >> 8);
    base[10] = (uint8_t)(mr & 0xFF);
    base[11] = (uint8_t)(mr >> 8);
    base[12] = (uint8_t)(HAL_I2C_GetState(&hi2c1) & 0xFFU);
    base[13] = (uint8_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6);
    base[14] = (uint8_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7);
    base[15] = 0U;
}

uint8_t app_i2c_slave_has_errors(void)
{
    return (s_diag.progress_timeout_count
          + s_diag.stuck_scl_count
          + s_diag.stuck_sda_count
          + s_diag.hard_recover_count
          + s_diag.malformed_count
          + s_diag.recover_fail_count) > 0U ? 1U : 0U;
}

/* ---- HMI diag line formatting (10 chars max) ---------------------------- */
#define DIAG_LINE_COUNT  8U

uint8_t app_i2c_slave_diag_line_count(void)
{
    return DIAG_LINE_COUNT;
}

void app_i2c_slave_format_diag_line(uint8_t index, char *buf, size_t buflen)
{
    if (buf == NULL || buflen < 11U) return;
    switch (index) {
    case 0: snprintf(buf, buflen, "TmOut:%04lu", (unsigned long)(s_diag.progress_timeout_count % 10000U)); break;
    case 1: snprintf(buf, buflen, "SclSt:%04lu", (unsigned long)(s_diag.stuck_scl_count % 10000U)); break;
    case 2: snprintf(buf, buflen, "SdaSt:%04lu", (unsigned long)(s_diag.stuck_sda_count % 10000U)); break;
    case 3: snprintf(buf, buflen, "Abort:%04lu", (unsigned long)(s_diag.abort_count % 10000U)); break;
    case 4: snprintf(buf, buflen, "HdRcv:%04lu", (unsigned long)(s_diag.hard_recover_count % 10000U)); break;
    case 5: snprintf(buf, buflen, "Malf :%04lu", (unsigned long)(s_diag.malformed_count % 10000U)); break;
    case 6: snprintf(buf, buflen, "RcFai:%04lu", (unsigned long)(s_diag.recover_fail_count % 10000U)); break;
    case 7: snprintf(buf, buflen, "MaxMs:%05lu", (unsigned long)(s_diag.max_recovery_ms % 100000U)); break;
    default: snprintf(buf, buflen, "----------"); break;
    }
}

/* ---- guard task --------------------------------------------------------- */
void StartTaskI2cGuard(void const *argument)
{
    (void)argument;
    for (;;) {
        app_i2c_slave_poll_recovery();
        process_deferred_actions();
        app_i2c_slave_sync_diag_to_ram();
        osDelay(5);
    }
}

static void process_host_message_from_isr(BaseType_t *prior)
{
    uint8_t psize = s_ram[HOST_MSG_RAM_ADDR];
    /* Validate header-declared length: min 1, max 11, and total must fit region */
    if (psize == 0U || psize >= 12U) {
        s_diag.malformed_count++;
        return;
    }
    if ((uint16_t)HOST_MSG_RAM_ADDR + 3U + (uint16_t)psize > sizeof(s_ram)) {
        s_diag.malformed_count++;
        return;
    }
    MsgHmi_t esp_msg = {
        .hmi_lock = s_ram[HOST_MSG_RAM_ADDR + 2],
        .msg_ttl  = s_ram[HOST_MSG_RAM_ADDR + 1],
        .psize    = psize,
    };
    memcpy(esp_msg.msg_buf, &s_ram[HOST_MSG_RAM_ADDR + 3], psize);
    xQueueSendFromISR(myQueueHmiMsgHandle, &esp_msg, prior);
    portYIELD_FROM_ISR(*prior);
}

static void process_auth_result_write(void)
{
    /* ESP contract: write [0x70, len=5, payload...], where only payload[0] is AuthResult_t.
     * payload[1..4] are accepted but intentionally ignored on STM32 side. */
    uint8_t auth_result = s_ram[HOST_AUTH_RESULT_RAM_ADDR];

    /* Always notify HMI — handles page switch for AUTH=1 and optional display */
    hmi_show_auth_result(auth_result);

    if (auth_result == 0x00U) {
        HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
        osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER1_MS);
    } else if (auth_result == 0x01U) {
        relay_request_pulse(RELAY_SRC_AUTH_OK);
    } else {
        /* AUTH_RESULT_BUSY and any unknown future values are accepted with no side effects. */
    }
}

/* ---- Deferred processing flags (ISR sets, task processes) -------------- */
#define DEFERRED_NONE           0x00U
#define DEFERRED_TIME_SYNC      0x01U
#define DEFERRED_AUTH_RESULT    0x02U
static volatile uint8_t s_deferred_action = DEFERRED_NONE;

static void process_register_write_from_isr(uint8_t base, BaseType_t *prior)
{
    if (base == HOST_MSG_RAM_ADDR) {
        /* ISR-safe: xQueueSendFromISR */
        process_host_message_from_isr(prior);
    } else if (base == I2C_REG_HW_TIME_SET_ADDR) {
        /* DEFER: service_time_sync_from_master uses blocking I2C2 + mutex.
         * Data is already in s_ram[0x88..]. Guard/RxTx task will pick up. */
        s_deferred_action |= DEFERRED_TIME_SYNC;
    } else if (base == I2C_REG_COUNTER_ADDR || base == I2C_REG_CFG_ADDR) {
        /* Keep old working behavior: apply config immediately on write completion. */
        runtime_config_apply_from_ram(s_ram);
    } else if (base == HOST_AUTH_RESULT_RAM_ADDR) {
        /* Keep auth processing deferred to task context because it touches timers/HMI/relay. */
        s_deferred_action |= DEFERRED_AUTH_RESULT;
    }
}

/* Called from task context (guard task) to execute deferred ISR work */
static void process_deferred_actions(void)
{
    uint8_t action;
    
    /* Critical section: read and clear flags to avoid race with ISR */
    __disable_irq();
    action = s_deferred_action;
    s_deferred_action = DEFERRED_NONE;
    __enable_irq();

    if (action == DEFERRED_NONE) {
        return;
    }

    if (action & DEFERRED_TIME_SYNC) {
        service_time_sync_from_master(&s_ram[I2C_REG_HW_TIME_SET_ADDR],
                                      I2C_TIME_SYNC_WRITE_LEN);
        service_time_sync_datetimepack();
    }
    if (action & DEFERRED_AUTH_RESULT) {
        process_auth_result_write();
    }
}

void app_i2c_slave_publish(const I2cPacketToMaster_t *pckt)
{
    uint8_t target_reg;
    uint8_t target_len;

    if (pckt == NULL) {
        return;
    }

    target_reg = packet_reg_for_type(pckt->type);
    target_len = packet_len_for_type(pckt->type, pckt->len);

    s_ram[I2C_PACKET_TYPE_ADDR] = (uint8_t)pckt->type;
    if ((target_len > 0U) && (pckt->payload != NULL)) {
        memset(&s_ram[target_reg], 0, target_len);
        memcpy(&s_ram[target_reg], pckt->payload, (pckt->len < target_len) ? pckt->len : target_len);
    }

    s_outbox_busy = 1U;
    s_outbox_type = (uint8_t)pckt->type;
    s_outbox_reg  = target_reg;
    s_outbox_len  = target_len;
    s_last_tx_reg = 0xFFU;
    s_last_tx_len = 0U;
    HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_RESET);
    s_event_latched = 1U;
}

void StartTaskRxTxI2c1(void const *argument)
{
    I2cPacketToMaster_t pckt;
    const char *bus_error_msg = "BUS ERROR       ";  /* padded to 16 chars for VP 0x5200 */
    (void)argument;

    app_i2c_slave_poll_recovery();
    if (HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK) {
        Error_Handler();
    }
    s_diag.relisten_count++;

    for (;;) {
        uint16_t count = 0U;
        HAL_I2C_StateTypeDef i2c1_state;
        xQueueReceive(myQueueToMasterHandle, &pckt, osWaitForever);

        /* Release the TIME-packet coalesce slot as soon as the entry is
         * dequeued so that the next 1Hz tick can re-arm immediately. */
        if (pckt.type == PACKET_TIME) {
            service_time_sync_packet_consumed();
        }

        /* TTL expiry: discard packets that have been waiting longer than
         * TTL_PACKET_SEC seconds (measured by the DS3231 1Hz uptime counter).
         * Expired packets are silently dropped and the next queued entry is
         * fetched immediately. */
        if (service_time_sync_get_uptime_sec() >= pckt.ttl) {
            continue;
        }

        do {
            app_i2c_slave_poll_recovery();
            i2c1_state = HAL_I2C_GetState(&hi2c1);
            count++;
            if (count > 20U) {
                dwin_text_output(0x5200, (const uint8_t*)bus_error_msg, strlen(bus_error_msg));
                count = 0U;
                hard_recover_bus();
            }
            osDelay(10);
        } while ((i2c1_state & 0xFFU) != HAL_I2C_STATE_LISTEN);

        while (s_outbox_busy != 0U) {
            app_i2c_slave_poll_recovery();
            osDelay(I2C_SLAVE_OUTBOX_RETRY_DELAY_MS);
        }

        osDelay(I2C_SLAVE_PUBLISH_PRE_DELAY_MS);
        app_i2c_slave_publish(&pckt);
        osDelay(I2C_SLAVE_PUBLISH_POST_DELAY_MS);
    }
}

/* ---- HAL delegates ------------------------------------------------------ */
void app_i2c_slave_abort_complete(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &hi2c1) return;
    recover_after_error();
    if (HAL_I2C_EnableListen_IT(hi2c) == HAL_OK) {
        s_diag.relisten_count++;
        note_recovery_duration();
    } else {
        s_diag.recover_fail_count++;
        s_recovery_pending = 1U;
    }
}

void app_i2c_slave_listen_complete(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &hi2c1) return;

    reset_fsm_state();
    if (HAL_I2C_EnableListen_IT(hi2c) == HAL_OK) {
        s_diag.relisten_count++;
        note_recovery_duration();
    } else {
        s_diag.recover_fail_count++;
        s_recovery_pending = 1U;
    }
}

void app_i2c_slave_addr_callback(I2C_HandleTypeDef *hi2c, uint8_t Direction, uint16_t AddrMatch)
{
    UNUSED(AddrMatch);
    if (hi2c != &hi2c1) return;

    s_i2c_test.adr_count++;
    note_progress(I2C_SLAVE_HEADER_TIMEOUT_MS);

    if (Direction == I2C_DIRECTION_TRANSMIT) {
        reset_fsm_state();
        note_progress(I2C_SLAVE_HEADER_TIMEOUT_MS);
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, &s_i2c_sec_ctrl_h.offset, 1, I2C_FIRST_FRAME);
        s_i2c_test.rcv_start++;
    } else {
        if ((s_i2c_sec_ctrl_h.first == 0U)
         && (s_i2c_sec_ctrl_h.second == 0U)
         && (s_i2c_sec_ctrl_h.final == 1U)
         && (validate_read_request(s_i2c_sec_ctrl_h.offset, s_i2c_sec_ctrl_h.rx_count) != 0U)) {
            s_i2c_sec_ctrl_h.tx_count = s_i2c_sec_ctrl_h.rx_count;
            s_last_tx_reg = s_i2c_sec_ctrl_h.offset;
            s_last_tx_len = s_i2c_sec_ctrl_h.tx_count;
            note_progress(I2C_SLAVE_PAYLOAD_TIMEOUT_MS);
            HAL_I2C_Slave_Seq_Transmit_IT(hi2c, &s_ram[s_i2c_sec_ctrl_h.offset], (uint16_t)s_i2c_sec_ctrl_h.tx_count, I2C_LAST_FRAME);
        } else {
            mark_malformed_and_recover();
        }
    }
}

void app_i2c_slave_rx_complete(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &hi2c1) return;

    s_i2c_test.rcv_cplt++;
    note_progress(I2C_SLAVE_HEADER_TIMEOUT_MS);

    if (s_i2c_sec_ctrl_h.first) {
        s_i2c_sec_ctrl_h.first = 0U;
        s_i2c_sec_ctrl_h.second = 1U;
        s_i2c_sec_ctrl_h.final = 0U;
        s_i2c_sec_ctrl_h.rx_count = 0U;
        s_i2c_sec_ctrl_h.tx_count = 0U;
        s_i2c_sec_ctrl_h.last_base_ram_rcvd_addr = s_i2c_sec_ctrl_h.offset;
        if (is_known_register(s_i2c_sec_ctrl_h.offset) == 0U) {
            mark_malformed_and_recover();
            return;
        }
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, &s_i2c_sec_ctrl_h.rx_count, 1, I2C_NEXT_FRAME);
        s_i2c_test.rcv_start++;
        return;
    }

    if (s_i2c_sec_ctrl_h.second) {
        uint8_t strict_len;
        s_i2c_sec_ctrl_h.first = 0U;
        s_i2c_sec_ctrl_h.second = 0U;
        s_i2c_sec_ctrl_h.final = 1U;

        strict_len = strict_rx_len_for_register(s_i2c_sec_ctrl_h.offset);
        if ((strict_len != 0U) && (s_i2c_sec_ctrl_h.rx_count != strict_len)) {
            mark_malformed_and_recover();
            return;
        }
        if ((uint16_t)s_i2c_sec_ctrl_h.offset + (uint16_t)s_i2c_sec_ctrl_h.rx_count > sizeof(s_ram)) {
            mark_malformed_and_recover();
            return;
        }
        if (s_i2c_sec_ctrl_h.rx_count == 0U) {
            clear_progress_watchdog();
            return;
        }

        note_progress(I2C_SLAVE_PAYLOAD_TIMEOUT_MS);
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, &s_ram[s_i2c_sec_ctrl_h.offset], s_i2c_sec_ctrl_h.rx_count, I2C_LAST_FRAME);
        s_i2c_test.rcv_start++;
        return;
    }

    if (s_i2c_sec_ctrl_h.final) {
        uint8_t base = s_i2c_sec_ctrl_h.last_base_ram_rcvd_addr;
        BaseType_t prior = pdFALSE;
        s_i2c_sec_ctrl_h.final = 0U;
        clear_progress_watchdog();
        process_register_write_from_isr(base, &prior);
    }
}

void app_i2c_slave_tx_complete(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &hi2c1) return;
    s_i2c_sec_ctrl_h.final = 0U;
    clear_progress_watchdog();
    if ((s_outbox_busy != 0U) && (s_last_tx_reg == s_outbox_reg) && (s_last_tx_len >= s_outbox_len)) {
        outbox_complete_ack();
    }
}

void app_i2c_slave_error(I2C_HandleTypeDef *hi2c)
{
    uint32_t errorcode;

    if (hi2c != &hi2c1) return;

    errorcode = HAL_I2C_GetError(hi2c);

    if (errorcode == HAL_I2C_ERROR_AF) {
        if ((s_last_tx_reg != 0xFFU) && (s_last_tx_len > 0U) && (s_i2c_sec_ctrl_h.final != 0U)) {
            s_i2c_sec_ctrl_h.final = 0U;
            clear_progress_watchdog();
            restart_listen_if_needed();
            return;
        }
        mark_malformed_and_recover();
        return;
    }

    /* Keep robust recovery for real bus errors. */
    schedule_abort_recovery();
}
