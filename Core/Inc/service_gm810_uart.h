#ifndef INC_SERVICE_GM810_UART_H_
#define INC_SERVICE_GM810_UART_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

typedef struct {
	uint32_t init_calls;
	uint32_t start_calls;
	uint32_t restart_calls;
	uint32_t irq_callbacks;
	uint32_t rx_callbacks;
	uint32_t error_callbacks;
	uint32_t start_retry_count;
	uint32_t start_delay_ms;
	uint32_t last_start_tick;
	uint32_t ready_tick;
	uint32_t boot_flush_bytes;
	uint32_t last_irq_tick;
	uint32_t last_rx_tick;
	uint32_t last_rx_byte;
	uint32_t last_uart_error;
	uint32_t last_receive_status;
	uint32_t last_rx_state;
	uint32_t last_g_state;
	uint32_t last_usart_sr;
	uint32_t last_usart_cr1;
	uint32_t last_usart_cr3;
	uint32_t last_gpioa_moder;
	uint32_t last_gpioa_afr_high;
	uint32_t boot_verify_calls;
	uint32_t boot_configure_calls;
	uint32_t boot_save_calls;
	uint32_t boot_failures;
	uint32_t boot_last_mismatch_addr;
	uint8_t  boot_state;
} service_gm810_uart_diag_t;

void service_gm810_uart_init(void);
void service_gm810_uart_start(void);
void service_gm810_uart_irq_callback(UART_HandleTypeDef *huart);
void service_gm810_uart_rx_callback(UART_HandleTypeDef *huart);
void service_gm810_uart_error_callback(UART_HandleTypeDef *huart);
const volatile service_gm810_uart_diag_t *service_gm810_uart_get_diag(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_GM810_UART_H_ */
