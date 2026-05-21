/**
 * display_controller.h — FreeRTOS task + view state machine.
 *
 * Owns the current view, menu cursor, fault state, and the demo
 * data sweep (TEMP — replaced when the real charge controller +
 * INA238 producer wire up to monitor_state). Each tick:
 *
 *   1. display_input_poll(now_ms)
 *   2. drain display_input_get_event() — apply nav transitions per
 *      docs/display_requirements.md
 *   3. if no input for 30 s AND 8 s since last demo step, advance
 *      view within {MAIN, ALT_THERMAL, ALT_COOLING, ALT_BATTERY}
 *   4. if fault_active, override effective view = FAULT
 *   5. build display_ctx_t for the effective view; display_render
 *
 * Logic (display_controller_tick) is host-testable without the
 * kernel — pass a synthetic `now_ms` and check `display_controller_view()`
 * after. The render call is split into display_controller_render()
 * which is a no-op when ssd1306_u8g2() returns NULL.
 */

#ifndef DISPLAY_CONTROLLER_H
#define DISPLAY_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "display_render.h"   /* display_view_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Reset internal state; sets view = MAIN, clears fault, etc.
 * Idempotent. */
void display_controller_init(void);

/* Spawn the FreeRTOS task (production entry point — replaces the
 * old display_task_start). Calls _init internally if not already. */
void display_controller_start(void);

/* One step of controller logic. Drains input, applies nav,
 * advances demo cycle. Does NOT render — use _render() for that.
 * Splitting is what makes this host-testable. */
void display_controller_tick(uint32_t now_ms);

/* Render the current effective view (fault override applied) using
 * the live monitor_state snapshot + the demo data sweep. No-op if
 * ssd1306 hasn't been initialised yet. */
void display_controller_render(void);

/* Current navigated view ID (ignores fault override). */
display_view_t display_controller_view(void);

/* Currently-highlighted menu item index. Meaningful only when
 * display_controller_view() == DISPLAY_VIEW_MENU. */
uint8_t display_controller_menu_cursor(void);

/* Is a fault currently active (overrides the rendered view)? */
bool display_controller_fault_active(void);

/* Future API — surfaces a fault to the user. The charge controller
 * will call this when an interlock fires; today no producer exists
 * (the fault stays clear unless tests inject). Strings must remain
 * valid for the lifetime of the fault (typically string literals or
 * controller-owned buffers — the controller copies into a small
 * internal buffer so caller storage doesn't have to outlive the
 * fault). */
void display_controller_set_fault(const char *code, const char *detail);
void display_controller_clear_fault(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_CONTROLLER_H */
