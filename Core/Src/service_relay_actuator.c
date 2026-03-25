/*
 * service_relay_actuator.c
 *
 * Реле + зуммер.
 * Источники импульса:
 *   - RELAY_SRC_AUTH_OK : успешная авторизация от мастера
 *   - RELAY_SRC_EXT_BTN : внешняя кнопка через TCA P2 (после debounce)
 */

#include "main.h"
#include "cmsis_os.h"
#include <stdbool.h>
#include "service_runtime_config.h"
#include "service_relay_actuator.h"

/* ---- extern timers from main.c ---------------------------------------- */
extern osTimerId myTimerReleBeforeHandle;
extern osTimerId myTimerReleActHandle;
extern osTimerId myTimerBuzzerOffHandle;

/* ---- internal state ---------------------------------------------------- */
static uint8_t s_ext_btn_flag = 0U;
static uint8_t s_ext_btn_pulse_active = 0U;
static uint8_t s_auth_pulse_active = 0U;

void service_relay_actuator_init(void)
{
    s_ext_btn_flag = 0U;
    s_ext_btn_pulse_active = 0U;
    s_auth_pulse_active = 0U;
}

void relay_request_pulse(relay_source_t source)
{
    const runtime_config_t *cfg = runtime_config_get();

    if (source == RELAY_SRC_EXT_BTN) {
        if ((cfg == NULL) || (cfg->relay_pulse_en == 0U)) {
            return;
        }
        if (s_ext_btn_flag != 0U) {
            return;
        }
        s_ext_btn_flag = 1U;
        s_ext_btn_pulse_active = 1U;
        HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
        osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
        osTimerStart(myTimerReleBeforeHandle, 10U);
        return;
    }

    /* RELAY_SRC_AUTH_OK */
    s_auth_pulse_active = 1U;
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
    osTimerStart(myTimerBuzzerOffHandle, BUZZER_TIMER2_MS);
    if (cfg != NULL) {
        uint32_t before_ms = (uint32_t)cfg->relay_before_100ms * 100U;
        if (before_ms == 0U) before_ms = 1U;  /* FreeRTOS requires > 0 */
        osTimerStart(myTimerReleBeforeHandle, before_ms);
    } else {
        uint32_t before_ms = (uint32_t)RELE1_BEFORE_100MS_DEFAULT * 100U;
        if (before_ms == 0U) before_ms = 1U;
        osTimerStart(myTimerReleBeforeHandle, before_ms);
    }
}

void relay_ext_button_release(void)
{
    s_ext_btn_flag = 0U;
}

bool relay_is_relay1_active(void)
{
    return (s_ext_btn_pulse_active != 0U) || (s_auth_pulse_active != 0U);
}

bool relay_is_ext_button_flow_blocked(void)
{
    return s_ext_btn_pulse_active != 0U;
}

void cb_TmReleBefore(void const *argument)
{
    const runtime_config_t *cfg = runtime_config_get();
    uint8_t relay_pulse_en = (cfg != NULL) ? cfg->relay_pulse_en : RELE1_MODE_FLAG_DEFAULT;
    uint8_t relay_act_sec  = (cfg != NULL) ? cfg->relay_act_sec  : RELE1_ACT_SEC_DEFAULT;
    uint32_t act_ms;
    (void)argument;

    if (s_ext_btn_pulse_active != 0U) {
        /* External button pulse — gated by relay_pulse_en (already checked in relay_request_pulse) */
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_SET);
    } else if (s_auth_pulse_active != 0U) {
        /* Auth result pulse — always unconditional */
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_SET);
    } else if (relay_pulse_en == 1U) {
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_RESET);
    }

    act_ms = (uint32_t)relay_act_sec * 1000U;
    if (act_ms == 0U) act_ms = 1U;  /* FreeRTOS requires > 0 */
    osTimerStart(myTimerReleActHandle, act_ms);
}

void cb_TmReleAct(void const *argument)
{
    const runtime_config_t *cfg = runtime_config_get();
    uint8_t relay_pulse_en = (cfg != NULL) ? cfg->relay_pulse_en : RELE1_MODE_FLAG_DEFAULT;
    (void)argument;

    if (s_ext_btn_pulse_active != 0U) {
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_RESET);
        s_ext_btn_pulse_active = 0U;
    } else if (s_auth_pulse_active != 0U) {
        /* Auth result pulse — always deactivate relay */
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_RESET);
        s_auth_pulse_active = 0U;
    } else if (relay_pulse_en == 1U) {
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_RESET);
    } else {
        HAL_GPIO_WritePin(RELE1_GPIO_Port, RELE1_Pin, GPIO_PIN_SET);
    }
}

void cb_Tm_buzzerOff(void const *argument)
{
    (void)argument;
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
}