#include "app_watchdog.h"

#include "cmsis_os.h"
#include "main.h"

#define APP_IWDG_KR_ENABLE_WRITE   0x5555U
#define APP_IWDG_KR_REFRESH        0xAAAAU
#define APP_IWDG_KR_START          0xCCCCU
#define APP_IWDG_PRESCALER_DIV32   3U
#define APP_IWDG_RELOAD_2S         1999U
#define APP_IWDG_SR_PVU            (1UL << 0)
#define APP_IWDG_SR_RVU            (1UL << 1)
#define APP_IWDG_READY_TIMEOUT     1000000U

#define APP_WATCHDOG_REFRESH_MS        250U  /* Supervisor checks 4x per heartbeat window. */
#define APP_WATCHDOG_HEARTBEAT_MS      1000U /* Required tasks must report within this window. */
#define APP_WATCHDOG_STARTUP_GRACE_MS  3000U /* Gracefully covers first task scheduling after IWDG start. */
#define APP_WATCHDOG_RETAINED_MAGIC    0x57444731UL /* WDG1 */

typedef struct {
    uint32_t magic;
    uint32_t reset_flags;
    uint32_t last_reason;
    uint32_t fault_count;
    uint32_t supervisor_timeout_count;
} app_watchdog_retained_t;

static volatile uint32_t s_required_mask = 0U;
static volatile uint32_t s_heartbeat_tick[APP_WATCHDOG_TASK_MAX];
static volatile uint8_t s_started = 0U;
/* STM32F411CEUX_FLASH.ld maps .noinit as NOLOAD in SRAM, so this survives
 * NVIC_SystemReset() and IWDG reset while still being lost on power removal. */
static volatile app_watchdog_retained_t s_retained __attribute__((section(".noinit")));

static uint8_t iwdg_wait_ready(void)
{
    uint32_t guard = APP_IWDG_READY_TIMEOUT;

    while (((IWDG->SR & (APP_IWDG_SR_PVU | APP_IWDG_SR_RVU)) != 0U) && (guard > 0U)) {
        guard--;
    }

    return (guard > 0U) ? 1U : 0U;
}

static void iwdg_refresh(void)
{
    IWDG->KR = APP_IWDG_KR_REFRESH;
}

static void retained_init_once(void)
{
    if (s_retained.magic != APP_WATCHDOG_RETAINED_MAGIC) {
        s_retained.magic = APP_WATCHDOG_RETAINED_MAGIC;
        s_retained.last_reason = APP_WATCHDOG_REASON_NONE;
        s_retained.fault_count = 0U;
        s_retained.supervisor_timeout_count = 0U;
    }

    s_retained.reset_flags = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
}

void app_watchdog_init(void)
{
    retained_init_once();

    uint32_t guard = APP_IWDG_READY_TIMEOUT;
    RCC->CSR |= RCC_CSR_LSION;
    while (((RCC->CSR & RCC_CSR_LSIRDY) == 0U) && (guard > 0U)) {
        guard--;
    }
    if ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
        app_watchdog_record_fault(APP_WATCHDOG_REASON_ERROR_HANDLER);
        NVIC_SystemReset();
    }

    IWDG->KR = APP_IWDG_KR_ENABLE_WRITE;
    if (iwdg_wait_ready() == 0U) {
        app_watchdog_record_fault(APP_WATCHDOG_REASON_ERROR_HANDLER);
        NVIC_SystemReset();
    }

    IWDG->PR = APP_IWDG_PRESCALER_DIV32;
    IWDG->RLR = APP_IWDG_RELOAD_2S;
    if (iwdg_wait_ready() == 0U) {
        app_watchdog_record_fault(APP_WATCHDOG_REASON_ERROR_HANDLER);
        NVIC_SystemReset();
    }

    iwdg_refresh();
    IWDG->KR = APP_IWDG_KR_START;
    s_started = 1U;
}

void app_watchdog_require_task(app_watchdog_task_id_t id)
{
    if ((uint32_t)id < (uint32_t)APP_WATCHDOG_TASK_MAX) {
        s_heartbeat_tick[id] = HAL_GetTick();
        s_required_mask |= (1UL << (uint32_t)id);
    }
}

void app_watchdog_kick(app_watchdog_task_id_t id)
{
    if ((uint32_t)id < (uint32_t)APP_WATCHDOG_TASK_MAX) {
        s_heartbeat_tick[id] = HAL_GetTick();
    }
}

void app_watchdog_record_fault(app_watchdog_reason_t reason)
{
    if (s_retained.magic != APP_WATCHDOG_RETAINED_MAGIC) {
        s_retained.magic = APP_WATCHDOG_RETAINED_MAGIC;
        s_retained.fault_count = 0U;
        s_retained.supervisor_timeout_count = 0U;
    }

    s_retained.last_reason = (uint32_t)reason;
    if ((reason == APP_WATCHDOG_REASON_HARDFAULT)
     || (reason == APP_WATCHDOG_REASON_MEMMANAGE)
     || (reason == APP_WATCHDOG_REASON_BUSFAULT)
     || (reason == APP_WATCHDOG_REASON_USAGEFAULT)) {
        s_retained.fault_count++;
    } else if (reason == APP_WATCHDOG_REASON_SUPERVISOR_TIMEOUT) {
        s_retained.supervisor_timeout_count++;
    }
}

static uint8_t all_required_tasks_fresh(uint32_t now)
{
    uint32_t required = s_required_mask;

    for (uint32_t id = 0U; id < (uint32_t)APP_WATCHDOG_TASK_MAX; id++) {
        if ((required & (1UL << id)) != 0U) {
            uint32_t last = s_heartbeat_tick[id];
            if ((last == 0U) || ((now - last) > APP_WATCHDOG_HEARTBEAT_MS)) {
                return 0U;
            }
        }
    }

    return 1U;
}

void StartTaskWatchdog(void const *argument)
{
    uint32_t start_tick;
    uint32_t last_tick;

    (void)argument;

    start_tick = HAL_GetTick();
    last_tick = start_tick;

    for (;;) {
        uint32_t now;

        osDelay(APP_WATCHDOG_REFRESH_MS);
        now = HAL_GetTick();

        if ((s_started == 0U) || (now == last_tick)) {
            last_tick = now;
            continue;
        }

        if ((now - start_tick) < APP_WATCHDOG_STARTUP_GRACE_MS) {
            iwdg_refresh();
        } else if (all_required_tasks_fresh(now) != 0U) {
            iwdg_refresh();
        } else {
            app_watchdog_record_fault(APP_WATCHDOG_REASON_SUPERVISOR_TIMEOUT);
        }

        last_tick = now;
    }
}
