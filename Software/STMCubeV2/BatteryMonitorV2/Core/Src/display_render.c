/**
 * display_render.c — see display_render.h for the per-view layout
 * notes and the dispatch contract.
 *
 * One snprintf scratch buffer, three fonts, no allocation. Picolibc's
 * integer-only vfprintf (forced via -Wl,--defsym in starm-clang.cmake)
 * handles all formatting; no float math in this module.
 *
 * u8g2 font baseline note: u8g2_DrawStr places the GLYPH BASELINE at
 * the given y. For bm_font_18b (ascent ~14, descent ~3) baseline=15
 * puts the top of digits near y=1 and descender bottom near y=18.
 * For bm_font_5x7 (ascent ~6, descent ~1) baseline=31 puts the top
 * near y=25 and the descender bottom right at the panel edge.
 *
 * Stub views (ALT_THERMAL / ALT_COOLING / ALT_BATTERY / MENU / IDLE
 * / FAULT / DEBUG) share render_stub_view() which paints the view's
 * name centered with bm_font_15b plus a small "TODO" subtitle. Real
 * per-view layouts land as their dependencies arrive (NFC for
 * ALT_BATTERY, charge state machine for FAULT, etc.).
 */

#include "display_render.h"

#include "bm_fonts.h"
#include "direct_io.h"   /* led_state() for the heart heartbeat indicator */

#include <stdio.h>
#include <string.h>

/* --- MAIN view layout constants ------------------------------------ */

/* Big-digit headline baseline (bm_font_18b — 18 px tall, 9 px wide). */
#define HEADLINE_BASELINE_Y    15

/* Status row baseline (bm_font_5x7 — baseline at the bottom edge). */
#define STATUS_BASELINE_Y      31

/* Right edge for headline content (reading + unit). */
#define HEADLINE_RIGHT_X       127

/* Heart heartbeat indicator at bottom-right tray. 5x5 footprint
 * placed so its bottom-right corner is at the panel's bottom-right
 * corner. */
#define HEART_X                123
#define HEART_Y                27

/* Compact tray indicators just left of the heart, 5x7 glyphs with a
 * 3 px gap between each so the letters read as distinct symbols
 * (5x7 glyphs sometimes use the full cell width — a 1 px gap left
 * neighbours close enough to look fused). Letters render only when
 * the corresponding state is active; reserved positions keep the
 * heart from shifting. */
#define INDICATOR_F_X           99
#define INDICATOR_K_X          107
#define INDICATOR_B_X          115

/* --- Stub view layout constants ------------------------------------ */

/* Centered "View Name" label in the medium font (bm_font_15b). */
#define STUB_LABEL_BASELINE_Y   12

/* Centered "TODO" subtitle in the small font (bm_font_5x7),
 * baseline-aligned with the bottom edge. */
#define STUB_TODO_BASELINE_Y    27

/* Tiny 5x5 heart bitmap (see header for the pattern). */
static void draw_heart(u8g2_t *u8g2, uint8_t x, uint8_t y)
{
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 1U), y);
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 3U), y);
    u8g2_DrawHLine(u8g2, x,                       (u8g2_uint_t)(y + 1U), 5);
    u8g2_DrawHLine(u8g2, x,                       (u8g2_uint_t)(y + 2U), 5);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 1U),   (u8g2_uint_t)(y + 3U), 3);
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 2U),   (u8g2_uint_t)(y + 4U));
}

/* Compute the x offset that horizontally centers `str` rendered with
 * the currently-set font. Falls back to 0 if the string is wider
 * than the panel. */
static u8g2_uint_t centered_x(u8g2_t *u8g2, const char *str)
{
    u8g2_uint_t w = u8g2_GetStrWidth(u8g2, str);
    if (w >= 128U) {
        return 0;
    }
    return (u8g2_uint_t)((128U - w) / 2U);
}

/* --- MAIN view (real layout) --------------------------------------- */

/* TODO: when charge_pct is in range (0..100, not 0xFF) draw it in the
 * tray. Layout currently shows timer OR serial alternating; charge
 * indicator placement lands when the real charge controller starts
 * publishing the value. */
static void render_main_view(u8g2_t *u8g2, const display_main_ctx_t *m)
{
    const monitor_snapshot_t *s = m->snap;
    if (s == NULL) {
        return;
    }

    /* Format the right-side reading once before the page loop so we
     * can call u8g2_GetStrWidth on it for right-alignment. */
    char reading[16];
    const char *unit;
    if (m->show_voltage) {
        unit = "V";
        (void)snprintf(reading, sizeof(reading), "%lu.%lu",
                       (unsigned long)(s->vbus_mv / 1000U),
                       (unsigned long)((s->vbus_mv % 1000U) / 100U));
    } else {
        unit = "A";
        long ma = (long)s->current_ma;
        char sign = ' ';
        if (ma < 0) {
            sign = '-';
            ma = -ma;
        }
        (void)snprintf(reading, sizeof(reading), "%c%ld.%ld",
                       sign,
                       ma / 1000,
                       (ma % 1000) / 100);
    }

    /* Bottom-left slot — timer when show_voltage is true, serial when
     * false. Same x=0 origin so they replace each other cleanly. */
    char timer[10];
    const char *bottom_left = NULL;
    if (m->show_voltage) {
        uint32_t s_total = m->charge_time_s % (100UL * 3600UL);
        uint32_t hh = s_total / 3600U;
        uint32_t mm = (s_total / 60U) % 60U;
        uint32_t ss = s_total % 60U;
        (void)snprintf(timer, sizeof(timer),
                       "%02lu:%02lu:%02lu",
                       (unsigned long)hh,
                       (unsigned long)mm,
                       (unsigned long)ss);
        bottom_left = timer;
    } else if (m->serial != NULL && m->serial[0] != '\0') {
        bottom_left = m->serial;
    }

    /* Sample the LED state once before the page loop — keeps the
     * heart coherent across all 4 page passes even if the heartbeat
     * task flips the pin mid-render. */
    const bool heartbeat_on = led_state();

    u8g2_FirstPage(u8g2);
    do {
        /* Right-side big-digit reading + unit (right-aligned). */
        u8g2_SetFont(u8g2, bm_font_18b);
        const u8g2_uint_t reading_w = u8g2_GetStrWidth(u8g2, reading);

        u8g2_SetFont(u8g2, bm_font_5x7);
        const u8g2_uint_t unit_w = u8g2_GetStrWidth(u8g2, unit);

        const u8g2_uint_t unit_x    =
            (u8g2_uint_t)(HEADLINE_RIGHT_X - unit_w);
        const u8g2_uint_t reading_x =
            (u8g2_uint_t)(unit_x - reading_w - 1);

        u8g2_SetFont(u8g2, bm_font_18b);
        u8g2_DrawStr(u8g2, reading_x, HEADLINE_BASELINE_Y, reading);

        u8g2_SetFont(u8g2, bm_font_5x7);
        u8g2_DrawStr(u8g2, unit_x, HEADLINE_BASELINE_Y, unit);

        /* Left-side status label (medium font, baseline-aligned). */
        if (m->status_label != NULL && m->status_label[0] != '\0') {
            u8g2_SetFont(u8g2, bm_font_15b);
            u8g2_DrawStr(u8g2, 0, HEADLINE_BASELINE_Y, m->status_label);
        }

        /* Tray: alternating (timer | serial) on the left, F/K/B
         * indicators on the right. */
        u8g2_SetFont(u8g2, bm_font_5x7);
        if (bottom_left != NULL) {
            u8g2_DrawStr(u8g2, 0, STATUS_BASELINE_Y, bottom_left);
        }
        if (s->fan_duty_pct > 0U) {
            u8g2_DrawStr(u8g2, INDICATOR_F_X, STATUS_BASELINE_Y, "F");
        }
        if (s->k1_on) {
            u8g2_DrawStr(u8g2, INDICATOR_K_X, STATUS_BASELINE_Y, "K");
        }
        if (s->bleed_on) {
            u8g2_DrawStr(u8g2, INDICATOR_B_X, STATUS_BASELINE_Y, "B");
        }

        /* Heart heartbeat indicator (bottom-right tray). */
        if (heartbeat_on) {
            draw_heart(u8g2, HEART_X, HEART_Y);
        }
    } while (u8g2_NextPage(u8g2));
}

/* --- Stub views ---------------------------------------------------- */

/* Shared layout for the stub views — view name in the medium font
 * centered horizontally + "TODO" below in the small font. Visible
 * proof on hardware that the dispatcher reached this branch. */
static void render_stub_view(u8g2_t *u8g2, const char *name)
{
    if (name == NULL) {
        return;
    }
    u8g2_FirstPage(u8g2);
    do {
        u8g2_SetFont(u8g2, bm_font_15b);
        u8g2_DrawStr(u8g2,
                     centered_x(u8g2, name),
                     STUB_LABEL_BASELINE_Y,
                     name);

        u8g2_SetFont(u8g2, bm_font_5x7);
        u8g2_DrawStr(u8g2,
                     centered_x(u8g2, "TODO"),
                     STUB_TODO_BASELINE_Y,
                     "TODO");
    } while (u8g2_NextPage(u8g2));
}

/* --- Dispatcher ---------------------------------------------------- */

void display_render(u8g2_t *u8g2, const display_ctx_t *ctx)
{
    if (u8g2 == NULL || ctx == NULL) {
        return;
    }

    switch (ctx->view) {
        case DISPLAY_VIEW_MAIN:
            render_main_view(u8g2, &ctx->u.main);
            break;
        case DISPLAY_VIEW_ALT_THERMAL:
            render_stub_view(u8g2, "Thermal");
            break;
        case DISPLAY_VIEW_ALT_COOLING:
            render_stub_view(u8g2, "Cooling");
            break;
        case DISPLAY_VIEW_ALT_BATTERY:
            render_stub_view(u8g2, "Battery");
            break;
        case DISPLAY_VIEW_MENU:
            render_stub_view(u8g2, "Menu");
            break;
        case DISPLAY_VIEW_IDLE:
            render_stub_view(u8g2, "Idle");
            break;
        case DISPLAY_VIEW_FAULT:
            render_stub_view(u8g2, "Fault");
            break;
        case DISPLAY_VIEW_DEBUG:
            render_stub_view(u8g2, "Debug");
            break;
        default:
            /* Unknown view id — leave the panel as it was. */
            break;
    }
}
