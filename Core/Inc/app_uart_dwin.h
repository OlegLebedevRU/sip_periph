/*
 * app_uart_dwin.h
 *
 * DWIN HMI UART receive FSM.
 * Двухфазный приём пакетов: header [5A A5 LL] → body [LL bytes].
 * ISR-безопасный статический пул буферов (без malloc/free в ISR).
 */

#ifndef INC_APP_UART_DWIN_H_
#define INC_APP_UART_DWIN_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/*
 * Запустить FSM приёма DWIN пакетов.
 * Вызывать однократно из StartTaskHmi перед основным циклом.
 */
void dwin_uart_start(void);

/*
 * Освободить буфер пула после обработки пакета.
 * Вызывать вместо free(uart_msg.uart_buf) в StartTaskHmi.
 */
void dwin_buf_free(uint8_t *buf);

/*
 * Делегат для HAL_UART_RxCpltCallback.
 * Вызывать: app_uart_dwin_rx_callback(huart) внутри HAL_UART_RxCpltCallback.
 */
void app_uart_dwin_rx_callback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_UART_DWIN_H_ */
