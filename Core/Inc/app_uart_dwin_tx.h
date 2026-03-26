/*
 * app_uart_dwin_tx.h
 *
 * Shared DWIN USART2 TX transport.
 *
 * All DWIN TX traffic (text frames from hmi.c, graphic commands from
 * dwin_gfx.c) must go through this module to avoid concurrent access
 * to the same UART peripheral.
 *
 * The transport owns:
 *   - a common TX mutex (FreeRTOS osMutex)
 *   - a common TX buffer
 *   - bounded polling on huart2.gState before HAL_UART_Transmit_IT
 *
 * Created: 2026-03-26  (Stage 3 refactor)
 */

#ifndef INC_APP_UART_DWIN_TX_H_
#define INC_APP_UART_DWIN_TX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx_hal.h"

/* Maximum payload that can be sent in a single DWIN frame.
 * Header (5A A5 LL) = 3 bytes already counted by caller;
 * the transport buffer must hold the complete frame. */
#define DWIN_TX_BUF_SIZE   256U

/* Default wait parameters (can be tuned) */
#define DWIN_TX_WAIT_RETRIES   20U
#define DWIN_TX_WAIT_DELAY_MS  10U

/**
 * Initialise the shared TX transport (creates the mutex).
 * Safe to call multiple times — only the first call creates resources.
 * Must be called from a task context (not ISR).
 */
void dwin_tx_init(void);

/**
 * Send a pre-built frame through USART2 (IT-based).
 *
 * The function:
 *   1. Acquires the shared TX mutex (blocks indefinitely).
 *   2. Copies `len` bytes from `frame` into the internal TX buffer.
 *   3. Waits (bounded) for huart2.gState == READY.
 *   4. Calls HAL_UART_Transmit_IT on the internal buffer.
 *   5. Releases the mutex.
 *
 * @param frame  Pointer to the complete DWIN frame (including 5A A5 header).
 * @param len    Total frame length in bytes (must be <= DWIN_TX_BUF_SIZE).
 * @return       HAL status from HAL_UART_Transmit_IT, or HAL_ERROR on bad input.
 *
 * Thread-safe.  Must NOT be called from ISR context.
 */
HAL_StatusTypeDef dwin_tx_send(const uint8_t *frame, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_UART_DWIN_TX_H_ */
