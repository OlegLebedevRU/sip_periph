/*
 * hmi_console.c
 *
 * Console HMI module — DWIN page 2 system diagnostics display.
 *
 * Extracted from hmi.c on 2026-03-27.
 * Contains all console-specific state, formatting, and DWIN I/O.
 */

#include <stdio.h>
#include <string.h>
#include "hmi_console.h"
#include "main.h"
#include "cmsis_os.h"
#include "app_i2c_slave.h"
#include "dwin_gfx.h"
#include "service_time_sync.h"
#include "app_uart_dwin_tx.h"

/* ---- Console geometry --------------------------------------------------- */
#define DWIN_CONSOLE_VP_ADDR   0x6000U
#define DWIN_CONSOLE_LINES     15U
#define DWIN_CONSOLE_TEXT_LEN  32U                  /* visible chars per row           */
#define DWIN_CONSOLE_MAX_TASKS 16U                  /* max tasks queried for watermark */
#define DWIN_CONSOLE_EOL_LEN    2U                  /* \r\n                            */
#define DWIN_CONSOLE_ROW_LEN   (DWIN_CONSOLE_TEXT_LEN + DWIN_CONSOLE_EOL_LEN)  /* 34  */
#define DWIN_CONSOLE_BUF_LEN   (DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 2U) /* +2 for 0xFFFF terminator */
/* Max payload per dwin_text_output call: DWIN_TX_BUF_SIZE(256) - 6 = 250 bytes.
 * We send 7 rows per chunk = 7*34 = 238 bytes < 250. */
#define DWIN_CONSOLE_CHUNK_ROWS   7U
#define DWIN_CONSOLE_CHUNK_BYTES  (DWIN_CONSOLE_CHUNK_ROWS * DWIN_CONSOLE_ROW_LEN)
#define DWIN_CONSOLE_PACE_MS      30U   /* inter-chunk delay so DWIN can absorb data */

/* ---- Console state ------------------------------------------------------ */
static volatile uint8_t s_console_remain = 0U;
static uint8_t s_console_line_idx = 0U;
static char s_console_buf[DWIN_CONSOLE_BUF_LEN];
/* Wall-clock deadline set at activation time.  Used as a fallback when
 * hmi_notify_1hz_tick() is stalled (e.g. TCA6408A / DS3231 unavailable). */
static uint32_t s_console_deadline_tick = 0U;

/* ---- External DWIN output (defined in hmi.c) ---------------------------- */
extern void dwin_text_output(const uint16_t inaddr, const uint8_t *text_to_hmi,
                             size_t elen);

/* ---- 1 Hz tick ---------------------------------------------------------- */
void hmi_notify_1hz_tick(void)
{
    if (s_console_remain > 0U) {
        s_console_remain--;
    }
}

/* ---- Public API --------------------------------------------------------- */
uint8_t hmi_console_is_active(void)
{
    return (s_console_remain > 0U) ? 1U : 0U;
}

void hmi_console_activate(uint8_t duration_sec)
{
    s_console_line_idx = 0U;
    s_console_remain = duration_sec;
    /* Set absolute wall-clock deadline as a fallback for hmi_console_poll().
     * If the DS3231 1Hz tick chain is broken after reset, s_console_remain
     * would never count down and the console would never exit on its own.
     * The deadline forces an exit after duration_sec real seconds even when
     * hmi_notify_1hz_tick() is not being called. */
    s_console_deadline_tick = HAL_GetTick() + ((uint32_t)duration_sec * 1000U);
}

/* ---- Buffer management -------------------------------------------------- */

/* Pre-fill RAM buffer with spaces+CRLF and 0xFF terminator (no DWIN write). */
static void hmi_console_reset_buffer(void)
{
    for (uint8_t i = 0U; i < DWIN_CONSOLE_LINES; i++) {
        size_t base = (size_t)i * DWIN_CONSOLE_ROW_LEN;
        memset(&s_console_buf[base], ' ', DWIN_CONSOLE_TEXT_LEN);
        s_console_buf[base + DWIN_CONSOLE_TEXT_LEN + 0U] = '\r';
        s_console_buf[base + DWIN_CONSOLE_TEXT_LEN + 1U] = '\n';
    }
    s_console_buf[DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 0U] = (char)0xFF;
    s_console_buf[DWIN_CONSOLE_LINES * DWIN_CONSOLE_ROW_LEN + 1U] = (char)0xFF;
}

/* ---- Chunk-based DWIN output -------------------------------------------- */

/* Send total_bytes from s_console_buf to DWIN in safe-sized chunks.
 * Chunks are sent in REVERSE order (highest VP first, base VP 0x6000 last)
 * because DWIN text widgets refresh on a write to their base VP address.
 * Writing the base address last ensures all data is in place before render. */
static void hmi_console_flush(uint16_t total_bytes)
{
    /* Calculate number of chunks */
    uint16_t n_chunks = (total_bytes + DWIN_CONSOLE_CHUNK_BYTES - 1U)
                      / DWIN_CONSOLE_CHUNK_BYTES;
    if (n_chunks == 0U) return;

    /* Send from last chunk to first (base VP last triggers DWIN render).
     * Pace with osDelay between chunks so DWIN can absorb each one. */
    for (uint16_t ci = n_chunks; ci > 0U; ci--) {
        uint16_t idx = ci - 1U;
        uint16_t offset = idx * DWIN_CONSOLE_CHUNK_BYTES;
        uint16_t remain = total_bytes - offset;
        uint16_t chunk = (remain > DWIN_CONSOLE_CHUNK_BYTES)
                       ? DWIN_CONSOLE_CHUNK_BYTES : remain;
        uint16_t word_off = offset / 2U;
        uint16_t addr = (uint16_t)(DWIN_CONSOLE_VP_ADDR + word_off);
        dwin_text_output(addr, (uint8_t*)&s_console_buf[offset], chunk);

        /* Pace: give DWIN time to process before sending next chunk */
        if (ci > 1U) {
            osDelay(DWIN_CONSOLE_PACE_MS);
        }
    }
}

/* Send the entire buffer (all 15 rows + 0xFF terminator) to DWIN. */
static void hmi_console_flush_all(void)
{
    hmi_console_flush((uint16_t)sizeof(s_console_buf));
}

/* ---- Row formatting ----------------------------------------------------- */

/* Format one row into s_console_buf at position row_idx (no DWIN send).
 *
 * Row map (15 lines):
 *  0  HEADER            7  I2C RELISTEN
 *  1  UPTIME / LEFT     8  I2C STUCK SCL
 *  2  HEAP FREE         9  I2C RECOVERED
 *  3  HEAP MIN FREE    10  NO PROGRESS s
 *  4  LIBC HEAP USED   11  TOP-1 task (min watermark)
 *  5  MSP USED/SIZE    12  TOP-2 task
 *  6  .BSS+.DATA       13  TOP-3 task
 *                      14  RTC
 */
static void hmi_console_format_row(uint8_t row_idx)
{
    const app_i2c_slave_diag_t *d = app_i2c_slave_get_diag();
    const char *rtc = service_time_sync_get_datetime_str();
    uint32_t uptime = HAL_GetTick() / 1000U;
    uint32_t h = uptime / 3600U;
    uint32_t m = (uptime % 3600U) / 60U;
    uint32_t s = uptime % 60U;
    uint32_t since_progress = 0U;
    int written = 0;
    char tmp[DWIN_CONSOLE_TEXT_LEN + 1U];

    if (h > 999U) h = 999U;
    if (d->last_progress_tick != 0U) {
        uint32_t now = HAL_GetTick();
        if (now >= d->last_progress_tick)
            since_progress = (now - d->last_progress_tick) / 1000U;
    }
    if (rtc == NULL || rtc[0] == '\0')
        rtc = "--.--.-- --:--:--";

    /* ---- Rows 11-13: top-3 heaviest tasks (lowest stack watermark) ---- */
    if (row_idx >= 11U && row_idx <= 13U) {
        TaskStatus_t task_arr[DWIN_CONSOLE_MAX_TASKS];
        UBaseType_t n_tasks = uxTaskGetSystemState(task_arr, DWIN_CONSOLE_MAX_TASKS, NULL);

        uint8_t rank = row_idx - 11U;  /* 0, 1, or 2 */
        const char *name = "---";
        uint32_t wm = 0U;
        for (uint8_t r = 0U; r <= rank; r++) {
            uint16_t min_wm = 0xFFFFU;
            UBaseType_t min_idx = 0U;
            uint8_t found = 0U;
            for (UBaseType_t i = 0U; i < n_tasks; i++) {
                if (task_arr[i].usStackHighWaterMark < min_wm) {
                    min_wm = task_arr[i].usStackHighWaterMark;
                    min_idx = i;
                    found = 1U;
                }
            }
            if (found) {
                if (r == rank) {
                    name = task_arr[min_idx].pcTaskName;
                    wm = (uint32_t)task_arr[min_idx].usStackHighWaterMark;
                }
                task_arr[min_idx].usStackHighWaterMark = 0xFFFFU;
            }
        }
        /* watermark is in StackType_t words (4 bytes on CM4) */
        written = snprintf(tmp, sizeof(tmp), "%-16s stk%5lu", name, wm * 4UL);
    } else {
        switch (row_idx) {
        case 0:
            written = snprintf(tmp, sizeof(tmp), "=== SYSTEM DIAG PAGE 2 ===");
            break;
        case 1:
            written = snprintf(tmp, sizeof(tmp), "UP %03lu:%02lu:%02lu   LEFT %02lu",
                               h, m, s, (uint32_t)s_console_remain);
            break;
        case 2:
            written = snprintf(tmp, sizeof(tmp), "HEAP FREE      %10u",
                               (unsigned)xPortGetFreeHeapSize());
            break;
        case 3:
            written = snprintf(tmp, sizeof(tmp), "HEAP MIN FREE  %10u",
                               (unsigned)xPortGetMinimumEverFreeHeapSize());
            break;
        case 4: {
            /* libc heap used: current sbrk() watermark minus _end */
            extern uint8_t _end;           /* linker symbol: end of .bss */
            extern void *_sbrk(ptrdiff_t); /* sysmem.c */
            uint8_t *brk = (uint8_t *)_sbrk(0);
            uint32_t libc_used = (brk > &_end) ? (uint32_t)(brk - &_end) : 0U;
            written = snprintf(tmp, sizeof(tmp), "LIBC HEAP USED %10lu", libc_used);
            break;
        }
        case 5: {
            /* MSP usage: _estack (top) minus current MSP value */
            extern uint8_t _estack;        /* linker: top of RAM = MSP initial */
            extern uint32_t _Min_Stack_Size; /* linker: reserved MSP area */
            uint32_t msp_top  = (uint32_t)&_estack;
            uint32_t msp_size = (uint32_t)&_Min_Stack_Size;  /* value IS the size */
            uint32_t msp_cur  = __get_MSP();
            uint32_t msp_used = (msp_top > msp_cur) ? (msp_top - msp_cur) : 0U;
            written = snprintf(tmp, sizeof(tmp), "MSP %5lu / %5lu B",
                               msp_used, msp_size);
            break;
        }
        case 6: {
            /* .bss + .data size (global/static variables footprint) */
            extern uint8_t _sdata, _edata; /* linker: .data boundaries */
            extern uint8_t _sbss, _ebss;   /* linker: .bss boundaries */
            uint32_t data_sz = (uint32_t)(&_edata - &_sdata);
            uint32_t bss_sz  = (uint32_t)(&_ebss  - &_sbss);
            written = snprintf(tmp, sizeof(tmp), ".BSS+.DATA     %10lu",
                               data_sz + bss_sz);
            break;
        }
        case 7:
            written = snprintf(tmp, sizeof(tmp), "I2C RELISTEN   %10lu",
                               d->relisten_count);
            break;
        case 8:
            written = snprintf(tmp, sizeof(tmp), "I2C STUCK SCL  %10lu",
                               d->stuck_scl_count);
            break;
        case 9:
            written = snprintf(tmp, sizeof(tmp), "I2C RECOVERED  %10lu",
                               d->hard_recover_count);
            break;
        case 10:
            written = snprintf(tmp, sizeof(tmp), "NO PROGRESS s  %10lu",
                               since_progress);
            break;
        /* rows 11-13 handled above (top-3 tasks) */
        case 14:
            written = snprintf(tmp, sizeof(tmp), "RTC %-28s", rtc);
            break;
        default:
            return;
        }
    }

    if (written < 0) written = 0;
    if ((size_t)written > DWIN_CONSOLE_TEXT_LEN) written = (int)DWIN_CONSOLE_TEXT_LEN;

    char *row = &s_console_buf[(size_t)row_idx * DWIN_CONSOLE_ROW_LEN];
    memcpy(row, tmp, (size_t)written);
    if ((size_t)written < DWIN_CONSOLE_TEXT_LEN) {
        memset(&row[written], ' ', DWIN_CONSOLE_TEXT_LEN - (size_t)written);
    }
    row[DWIN_CONSOLE_TEXT_LEN + 0U] = '\r';
    row[DWIN_CONSOLE_TEXT_LEN + 1U] = '\n';
}

/* Fill all 15 rows into the buffer and send to DWIN once. */
static void hmi_console_fill_all(void)
{
    for (uint8_t r = 0U; r < DWIN_CONSOLE_LINES; r++) {
        hmi_console_format_row(r);
    }
    /* 0xFF terminator is already at the end from reset_buffer */
    hmi_console_flush_all();
}

/* Update one row (round-robin) and send to DWIN. */
static void hmi_console_update(void)
{
    hmi_console_format_row(s_console_line_idx);
    hmi_console_flush_all();

    s_console_line_idx++;
    if (s_console_line_idx >= DWIN_CONSOLE_LINES) {
        s_console_line_idx = 0U;
    }
}

/* ---- Poll (called from StartTaskHmiMsg loop) ---------------------------- */
void hmi_console_poll(uint8_t *was_console, uint8_t *last_remain,
                      uint32_t *time_tick)
{
    /* Fallback: if the DS3231 1Hz chain is stalled (TCA/RTC unavailable),
     * hmi_notify_1hz_tick() won't be called and s_console_remain never
     * decrements.  Force expiry via wall-clock deadline so the console
     * always exits after the requested duration regardless of I2C health. */
    if ((s_console_remain > 0U) && (s_console_deadline_tick != 0U)
            && ((int32_t)(HAL_GetTick() - s_console_deadline_tick) >= 0)) {
        s_console_remain = 0U;
    }

    if (s_console_remain > 0U) {
        if (*was_console == 0U) {
            /* Console just activated — fill all rows once, flush once, switch page */
            *was_console = 1U;
            *last_remain = s_console_remain;
            s_console_line_idx = 0U;
            hmi_console_reset_buffer();
            hmi_console_fill_all();
            dwin_gfx_page_switch(2U);
        } else if (s_console_remain != *last_remain) {
            /* Console tick — update one line */
            *last_remain = s_console_remain;
            hmi_console_update();
        }
    } else if (*was_console != 0U) {
        *was_console = 0U;
        *last_remain = 0U;
        /* Write 0xFFFF terminator to console base VP so DWIN clears
         * the text widget before we leave the page. */
        {
            const uint8_t term[2] = { 0xFF, 0xFF };
            dwin_text_output(DWIN_CONSOLE_VP_ADDR, term, sizeof(term));
        }
        osDelay(DWIN_CONSOLE_PACE_MS);  /* let UART+DWIN finish */
        dwin_gfx_page_switch(0);
        /* Force immediate clock redraw after returning from console */
        *time_tick = 0U;
    }
}
