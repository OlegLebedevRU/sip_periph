#include "app_watchdog.h"
#include "cmsis_os.h"

#define APP_WDG_FEED_PERIOD_MS   200U
#define APP_WDG_HEARTBEAT_WIN_MS 800U

static IWDG_HandleTypeDef s_hiwdg;
static volatile uint32_t s_task_alive_mask = 0U;
static uint32_t s_task_expected_mask = 0U;
static volatile uint32_t s_supervisor_beat = 0U;
static volatile uint8_t s_fast_probe_ok = 1U;

void AppWdg_Init(void)
{
    s_hiwdg.Instance = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    s_hiwdg.Init.Reload = 1499U; /* nominal ~1.5 s; about 1.25-1.875 s if LSI is close to the ±20% spread documented in docs/risk-audit/hardfault-risk-audit.md */

    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK) {
        NVIC_SystemReset();
    }
}

void AppWdg_RegisterTask(AppWdgTaskId id)
{
    if ((uint32_t)id < (uint32_t)APP_WDG_TASK_MAX) {
        s_task_expected_mask |= (1UL << (uint32_t)id);
    }
}

void AppWdg_Kick(AppWdgTaskId id)
{
    if ((uint32_t)id < (uint32_t)APP_WDG_TASK_MAX) {
        __atomic_or_fetch(&s_task_alive_mask, (1UL << (uint32_t)id), __ATOMIC_RELEASE);
    }
}

void AppWdg_FastIsrProbe(void)
{
    static uint32_t s_last_supervisor_beat = 0U;
    uint32_t current = s_supervisor_beat;

    s_fast_probe_ok = (current != s_last_supervisor_beat) ? 1U : 0U;
    s_last_supervisor_beat = current;
}

void AppWdg_Task(void const *argument)
{
    (void)argument;

    for (;;) {
        uint32_t alive_mask;
        uint8_t all_alive;

        osDelay(APP_WDG_FEED_PERIOD_MS);
        s_supervisor_beat++;

        alive_mask = __atomic_load_n(&s_task_alive_mask, __ATOMIC_ACQUIRE);
        all_alive = ((alive_mask & s_task_expected_mask) == s_task_expected_mask) ? 1U : 0U;

        if ((s_fast_probe_ok != 0U) && (all_alive != 0U)) {
            __atomic_store_n(&s_task_alive_mask, 0U, __ATOMIC_RELAXED);
            (void)HAL_IWDG_Refresh(&s_hiwdg);
        }
        /* otherwise intentionally do not refresh -> hardware reset */
    }
}
