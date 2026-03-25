/*
 * app_uart_dwin.c
 *
 * DWIN HMI UART receive FSM (USART2, 115200 baud).
 *
 * Протокол DWIN: каждый пакет начинается с заголовка [5A A5 LL],
 * затем идёт LL байт тела. Итого: 3 + LL байт.
 *
 * Замена malloc → статический пул из DWIN_POOL_SIZE буферов.
 * ISR берёт свободный слот, задача освобождает через dwin_buf_free().
 * При переполнении пула пакет отбрасывается (не вызывает Hard Fault).
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "app_uart_dwin.h"

/* ---- Константы --------------------------------------------------------- */
#define DWIN_HDR_SIZE    3U
#define DWIN_MAX_BODY    32U
#define DWIN_PKT_MAX     (DWIN_HDR_SIZE + DWIN_MAX_BODY)  /* 35 байт */
#define DWIN_POOL_SIZE   4U   /* глубина пула: 4 пакета в полёте одновременно */

/* ---- Статический пул буферов ------------------------------------------ */
static uint8_t s_pool_buf[DWIN_POOL_SIZE][DWIN_PKT_MAX];
static uint8_t s_pool_used[DWIN_POOL_SIZE];  /* 0=свободен, 1=занят */

/* ---- FSM state --------------------------------------------------------- */
static uint8_t s_dwin_hdr[DWIN_HDR_SIZE];
static uint8_t s_dwin_body[DWIN_MAX_BODY];
static uint8_t s_dwin_phase = 0;  /* 0=ждём header, 1=ждём body */

/* ---- extern UART handle и очередь (объявлены в main.c) ---------------- */
extern UART_HandleTypeDef huart2;
extern osMessageQId myQueueHMIRecvRawHandle;

/* ---- Внутренние функции ------------------------------------------------ */

/* Взять слот из пула (вызывается из ISR) */
static uint8_t *pool_alloc_from_isr(void)
{
    for (uint8_t i = 0U; i < DWIN_POOL_SIZE; i++) {
        if (s_pool_used[i] == 0U) {
            s_pool_used[i] = 1U;
            return s_pool_buf[i];
        }
    }
    return NULL;  /* пул исчерпан — пакет будет отброшен */
}

/* ---- Публичные функции ------------------------------------------------- */

void dwin_buf_free(uint8_t *buf)
{
    if (buf == NULL) return;
    for (uint8_t i = 0U; i < DWIN_POOL_SIZE; i++) {
        if (s_pool_buf[i] == buf) {
            s_pool_used[i] = 0U;
            return;
        }
    }
    /* Если указатель не из пула (legacy malloc) — игнорируем.
     * После полного перехода на пул эта ветка не достигается. */
}

void dwin_uart_start(void)
{
    s_dwin_phase = 0U;
    HAL_UART_Receive_IT(&huart2, s_dwin_hdr, DWIN_HDR_SIZE);
}

void app_uart_dwin_rx_callback(UART_HandleTypeDef *huart)
{
    if (huart != &huart2) return;

    BaseType_t prior = pdFALSE;

    if (s_dwin_phase == 0U) {
        /* Phase 0: получен заголовок — проверяем magic и длину тела */
        if (s_dwin_hdr[0] == 0x5AU && s_dwin_hdr[1] == 0xA5U) {
            uint8_t body_len = s_dwin_hdr[2];
            if (body_len > 0U && body_len <= DWIN_MAX_BODY) {
                s_dwin_phase = 1U;
                HAL_UART_Receive_IT(&huart2, s_dwin_body, body_len);
                return;
            }
        }
        /* Плохой заголовок — перезапустить */
        s_dwin_phase = 0U;
        HAL_UART_Receive_IT(&huart2, s_dwin_hdr, DWIN_HDR_SIZE);

    } else {
        /* Phase 1: получено тело — собрать пакет в слот пула */
        uint8_t body_len = s_dwin_hdr[2];
        uint16_t total   = (uint16_t)(DWIN_HDR_SIZE + body_len);

        uint8_t *pkt = pool_alloc_from_isr();
        if (pkt != NULL) {
            memcpy(pkt,                  s_dwin_hdr,  DWIN_HDR_SIZE);
            memcpy(pkt + DWIN_HDR_SIZE,  s_dwin_body, body_len);
            MsgUart_t msg = { .psize = total, .uart_buf = pkt };
            xQueueSendFromISR(myQueueHMIRecvRawHandle, &msg, &prior);
            portYIELD_FROM_ISR(prior);
        }
        /* Немедленно перезапустить приём следующего заголовка */
        s_dwin_phase = 0U;
        HAL_UART_Receive_IT(&huart2, s_dwin_hdr, DWIN_HDR_SIZE);
    }
}
