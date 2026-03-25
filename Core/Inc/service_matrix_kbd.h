/*
 * service_matrix_kbd.h
 *
 * Matrix keyboard service.
 * Владеет:
 *   - scan logic из EXTI ISR
 *   - debounce/final_input state
 *   - key buffer used by OLED + PACKET_PIN
 */

#ifndef INC_SERVICE_MATRIX_KBD_H_
#define INC_SERVICE_MATRIX_KBD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cmsis_os.h"

void service_matrix_kbd_init(void);
void matrix_kbd_exti_from_isr(uint16_t GPIO_Pin, BaseType_t *pxHigherPriorityTaskWoken);
void cb_keyTimer(void const *argument);

/* Нужен OLED task и временному legacy-коду до шага 9 */
const struct keyb *service_matrix_kbd_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_MATRIX_KBD_H_ */
