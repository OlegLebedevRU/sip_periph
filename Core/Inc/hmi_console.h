/*
 * hmi_console.h
 *
 * Console HMI module — DWIN page 2 system diagnostics display.
 *
 * Activated by magic code 102* on the touch keypad.
 * Shows 15 rows of system diagnostics (uptime, heap, I2C counters,
 * task stack watermarks, RTC) with a countdown timer.
 *
 * Created: 2026-03-27  (extracted from hmi.c)
 */

#ifndef INC_HMI_CONSOLE_H_
#define INC_HMI_CONSOLE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * Activate the console page for the given number of seconds.
 * Resets the line index, fills the console buffer, switches DWIN to page 2.
 * Call from StartTaskHmi when magic code 102* is detected.
 */
void hmi_console_activate(uint8_t duration_sec);

/**
 * Poll console state from the HMI message task loop.
 * Handles:
 *   - initial page switch when console first becomes active
 *   - per-tick round-robin row updates
 *   - return to page 0 when countdown expires
 *
 * @param[in,out] was_console  sticky flag: 1 while console is active
 * @param[in,out] last_remain  previous s_console_remain snapshot
 * @param[out]    time_tick    reset to 0 on console→page0 transition
 *                             so caller redraws the clock immediately
 */
void hmi_console_poll(uint8_t *was_console, uint8_t *last_remain,
                      uint32_t *time_tick);

/**
 * 1 Hz tick handler — decrements the console countdown.
 * Must be called from a 1 Hz source (e.g. DS3231 SQW via TCA6408).
 */
void hmi_notify_1hz_tick(void);

/**
 * @return non-zero while the console page is active (countdown > 0).
 */
uint8_t hmi_console_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_HMI_CONSOLE_H_ */
