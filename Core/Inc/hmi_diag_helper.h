/*
 * hmi_diag_helper.h
 *
 * Diagnostic string formatting helpers for HMI display.
 *
 * Extracts common patterns from hmi.c:
 *   - I2C error sum calculation for change detection
 *   - HH:MM:SS extraction from "DD.MM.YY-HH:MM:SS" time string
 *   - Extended diag line formatting (hw counters + time as last line)
 *
 * Created: 2026-03-27  (extracted from hmi.c)
 */

#ifndef INC_HMI_DIAG_HELPER_H_
#define INC_HMI_DIAG_HELPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
 * Compute sum of all I2C slave diagnostic error counters.
 * Used by hmi.c for change detection (start/stop diag rotation).
 */
uint32_t hmi_diag_error_sum(void);

/**
 * Extract "HH:MM:SS" from the service_time_sync datetime string
 * and copy into buf.  If time is unavailable, writes "--:--:--".
 *
 * @param buf     destination buffer
 * @param buflen  size of buf (must be >= 9)
 */
void hmi_diag_format_time_hms(char *buf, size_t buflen);

/**
 * Format one diagnostic line into buf.
 *
 * Lines 0..(hw_count-1) delegate to app_i2c_slave_format_diag_line().
 * Line hw_count is the "HH:MM:SS" time string.
 *
 * @param index   line index (0-based)
 * @param buf     destination buffer
 * @param buflen  size of buf
 */
void hmi_diag_format_line_ext(uint8_t index, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* INC_HMI_DIAG_HELPER_H_ */
