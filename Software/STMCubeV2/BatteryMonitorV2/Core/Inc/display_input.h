/**
 * display_input.h — button input layer for the 2-button display UI.
 *
 * Reads SW_A (NEXT) and SW_B (SELECT) via direct_io's wrappers,
 * runs debounce + short / long-press classification, and posts
 * events to a small internal queue the display controller drains
 * each tick.
 *
 * Event semantics (matches docs/display_requirements.md):
 *   NEXT_SHORT    — SW_A pressed and released within the threshold
 *   NEXT_LONG     — SW_A held past the threshold (~750 ms); fires
 *                   ONCE while held, NEXT_SHORT does not fire on
 *                   the subsequent release
 *   SELECT_SHORT  — SW_B pressed and released (any duration; no
 *                   SELECT_LONG exists in the nav table)
 *
 * The clock is injected via display_input_poll(now_ms) so the
 * module is host-testable without mocking osKernelGetTickCount.
 * Production callers pass osKernelGetTickCount().
 *
 * Single-task usage assumed (the display controller owns polling
 * + dequeue + injection). No internal locking.
 */

#ifndef DISPLAY_INPUT_H
#define DISPLAY_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_EVENT_NEXT_SHORT = 0,
    DISPLAY_EVENT_NEXT_LONG,
    DISPLAY_EVENT_SELECT_SHORT,
} display_event_t;

/* Reset internal state (debounce + press timers + queue). Safe to
 * call multiple times; idempotent. */
void display_input_init(void);

/* One tick of polling work. Reads sw_a_read() / sw_b_read(), runs
 * debounce + edge classification, enqueues events. `now_ms` should
 * be a monotonic tick count. Call every controller tick (typically
 * every 200 ms). */
void display_input_poll(uint32_t now_ms);

/* Non-blocking dequeue. Returns true + writes the event when one is
 * available, false (and leaves *out untouched) when empty. NULL-safe
 * (returns false). */
bool display_input_get_event(display_event_t *out);

/* Push an event directly, bypassing the polling layer. Used by unit
 * tests and the future SDL desktop sim. Same drop-on-full semantics
 * as polled events. */
void display_input_inject(display_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INPUT_H */
