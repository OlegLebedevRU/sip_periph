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

HAL_StatusTypeDef service_tca6408_write_reg(uint8_t reg_addr, uint8_t data);
HAL_StatusTypeDef service_tca6408_read_reg(uint8_t reg_addr, uint8_t *data);

HAL_StatusTypeDef service_tca6408_get_last_hal_status(void);
uint32_t service_tca6408_get_last_i2c_error(void);

bool service_tca6408_is_button_debounce_active(void);
uint8_t service_tca6408_get_last_inputs(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_TCA6408_H_ */