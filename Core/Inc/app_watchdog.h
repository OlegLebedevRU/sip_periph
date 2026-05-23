#ifndef INC_APP_WATCHDOG_H_
#define INC_APP_WATCHDOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WATCHDOG_TASK_I2C1_GUARD = 0,
    APP_WATCHDOG_TASK_I2C1_RXTX,
    APP_WATCHDOG_TASK_MAX
} app_watchdog_task_id_t;

typedef enum {
    APP_WATCHDOG_REASON_NONE = 0,
    APP_WATCHDOG_REASON_HARDFAULT = 1,
    APP_WATCHDOG_REASON_MEMMANAGE = 2,
    APP_WATCHDOG_REASON_BUSFAULT = 3,
    APP_WATCHDOG_REASON_USAGEFAULT = 4,
    APP_WATCHDOG_REASON_ERROR_HANDLER = 5,
    APP_WATCHDOG_REASON_SUPERVISOR_TIMEOUT = 6
} app_watchdog_reason_t;

void app_watchdog_init(void);
void app_watchdog_require_task(app_watchdog_task_id_t id);
void app_watchdog_kick(app_watchdog_task_id_t id);
void app_watchdog_record_fault(app_watchdog_reason_t reason);
void StartTaskWatchdog(void const *argument);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_WATCHDOG_H_ */
