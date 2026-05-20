/**
 * display_render.c — see display_render.h for the layout sketch.
 *
 * One snprintf scratch buffer, two fonts, no allocation. Picolibc's
 * integer-only vfprintf (forced via -Wl,--defsym in starm-clang.cmake)
 * handles all the formatting; no float math in this module.
 *
 * The reading on the right (V or A) is right-aligned via
 * u8g2_GetStrWidth so the unit suffix always sits in the same spot
 * regardless of how many digits the number takes.
 *
 * u8g2 font baseline note: u8g2_DrawStr places the GLYPH BASELINE at
 * the given y. For 9x18B (ascent ~14, descent ~3) baseline=15 puts the
 * top of digits near y=1 and descender bottom near y=18. For 5x7
 * (ascent ~6, descent ~1) baseline=31 puts the top near y=25 and the
 * descender bottom right at the panel edge.
 */

#include "display_render.h"

#include "bm_fonts.h"
#include "direct_io.h"   /* led_state() for the heart heartbeat indicator */

#include <stdio.h>
#include <string.h>

/* Big-digit headline baseline (font is 18 px tall, 9 px wide).
 * The top of glyphs lands around y=1, descender bottom around y=18. */
#define HEADLINE_BASELINE_Y    15

/* Status row baseline (5x7 font, baseline at the bottom edge of the panel). */
#define STATUS_BASELINE_Y      31

/* Right-most pixel reserved for headline content (reading + unit).
 * Sits at the right edge of the panel; the bottom tray heart lives
 * below the headline so they don't conflict horizontally. */
#define HEADLINE_RIGHT_X       127

/* Heart heartbeat indicator at bottom-right tray. 5x5 footprint placed
 * so its bottom-right corner is at the panel's bottom-right corner. */
#define HEART_X                123
#define HEART_Y                27

/* Compact tray indicators just left of the heart, 5x7 glyphs with a
 * 3 px gap between each so the letters read as distinct symbols
 * (5x7 glyphs sometimes use the full cell width — a 1 px gap left
 * neighbours close enough to look fused). Letters render only when
 * the corresponding state is active; reserved positions keep the
 * heart from shifting. */
#define INDICATOR_F_X           99  /* fan,   glyph 5 wide, x= 99..103 */
#define INDICATOR_K_X          107  /* relay, glyph 5 wide, x=107..111 */
#define INDICATOR_B_X          115  /* bleed, glyph 5 wide, x=115..119 */

/* Tiny 5x5 heart bitmap:
 *   .#.#.
 *   #####
 *   #####
 *   .###.
 *   ..#..
 * Drawn from primitives so we don't have to vendor an icon font. */
static void draw_heart(u8g2_t *u8g2, uint8_t x, uint8_t y)
{
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 1U), y);
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 3U), y);
    u8g2_DrawHLine(u8g2, x,                       (u8g2_uint_t)(y + 1U), 5);
    u8g2_DrawHLine(u8g2, x,                       (u8g2_uint_t)(y + 2U), 5);
    u8g2_DrawHLine(u8g2, (u8g2_uint_t)(x + 1U),   (u8g2_uint_t)(y + 3U), 3);
    u8g2_DrawPixel(u8g2, (u8g2_uint_t)(x + 2U),   (u8g2_uint_t)(y + 4U));
}

void display_render(u8g2_t *u8g2,
                    const char *status_label,
                    uint32_t charge_time_s,
                    const char *serial,
                    bool show_voltage,
                    const monitor_snapshot_t *s)
{
    if (u8g2 == NULL || s == NULL) {
        return;
    }

    /* Format the right-side reading once before the page loop so we
     * can call u8g2_GetStrWidth on it for right-alignment. */
    char reading[16];
    const char *unit;
    if (show_voltage) {
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

    /* Bottom-left slot — when show_voltage is true we paint the charge
     * timer, when false we paint the battery serial. Same x=0 origin
     * so they replace each other cleanly each cycle. */
    char timer[10];
    const char *bottom_left = NULL;
    if (show_voltage) {
        uint32_t s_total = charge_time_s % (100UL * 3600UL);
        uint32_t hh = s_total / 3600U;
        uint32_t mm = (s_total / 60U) % 60U;
        uint32_t ss = s_total % 60U;
        (void)snprintf(timer, sizeof(timer),
                       "%02lu:%02lu:%02lu",
                       (unsigned long)hh,
                       (unsigned long)mm,
                       (unsigned long)ss);
        bottom_left = timer;
    } else if (serial != NULL && serial[0] != '\0') {
        bottom_left = serial;
    }

    /* Sample the LED state once before the page loop — keeps the heart
     * coherent across all 4 page passes, even if the heartbeat task
     * flips the pin mid-render. */
    const bool heartbeat_on = led_state();

    u8g2_FirstPage(u8g2);
    do {
        /* --- Right-side big-digit reading + unit (right-aligned) -- */
        u8g2_SetFont(u8g2, bm_font_18b);
        const u8g2_uint_t reading_w = u8g2_GetStrWidth(u8g2, reading);

        u8g2_SetFont(u8g2, bm_font_5x7);
        const u8g2_uint_t unit_w = u8g2_GetStrWidth(u8g2, unit);

        /* Lay out so the unit's rightmost pixel sits at HEADLINE_RIGHT_X.
         * Reading sits immediately to its left with 1 px of breathing
         * room. */
        const u8g2_uint_t unit_x    =
            (u8g2_uint_t)(HEADLINE_RIGHT_X - unit_w);
        const u8g2_uint_t reading_x =
            (u8g2_uint_t)(unit_x - reading_w - 1);

        u8g2_SetFont(u8g2, bm_font_18b);
        u8g2_DrawStr(u8g2, reading_x, HEADLINE_BASELINE_Y, reading);

        u8g2_SetFont(u8g2, bm_font_5x7);
        u8g2_DrawStr(u8g2, unit_x, HEADLINE_BASELINE_Y, unit);

        /* --- Left-side status label (medium font, baseline-aligned) -- */
        if (status_label != NULL && status_label[0] != '\0') {
            u8g2_SetFont(u8g2, bm_font_15b);
            u8g2_DrawStr(u8g2, 0, HEADLINE_BASELINE_Y, status_label);
        }

        /* --- Tray: alternating (timer | serial) on the left,
         *           F/K/B indicators on the right -------------- */
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

        /* --- Heart heartbeat indicator (bottom-right tray) ------- */
        if (heartbeat_on) {
            draw_heart(u8g2, HEART_X, HEART_Y);
        }
    } while (u8g2_NextPage(u8g2));
}
