/*
 * app_irq_router.h
 *
 * Тонкий ISR-маршрутизатор — единственная точка входа для всех
 * GPIO EXTI прерываний. Делегирует обработку в очереди FreeRTOS,
 * не содержит прикладной логики.
 *
 * Для TCA6408A: ISR только маршрутизирует EXT_INT в очередь,
 * а чтение reg[0] и интерпретация P0/P2/P3 выполняется в service_tca6408.
 */

#ifndef INC_APP_IRQ_ROUTER_H_
#define INC_APP_IRQ_ROUTER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * Вызывать из HAL_GPIO_EXTI_Callback(GPIO_Pin).
 * Маршрутизирует события:
 *   EXT_INT_Pin  → myQueueTCA6408Handle  (TCA6408A IRQ: DS/PN532/кнопка)
 *   W_D0/W_D1    → myQueueWiegandHandle  + TIM11 PWM start
 *   ROW1..ROW4   → matrix_kbd_exti_from_isr()  (scan + queue)
 */
void app_irq_router_exti_callback(uint16_t GPIO_Pin);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_IRQ_ROUTER_H_ */