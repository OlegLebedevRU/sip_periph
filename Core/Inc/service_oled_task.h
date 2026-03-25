/*
 * service_oled_task.h
 *
 * OLED SSD1306 display task module.
 * Владеет:
 *   - StartTaskOLED (FreeRTOS task body)
 *
 * Перенесено из main.c (шаг 11 рефакторинга).
 */

#ifndef INC_SERVICE_OLED_TASK_H_
#define INC_SERVICE_OLED_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FreeRTOS task body — зарегистрировать как osThreadDef(myTaskOLED, StartTaskOLED, ...).
 */
void StartTaskOLED(void const *argument);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_OLED_TASK_H_ */
