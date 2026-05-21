/**
 * display_render.h — pure render layer for the 128x32 OLED, dispatched
 * by view ID.
 *
 * Stateless functions, one logical per-view body. A single public
 * entry point — display_render(u8g2, ctx) — switches on `ctx->view`
 * and calls the right per-view helper. The caller (display_controller
 * today, the future SDL sim) decides which view to render and supplies
 * the view-specific context payload.
 *
 * Views — `display_view_t`:
 *
 *   DISPLAY_VIEW_MAIN          live stats (status label + V/A + tray)
 *   DISPLAY_VIEW_ALT_THERMAL   thermal telem detail   (stub today)
 *   DISPLAY_VIEW_ALT_COOLING   cooling / fan detail   (stub today)
 *   DISPLAY_VIEW_ALT_BATTERY   battery info / NFC     (stub today)
 *   DISPLAY_VIEW_MENU          settings / params menu (stub today)
 *   DISPLAY_VIEW_IDLE          screensaver            (stub today)
 *   DISPLAY_VIEW_FAULT         fault override         (stub today)
 *   DISPLAY_VIEW_DEBUG         free-form debug text   (stub today)
 *
 * MAIN layout (128x32) — only fully-implemented view in v1:
 *
 *   .--------------------------------------------------------.
 *   | Charging                            13.5V  |  bm_font_15b
 *   |                                            |  + bm_font_18b
 *   |                                            |  + bm_font_5x7
 *   | 00:23:45                       F K B  [♥]  |  tray (5x7):
 *   '--------------------------------------------------------'  alternating
 *                                                                bottom-left
 *                                                                slot + F/K/B
 *                                                                indicators +
 *                                                                heartbeat.
 *
 * Tray indicators appear only when active:
 *   F — fan_duty_pct > 0   K — k1_on   B — bleed_on
 *   ♥ — blinks in time with the MCU LED via led_state()
 *
 * The stub views each draw their name centered (big font) plus a
 * "TODO" subtitle (small font) — they're reachable by button input
 * (NEXT cycles through MAIN → ALT_*) so the dispatch can be eyeballed
 * on hardware without faking state.
 */

#ifndef DISPLAY_RENDER_H
#define DISPLAY_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "monitor_state.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_VIEW_MAIN = 0,
    DISPLAY_VIEW_ALT_THERMAL,
    DISPLAY_VIEW_ALT_COOLING,
    DISPLAY_VIEW_ALT_BATTERY,
    DISPLAY_VIEW_MENU,
    DISPLAY_VIEW_IDLE,
    DISPLAY_VIEW_FAULT,
    DISPLAY_VIEW_DEBUG,
    DISPLAY_VIEW_COUNT,
} display_view_t;

/* --- Per-view contexts --------------------------------------------
 * Each view declares only the data it actually consumes. Adding a
 * field to one view is local; adding a new view is one enum entry
 * + one struct + one case in the dispatcher.
 */

/* MAIN — the headline view. Status label comes from
 * display_state.h's map (caller maps a charge_state_t to a string).
 * charge_pct: 0..100 means "show me", 0xFF means "hide" (no estimate
 * available). */
typedef struct {
    const char *status_label;
    uint32_t    charge_time_s;
    const char *serial;
    uint8_t     charge_pct;
    bool        show_voltage;
    const monitor_snapshot_t *snap;
} display_main_ctx_t;

typedef struct { const monitor_snapshot_t *snap; } display_thermal_ctx_t;
typedef struct { const monitor_snapshot_t *snap; } display_cooling_ctx_t;
typedef struct { const char *serial; }             display_battery_ctx_t;

typedef struct {
    uint8_t cursor;              /* highlighted item index */
    uint8_t count;
    const char *const *items;    /* item label array */
} display_menu_ctx_t;

typedef struct {
    const char *code;            /* e.g. "OVERCURRENT" — required */
    const char *detail;          /* nullable second line */
} display_fault_ctx_t;

typedef struct {
    uint32_t frame;              /* monotonic for animation */
} display_idle_ctx_t;

typedef struct {
    display_view_t view;
    union {
        display_main_ctx_t     main;
        display_thermal_ctx_t  thermal;
        display_cooling_ctx_t  cooling;
        display_battery_ctx_t  battery;
        display_menu_ctx_t     menu;
        display_fault_ctx_t    fault;
        display_idle_ctx_t     idle;
    } u;
} display_ctx_t;

/* Single entry point — dispatches on ctx->view. Safe with NULL u8g2,
 * NULL ctx, and any out-of-range view (all no-op). */
void display_render(u8g2_t *u8g2, const display_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_RENDER_H */
