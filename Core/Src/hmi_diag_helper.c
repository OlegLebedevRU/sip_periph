/*
 * hmi_diag_helper.c
 *
 * Diagnostic string formatting helpers for HMI display.
 *
 * Extracted from hmi.c on 2026-03-27.
 */

#include <stdio.h>
#include <string.h>
#include "hmi_diag_helper.h"
#include "app_i2c_slave.h"
#include "service_time_sync.h"

/* ---- Error sum ---------------------------------------------------------- */
uint32_t hmi_diag_error_sum(void)
{
    const app_i2c_slave_diag_t *d = app_i2c_slave_get_diag();
    return d->progress_timeout_count + d->stuck_scl_count
         + d->stuck_sda_count + d->abort_count
         + d->hard_recover_count + d->malformed_count
         + d->recover_fail_count;
}

/* ---- HH:MM:SS extraction ------------------------------------------------ */
void hmi_diag_format_time_hms(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0U) {
        return;
    }

    const char *ts = service_time_sync_get_datetime_str();
    if (ts != NULL && ts[0] != '\0') {
        size_t tslen = strlen(ts);
        const char *hms = (tslen > 9U) ? &ts[9] : ts;
        size_t hmslen = (tslen > 9U) ? (tslen - 9U) : tslen;
        if (hmslen >= buflen) {
            hmslen = buflen - 1U;
        }
        memcpy(buf, hms, hmslen);
        buf[hmslen] = '\0';
    } else {
        snprintf(buf, buflen, "--:--:--");
    }
}

/* ---- Extended diag line (hw counters + time) ----------------------------- */
void hmi_diag_format_line_ext(uint8_t index, char *buf, size_t buflen)
{
    const uint8_t hw_count = app_i2c_slave_diag_line_count();

    if (index < hw_count) {
        /* Hardware diag counter lines */
        app_i2c_slave_format_diag_line(index, buf, buflen);
    } else {
        /* Last line: current time (HH:MM:SS only) */
        hmi_diag_format_time_hms(buf, buflen);
    }
}
