/*
 * dwin_gfx.h
 *
 * DWIN DGUS graphic primitives over UART.
 * Uses command 0x84 (graphic / SRAM write) to draw shapes directly
 * on the display without pre-built DGUS project icons.
 *
 * Protocol reference (DGUS II / T5L / DMG series):
 *   Header : 5A A5 LL 84 00 <sub_cmd> <params...>
 *   sub_cmd: 01=rect outline, 02=circle outline, 03=line,
 *            04=filled rect,  05=filled circle
 *   Coords : big-endian uint16  |  Color : RGB565 big-endian
 *
 * All functions are non-blocking (HAL_UART_Transmit_IT via shared txbuf).
 * Call from a single FreeRTOS task to avoid concurrent txbuf access.
 */

#ifndef INC_DWIN_GFX_H_
#define INC_DWIN_GFX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -----------------------------------------------------------------------
 *  RGB565 colour helpers
 * ----------------------------------------------------------------------- */
#define DWIN_COLOR_BLACK    0x0000U
#define DWIN_COLOR_WHITE    0xFFFFU
#define DWIN_COLOR_RED      0xF800U
#define DWIN_COLOR_GREEN    0x07E0U
#define DWIN_COLOR_BLUE     0x001FU
#define DWIN_COLOR_YELLOW   0xFFE0U
#define DWIN_COLOR_CYAN     0x07FFU
#define DWIN_COLOR_MAGENTA  0xF81FU
#define DWIN_COLOR_ORANGE   0xFD20U
#define DWIN_COLOR_GREY     0x7BEFU

/* Compose RGB565 from 8-bit components (r 0-31, g 0-63, b 0-31) */
#define DWIN_RGB565(r5, g6, b5) \
    ((uint16_t)(((r5) << 11) | ((g6) << 5) | (b5)))

/* -----------------------------------------------------------------------
 *  Primitive drawing — DWIN 0x84 graphic commands
 * ----------------------------------------------------------------------- */

/** Draw rectangle outline.
 *  @param x1,y1 top-left corner
 *  @param x2,y2 bottom-right corner
 *  @param color  RGB565 */
void dwin_gfx_rect(uint16_t x1, uint16_t y1,
                    uint16_t x2, uint16_t y2, uint16_t color);

/** Draw filled rectangle.
 *  @param x1,y1 top-left corner
 *  @param x2,y2 bottom-right corner
 *  @param color  RGB565 */
void dwin_gfx_rect_fill(uint16_t x1, uint16_t y1,
                         uint16_t x2, uint16_t y2, uint16_t color);

/** Draw circle outline.
 *  @param xc,yc  centre
 *  @param r      radius (pixels)
 *  @param color  RGB565 */
void dwin_gfx_circle(uint16_t xc, uint16_t yc,
                      uint16_t r, uint16_t color);

/** Draw filled circle.
 *  @param xc,yc  centre
 *  @param r      radius (pixels)
 *  @param color  RGB565 */
void dwin_gfx_circle_fill(uint16_t xc, uint16_t yc,
                           uint16_t r, uint16_t color);

/** Draw a line from (x1,y1) to (x2,y2).
 *  @param color  RGB565 */
void dwin_gfx_line(uint16_t x1, uint16_t y1,
                    uint16_t x2, uint16_t y2, uint16_t color);

/** Clear a rectangular region (fills with background colour).
 *  Equivalent to dwin_gfx_rect_fill with bg colour. */
void dwin_gfx_clear_region(uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, uint16_t bg_color);

/* -----------------------------------------------------------------------
 *  Diagnostic / indicator helpers
 * ----------------------------------------------------------------------- */

/** Draw a small status dot (filled circle r=6).
 *  Useful for showing module health on a dashboard.
 *  @param xc,yc  centre position
 *  @param color  DWIN_COLOR_GREEN / RED / YELLOW */
void dwin_gfx_status_dot(uint16_t xc, uint16_t yc, uint16_t color);

/** Draw a horizontal bar graph (0-100 %).
 *  Draws a filled rect proportional to `pct` inside a border rect.
 *  @param x,y     top-left of bar area
 *  @param w,h     total width/height of bar area
 *  @param pct     fill percentage 0..100
 *  @param fg      bar colour
 *  @param bg      background / empty colour */
void dwin_gfx_bar(uint16_t x, uint16_t y,
                   uint16_t w, uint16_t h,
                   uint8_t pct,
                   uint16_t fg, uint16_t bg);

/* -----------------------------------------------------------------------
 *  T5L register-write helpers (always work, no DGUS project changes)
 * ----------------------------------------------------------------------- */

/** Switch display page (0-based, matches NN.png in DWIN_SET). */
void dwin_gfx_page_switch(uint8_t page);

/** Set backlight level (0 = off, 0x40 = max). */
void dwin_gfx_backlight(uint8_t level);

/** Smoke-test: switches to page 1 to verify UART→display link.
 *  Remove after testing. */
void dwin_gfx_test(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DWIN_GFX_H_ */
