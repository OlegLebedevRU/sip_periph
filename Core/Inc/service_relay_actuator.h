/*
 * service_relay_actuator.h
 *
 * Сервис управления реле и зуммером.
 * Единая точка входа для:
 *   - auth result от мастера (ESP32 -> reg 0x70)
 *   - внешней кнопки (TCA P2, после debounce)
 */

#ifndef INC_SERVICE_RELAY_ACTUATOR_H_
#define INC_SERVICE_RELAY_ACTUATOR_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RELAY_SRC_AUTH_OK = 0,
    RELAY_SRC_EXT_BTN,
} relay_source_t;

/* Инициализация внутреннего состояния. Вызывать 1 раз из StartDefaultTask. */
void service_relay_actuator_init(void);

/* Запросить импульс реле от указанного источника. */
void relay_request_pulse(relay_source_t source);

/* Освободить внешний источник после отпускания кнопки. */
void relay_ext_button_release(void);

/* Read-only status for TCA button arbitration/suppress logic. */
bool relay_is_relay1_active(void);
bool relay_is_ext_button_flow_blocked(void);

/* Timer callbacks — регистрируются в main.c */
void cb_TmReleBefore(void const *argument);
void cb_TmReleAct(void const *argument);
void cb_Tm_buzzerOff(void const *argument);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_RELAY_ACTUATOR_H_ */