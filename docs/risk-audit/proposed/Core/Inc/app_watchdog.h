#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WDG_TASK_UART_DWIN = 0,
    APP_WDG_TASK_PN532,
    APP_WDG_TASK_OLED,
    APP_WDG_TASK_I2C1,
    APP_WDG_TASK_I2C1_GUARD,
    APP_WDG_TASK_TCA6408,
    APP_WDG_TASK_I2C2_GUARD,
    APP_WDG_TASK_HMI,
    APP_WDG_TASK_HMI_MSG,
    APP_WDG_TASK_MAX
} AppWdgTaskId;

void AppWdg_Init(void);
void AppWdg_RegisterTask(AppWdgTaskId id);
void AppWdg_Kick(AppWdgTaskId id);
void AppWdg_Task(void const *argument);
void AppWdg_FastIsrProbe(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WATCHDOG_H */
