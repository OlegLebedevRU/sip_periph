/*
 * service_pn532_task.h
 *
 * PN532 NFC reader task module.
 * Владеет:
 *   - StartTask532 (FreeRTOS task body)
 *   - pn532_t handle, slaveTxData, uid, response buffers
 *   - I2C2 fault flag for PN532
 *
 * Перенесено из main.c (шаг 10 рефакторинга).
 */

#ifndef INC_SERVICE_PN532_TASK_H_
#define INC_SERVICE_PN532_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * Инициализация внутреннего состояния (pn532 handle, буферы).
 * Вызывать однократно из StartDefaultTask до цикла.
 */
void service_pn532_init(void);

/*
 * FreeRTOS task body — зарегистрировать как osThreadDef(myTask532, StartTask532, ...).
 */
void StartTask532(void const *argument);

/*
 * Уведомить модуль об ошибке I2C2 (вызывается из HAL_I2C_ErrorCallback, hi2c2 ветка).
 * Устанавливает внутренний флаг pn_i2c_fault = 1 для повторной инициализации PN532.
 */
void service_pn532_notify_i2c_fault(void);

/*
 * Получить указатель на slaveTxData[] (нужен для обратной совместимости).
 * TODO: убрать после перехода на copy-into-outbox.
 */
uint8_t *service_pn532_get_slaveTxData(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_PN532_TASK_H_ */
