/*
 * app_uart_dwin_tx.c
 *
 * Shared DWIN USART2 TX transport — single point of serialisation
 * for all DWIN writes (text frames, graphic commands, register writes).
 *
 * Design:
 *   - One FreeRTOS mutex protects concurrent callers.
 *   - One static TX buffer avoids lifetime issues with IT-based transmit.
 *   - Bounded polling on huart2.gState keeps the same low-risk style
 *     already used elsewhere in the project.
 *
 * Created: 2026-03-26  (Stage 3 refactor)
 */

#include "app_uart_dwin_tx.h"
#include "cmsis_os.h"
#include <string.h>

/* ---- Extern UART handle (declared in main.c / CubeMX) ----------------- */
extern UART_HandleTypeDef huart2;

/* ---- Private state ----------------------------------------------------- */
static uint8_t  s_tx_buf[DWIN_TX_BUF_SIZE];
static osMutexId s_tx_mutex = NULL;

/* ---- Public API -------------------------------------------------------- */

void dwin_tx_init(void)
{
    if (s_tx_mutex != NULL) {
        return;  /* already initialised */
    }
    osMutexDef(dwinTxMtx);
    s_tx_mutex = osMutexCreate(osMutex(dwinTxMtx));
}

HAL_StatusTypeDef dwin_tx_send(const uint8_t *frame, uint16_t len)
{
    HAL_StatusTypeDef rc;

    if (frame == NULL || len == 0U || len > DWIN_TX_BUF_SIZE) {
        return HAL_ERROR;
    }

    /* Lazy init — safe because osMutexCreate is reentrant-safe on first
     * call from task context, and callers are always tasks. */
    if (s_tx_mutex == NULL) {
        dwin_tx_init();
    }
    if (s_tx_mutex == NULL) {
        return HAL_ERROR;
    }

    if (osMutexWait(s_tx_mutex, osWaitForever) != osOK) {
        return HAL_ERROR;
    }

    /* Bounded wait for previous IT transmit to finish BEFORE overwriting
     * the shared buffer — HAL_UART_Transmit_IT reads from s_tx_buf in
     * interrupt context, so we must not touch it while TX is in flight. */
    for (uint8_t r = 0U; r < DWIN_TX_WAIT_RETRIES; r++) {
        if (huart2.gState == HAL_UART_STATE_READY) {
            break;
        }
        osDelay(DWIN_TX_WAIT_DELAY_MS);
    }

    /* Copy frame into the internal buffer so caller's stack/local buffer
     * can be reused immediately after this function returns. */
    memcpy(s_tx_buf, frame, len);

    rc = HAL_UART_Transmit_IT(&huart2, s_tx_buf, len);

    osMutexRelease(s_tx_mutex);
    return rc;
}
