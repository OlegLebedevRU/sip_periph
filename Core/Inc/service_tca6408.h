/*
 * service_tca6408.h
 *
 * Сервис обработки TCA6408A по сигналу ~INT -> STM32 EXT_INT.
 *
 * Назначение входов:
 *   P0 = DS3231M SQW 1Hz (активный LOW)
 *   P2 = внешняя кнопка BTN_OPEN (активный LOW, с debounce)
 *   P3 = PN532 IRQ (активный LOW, рабочая полярность сохранена)
 */

#ifndef INC_SERVICE_TCA6408_H_
#define INC_SERVICE_TCA6408_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

void service_tca6408_init(void);
void service_tca6408_post_bootstrap(void);
void service_tca6408_process_irq_event(void);

/* FreeRTOS task body — registered as osThreadDef(myTask_tca6408a, ...) in main.c */
void StartTasktca6408a(void const *argument);

/* FreeRTOS task body — I2C2 bus health watchdog using DS3231 1Hz heartbeat */
void StartTaskI2c2Guard(void const *argument);

HAL_StatusTypeDef service_tca6408_write_reg(uint8_t reg_addr, uint8_t data);
HAL_StatusTypeDef service_tca6408_read_reg(uint8_t reg_addr, uint8_t *data);

HAL_StatusTypeDef service_tca6408_get_last_hal_status(void);
uint32_t service_tca6408_get_last_i2c_error(void);
uint32_t service_tca6408_get_hard_recover_count(void);

/* Called by pn532_com after each I2C2 operation so PN532 errors feed the
 * same soft/hard recovery state machine used by TCA6408A transactions. */
void service_tca6408_i2c2_recover_if_needed(HAL_StatusTypeDef status);

bool service_tca6408_is_button_debounce_active(void);
uint8_t service_tca6408_get_last_inputs(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_TCA6408_H_ */