#ifndef INC_SERVICE_GM810_UART_H_
#define INC_SERVICE_GM810_UART_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

void service_gm810_uart_init(void);
void service_gm810_uart_start(void);
void service_gm810_uart_rx_callback(UART_HandleTypeDef *huart);
void service_gm810_uart_error_callback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_GM810_UART_H_ */
