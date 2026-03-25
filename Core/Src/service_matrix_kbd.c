/*
 * service_matrix_kbd.c
 *
 * Matrix keyboard service (4x3).
 * Перенесено из HAL_GPIO_EXTI_Callback/main.c.
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "service_runtime_config.h"
#include "service_matrix_kbd.h"

/* ---- extern RTOS handles/timers from main.c ---------------------------- */
extern osMessageQId myQueueToMasterHandle;
extern osMessageQId myQueueOLEDHandle;
extern osTimerId    myTimerKeyHandle;

/* ---- internal state ----------------------------------------------------- */
static struct keyb s_keyb = { .buf = {0}, .offset = 0 };
static __IO uint8_t s_debounce = 1U;
static uint8_t s_final_input = 0U;
static GPIO_InitTypeDef s_gpio_init = {0};
static GPIO_InitTypeDef s_gpio_init_restore = {0};

void service_matrix_kbd_init(void)
{
    memset(&s_keyb, 0, sizeof(s_keyb));
    s_debounce = 1U;
    s_final_input = 0U;
}

const struct keyb *service_matrix_kbd_get_state(void)
{
    return &s_keyb;
}

void matrix_kbd_exti_from_isr(uint16_t GPIO_Pin, BaseType_t *pxHigherPriorityTaskWoken)
{
    uint8_t keyPressed = 0U;
    uint16_t sig1 = 0x01U;
    const runtime_config_t *cfg = runtime_config_get();
    uint32_t freeze_sec = (cfg != NULL) ? cfg->matrix_freeze_sec : MATRIX_KEYB_FREEZE_SEC_DEFAULT;

    if (pxHigherPriorityTaskWoken == NULL) {
        return;
    }
    if (!s_debounce) {
        return;
    }

    /* Configure ROW pins to input for scan */
    s_gpio_init.Pin   = ROW1_Pin | ROW2_Pin;
    s_gpio_init.Mode  = GPIO_MODE_INPUT;
    s_gpio_init.Pull  = GPIO_PULLDOWN;
    s_gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &s_gpio_init);
    s_gpio_init.Pin = ROW3_Pin | ROW4_Pin;
    HAL_GPIO_Init(GPIOB, &s_gpio_init);

    /* COL1 */
    HAL_GPIO_WritePin(GPIOB, COL1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL2_Pin | COL3_Pin, GPIO_PIN_RESET);
    if      (GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin)) keyPressed = 0x31;
    else if (GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin)) keyPressed = 0x34;
    else if (GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin)) keyPressed = 0x37;
    else if (GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin)) keyPressed = 0x2A;

    /* COL2 */
    HAL_GPIO_WritePin(GPIOB, COL2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL1_Pin | COL3_Pin, GPIO_PIN_RESET);
    if      (GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin)) keyPressed = 0x32;
    else if (GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin)) keyPressed = 0x35;
    else if (GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin)) keyPressed = 0x38;
    else if (GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin)) keyPressed = 0x30;

    /* COL3 */
    HAL_GPIO_WritePin(GPIOB, COL3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL1_Pin | COL2_Pin, GPIO_PIN_RESET);
    if      (GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin)) keyPressed = 0x33;
    else if (GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin)) keyPressed = 0x36;
    else if (GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin)) keyPressed = 0x39;
    else if (GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin)) keyPressed = 0x23;

    if (keyPressed != 0U && keyPressed != 0x23U) {
        s_keyb.buf[s_keyb.offset] = keyPressed;
        s_keyb.offset++;
        s_keyb.buf[s_keyb.offset] = 0;
        xQueueSendFromISR(myQueueOLEDHandle, &sig1, pxHigherPriorityTaskWoken);
        osTimerStart(myTimerKeyHandle, 300U);
    } else if (keyPressed == 0x23U) {
        I2cPacketToMaster_t pckt;
        s_keyb.buf[s_keyb.offset] = keyPressed;
        s_keyb.offset++;
        s_keyb.buf[s_keyb.offset] = 0;
        pckt.payload = &s_keyb.buf[0];
        pckt.len     = s_keyb.offset;
        pckt.type    = PACKET_PIN;
        pckt.ttl     = uid_ttl;
        xQueueSendFromISR(myQueueToMasterHandle, &pckt, pxHigherPriorityTaskWoken);
        xQueueSendFromISR(myQueueOLEDHandle, &sig1, pxHigherPriorityTaskWoken);
        osTimerStart(myTimerKeyHandle, freeze_sec * 1000U);
        s_final_input = 1U;
    } else {
        osTimerStart(myTimerKeyHandle, 1U);
    }

    s_debounce = 0U;
}

void cb_keyTimer(void const *argument)
{
    BaseType_t prior = pdFALSE;
    uint16_t sig1 = 0x01U;
    (void)argument;

    if (s_final_input == 1U) {
        memset((void*)s_keyb.buf, 0, sizeof(s_keyb.buf));
        s_keyb.offset = 0;
        xQueueSendFromISR(myQueueOLEDHandle, &sig1, &prior);
        portYIELD_FROM_ISR(prior);
        s_final_input = 0U;
    }

    s_debounce = 1U;
    HAL_GPIO_WritePin(GPIOB, COL1_Pin | COL2_Pin | COL3_Pin, GPIO_PIN_SET);

    /* Restore ROW pins back to EXTI */
    s_gpio_init_restore.Pin = ROW1_Pin | ROW2_Pin;
    s_gpio_init_restore.Mode = GPIO_MODE_IT_RISING;
    s_gpio_init_restore.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &s_gpio_init_restore);
    s_gpio_init_restore.Pin = ROW3_Pin;
    HAL_GPIO_Init(GPIOB, &s_gpio_init_restore);
}
