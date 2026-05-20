/**
 * display_render.h — pure render layer for the 128x32 OLED stats screen.
 *
 * Stateless: takes a u8g2 instance, a charging-state label, a mode bit
 * (show V or A), and a monitor_snapshot_t. Paints the current "live
 * stats" screen via u8g2_FirstPage / u8g2_NextPage. No FreeRTOS, no
 * HAL, no globals beyond const fonts — the goal is that this same
 * function can be linked into the desktop SDL sim (tools/sim/) for
 * layout iteration without the firmware glue.
 *
 * Layout (128x32):
 *
 *   .--------------------------------------------------------.
 *   | Charging                            13.5V  |  bm_state_label
 *   |                                            |  + bm_big_digits
 *   |                                            |  + bm_small_status
 *   | 00:23:45                       F K B  [♥]  |  tray (5x7):
 *   '--------------------------------------------------------'  alternating
 *                                                                bottom-left
 *                                                                slot + F/K/B
 *                                                                indicators +
 *                                                                heartbeat.
 *
 * The tray indicators are minimal and only appear when active:
 *   F — fan_duty_pct > 0
 *   K — k1_on
 *   B — bleed_on
 *   ♥ — blinks in time with the MCU LED via led_state()
 *
 * The bottom-left slot alternates in sync with `show_voltage`:
 *   show_voltage == true  → "HH:MM:SS"  (formatted from charge_time_s)
 *   show_voltage == false → serial      (caller-supplied string)
 *
 * The caller (display_task or the SDL sim) decides which reading to
 * show, which status label to use, what charge_time_s value to use,
 * and what serial string to use — display_render just paints.
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

/* Paint one full frame.
 *   `status_label`    — short string for the left side of the headline
 *                       ("Charging", "No Batt", "Trickle", etc.).
 *                       Pass NULL or "" to omit.
 *   `charge_time_s`   — seconds elapsed since charge start. Rendered
 *                       as "HH:MM:SS" in the bottom-left slot when
 *                       show_voltage is true. Wraps at 100 hours.
 *   `serial`          — battery serial string (e.g. "S/N: AB12CD34"
 *                       from a future NFC read) shown in the
 *                       bottom-left slot when show_voltage is false.
 *                       Pass NULL or "" to leave the slot blank.
 *   `show_voltage`    — true: render vbus on the right + timer on the
 *                       bottom-left; false: render current on the
 *                       right + serial on the bottom-left. Caller
 *                       toggles to alternate.
 *   `s`               — non-NULL snapshot from monitor_state_get(). */
void display_render(u8g2_t *u8g2,
                    const char *status_label,
                    uint32_t charge_time_s,
                    const char *serial,
                    bool show_voltage,
                    const monitor_snapshot_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_RENDER_H */
