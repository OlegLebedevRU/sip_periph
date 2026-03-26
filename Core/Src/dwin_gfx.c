/*
 * dwin_gfx.c
 *
 * DWIN T5L DGUS graphic helpers for DMG80480C043.
 *
 * UART safety: gfx_send() delegates to the shared DWIN TX transport
 * (app_uart_dwin_tx.c) which serialises all USART2 TX traffic with a
 * common mutex and buffer.
 *
 * === Drawing primitives ===
 * T5L DGUS does NOT support arbitrary framebuffer drawing via UART.
 * Lines, rectangles, circles require a "Basic Graphic" widget in the
 * DGUS project (14ShowFile.bin) bound to a VP address.  Without it,
 * all primitive draw calls are compiled as NO-OPs.
 *
 * Set GFX_VP_ENABLED to 1 *after* adding the widget in DGUS Tools.
 *
 * === What DOES work without DGUS changes ===
 *   - dwin_gfx_page_switch(page)  — switch display page (register 0x03)
 *   - dwin_gfx_backlight(level)   — set backlight 0..64  (register 0x82)
 * These use register-write (0x80) which is always supported.
 */

#include "dwin_gfx.h"
#include "app_uart_dwin_tx.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"
#include <string.h>

/* ---- Private TX buffer (separate from txbuf[] in hmi.c) --------------- */
#define GFX_BUF_SIZE  32U
static uint8_t s_gfx_buf[GFX_BUF_SIZE];

/* ---- Safe IT-based send via shared DWIN TX transport ------------------ */
static void gfx_send(uint8_t len)
{
    dwin_tx_send(s_gfx_buf, len);
}

/* ---- Helpers ---------------------------------------------------------- */
static uint8_t put8(uint8_t i, uint8_t v)  { s_gfx_buf[i++] = v; return i; }
static uint8_t put16(uint8_t i, uint16_t v)
{
    s_gfx_buf[i++] = (uint8_t)(v >> 8);
    s_gfx_buf[i++] = (uint8_t)(v & 0xFFU);
    return i;
}

/* ========================================================================
 *  Register-write helpers (always work on T5L)
 * ======================================================================== */

/* Page switch: register 0x03, data = 2-byte page ID (big-endian)
 *   5A A5 04 80 03 00 PP                                         */
void dwin_gfx_page_switch(uint8_t page)
{
    uint8_t i = 0;
    i = put8(i, 0x5A);
    i = put8(i, 0xA5);
    i = put8(i, 0x04);          /* body length = 4    */
    i = put8(i, 0x80);          /* cmd: reg write     */
    i = put8(i, 0x03);          /* register: PIC_ID   */
    i = put16(i, (uint16_t)page);
    gfx_send(i);                /* 7 bytes            */
}

/* Backlight: register 0x82, data = 1 byte (0..0x40 = 0..100%)
 *   5A A5 03 80 82 LL                                             */
void dwin_gfx_backlight(uint8_t level)
{
    if (level > 0x40) level = 0x40;
    uint8_t i = 0;
    i = put8(i, 0x5A);
    i = put8(i, 0xA5);
    i = put8(i, 0x03);          /* body length = 3    */
    i = put8(i, 0x80);          /* cmd: reg write     */
    i = put8(i, 0x82);          /* register: LED_NOW  */
    i = put8(i, level);
    gfx_send(i);                /* 6 bytes            */
}

/* ========================================================================
 *  VP-write drawing (requires "Basic Graphic" widget in DGUS project)
 * ======================================================================== */
#ifndef DWIN_GFX_VP
#define DWIN_GFX_VP      0x5000U
#endif

#define GFX_VP_ENABLED   0       /* <<< set to 1 after adding widget */

#if GFX_VP_ENABLED
static void gfx_vp_draw(uint16_t mode, uint16_t color,
                         uint16_t x1, uint16_t y1,
                         uint16_t x2, uint16_t y2)
{
    uint8_t i = 0;
    i = put8(i, 0x5A);
    i = put8(i, 0xA5);
    i = put8(i, 0x11);          /* body = 17          */
    i = put8(i, 0x82);          /* cmd: VP write      */
    i = put16(i, DWIN_GFX_VP);
    i = put16(i, mode);
    i = put16(i, color);
    i = put16(i, x1);
    i = put16(i, y1);
    i = put16(i, x2);
    i = put16(i, y2);
    i = put16(i, 0x0000);       /* end marker         */
    gfx_send(i);                /* 20 bytes           */
}
#endif

/* ---- Mode constants --------------------------------------------------- */
#define MODE_LINE        0x0002U
#define MODE_RECT        0x0003U
#define MODE_RECT_FILL   0x0004U
#define MODE_CIRCLE      0x0005U
#define MODE_CIRCLE_FILL 0x0006U

/* ---- Public API — NO-OP when GFX_VP_ENABLED == 0 ---------------------- */

void dwin_gfx_rect(uint16_t x1, uint16_t y1,
                    uint16_t x2, uint16_t y2, uint16_t color)
{
#if GFX_VP_ENABLED
    gfx_vp_draw(MODE_RECT, color, x1, y1, x2, y2);
#else
    (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
#endif
}

void dwin_gfx_rect_fill(uint16_t x1, uint16_t y1,
                         uint16_t x2, uint16_t y2, uint16_t color)
{
#if GFX_VP_ENABLED
    gfx_vp_draw(MODE_RECT_FILL, color, x1, y1, x2, y2);
#else
    (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
#endif
}

void dwin_gfx_circle(uint16_t xc, uint16_t yc,
                      uint16_t r, uint16_t color)
{
#if GFX_VP_ENABLED
    gfx_vp_draw(MODE_CIRCLE, color, xc, yc, r, 0);
#else
    (void)xc; (void)yc; (void)r; (void)color;
#endif
}

void dwin_gfx_circle_fill(uint16_t xc, uint16_t yc,
                           uint16_t r, uint16_t color)
{
#if GFX_VP_ENABLED
    gfx_vp_draw(MODE_CIRCLE_FILL, color, xc, yc, r, 0);
#else
    (void)xc; (void)yc; (void)r; (void)color;
#endif
}

void dwin_gfx_line(uint16_t x1, uint16_t y1,
                    uint16_t x2, uint16_t y2, uint16_t color)
{
#if GFX_VP_ENABLED
    gfx_vp_draw(MODE_LINE, color, x1, y1, x2, y2);
#else
    (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
#endif
}

void dwin_gfx_clear_region(uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, uint16_t bg_color)
{
    dwin_gfx_rect_fill(x1, y1, x2, y2, bg_color);
}

void dwin_gfx_status_dot(uint16_t xc, uint16_t yc, uint16_t color)
{
    dwin_gfx_circle_fill(xc, yc, 6U, color);
}

void dwin_gfx_bar(uint16_t x, uint16_t y,
                   uint16_t w, uint16_t h,
                   uint8_t pct,
                   uint16_t fg, uint16_t bg)
{
    if (pct > 100U) pct = 100U;
    dwin_gfx_rect_fill(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U), bg);
    if (pct > 0U) {
        uint16_t fw = (uint16_t)((uint32_t)w * pct / 100U);
        if (fw == 0U) fw = 1U;
        dwin_gfx_rect_fill(x, y, (uint16_t)(x + fw - 1U), (uint16_t)(y + h - 1U), fg);
    }
    dwin_gfx_rect(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U), fg);
}

/* ---- Smoke test: switch to page 1 to verify register writes work ------ */
void dwin_gfx_test(void)
{
    dwin_gfx_page_switch(1);
}