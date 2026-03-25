/*
 * app_irq_router.c
 *
 * Тонкий ISR-маршрутизатор для HAL_GPIO_EXTI_Callback.
 * Правило: только xQueueSendFromISR / portYIELD_FROM_ISR, никакой бизнес-логики.
 */

#include "main.h"
#include "cmsis_os.h"
#include "app_irq_router.h"
#include "service_matrix_kbd.h"

/* ---- extern RTOS handles (объявлены в main.c) -------------------------- */
extern osMessageQId myQueueTCA6408Handle;
extern osMessageQId myQueueWiegandHandle;
extern TIM_HandleTypeDef htim11;

void app_irq_router_exti_callback(uint16_t GPIO_Pin)
{
    BaseType_t prior = pdFALSE;

    /* --- TCA6408A IRQ: все сигналы через один пин PC13 ------------------- */
    if (GPIO_Pin == EXT_INT_Pin) {
        uint16_t tca = 1U;
        xQueueSendFromISR(myQueueTCA6408Handle, &tca, &prior);
        portYIELD_FROM_ISR(prior);
        return;
    }

    /* --- Wiegand D0 / D1 ------------------------------------------------- */
    if (GPIO_Pin == W_D0_Pin || GPIO_Pin == W_D1_Pin) {
        HAL_NVIC_DisableIRQ(W_D0_EXTI_IRQn);
        {
            uint8_t wmsg = (GPIO_Pin == W_D1_Pin) ? 1U : 0U;
            HAL_TIM_PWM_Start_IT(&htim11, TIM_CHANNEL_1);
            xQueueSendFromISR(myQueueWiegandHandle, &wmsg, &prior);
            portYIELD_FROM_ISR(prior);
        }
        return;
    }

    /* --- Matrix keyboard ROW scan --------------------------------------- */
    matrix_kbd_exti_from_isr(GPIO_Pin, &prior);
    portYIELD_FROM_ISR(prior);
}