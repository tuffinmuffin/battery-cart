/**
 * display_input.c — see display_input.h for the contract.
 *
 * Per-button state machine:
 *   raw → debounce → edge detect → press-duration classification
 *
 *   on raw change: snapshot the raw level + restart the debounce
 *                  timer. If raw flips back during the window we just
 *                  reset again (the timer never elapses, no state
 *                  change confirmed — that's the debounce).
 *   on debounce window elapsed with raw stable:
 *                  if raw != debounced, confirm the new debounced
 *                  state and dispatch the matching edge handler:
 *                    rising  → record press_start_ms
 *                    falling → if !long_fired and press was shorter
 *                              than the long threshold, enqueue
 *                              SHORT. Reset long_fired.
 *   while held (debounced AND !long_fired):
 *                  if elapsed >= long threshold, enqueue LONG (only
 *                  for NEXT; SELECT has no long-press event), set
 *                  long_fired so we don't repeat.
 */

#include "display_input.h"

#include "direct_io.h"   /* sw_a_read, sw_b_read */

#include <string.h>

#define DEBOUNCE_MS       20U
#define LONG_PRESS_MS     750U
#define EVENT_QUEUE_SIZE  4U

typedef struct {
    bool      raw_pressed;
    bool      debounced_pressed;
    uint32_t  last_change_ms;   /* monotonic; when raw last flipped */
    uint32_t  press_start_ms;   /* when debounced went pressed */
    bool      long_fired;       /* sticky for one hold */
} button_state_t;

static button_state_t s_next;     /* SW_A */
static button_state_t s_select;   /* SW_B */

/* Ring-buffer queue. Single-task usage → no locking. */
static display_event_t s_queue[EVENT_QUEUE_SIZE];
static uint8_t s_q_head;
static uint8_t s_q_tail;
static uint8_t s_q_count;

static void queue_push(display_event_t e)
{
    if (s_q_count >= EVENT_QUEUE_SIZE) {
        return;   /* drop newest on full — controller is too slow */
    }
    s_queue[s_q_tail] = e;
    s_q_tail = (uint8_t)((s_q_tail + 1U) % EVENT_QUEUE_SIZE);
    s_q_count++;
}

void display_input_init(void)
{
    (void)memset(&s_next,   0, sizeof(s_next));
    (void)memset(&s_select, 0, sizeof(s_select));
    s_q_head = 0U;
    s_q_tail = 0U;
    s_q_count = 0U;
}

/* Common button-step logic. `long_event_or_none` is the long-press
 * event to fire for this button, or DISPLAY_EVENT_NEXT_SHORT as a
 * sentinel "no long-press" indicator (with the `has_long_press`
 * flag actually controlling the behaviour — using a separate bool
 * is clearer than overloading an enum value). */
static void process_button(button_state_t *b,
                           uint32_t now_ms,
                           bool raw,
                           display_event_t short_event,
                           bool has_long_press,
                           display_event_t long_event)
{
    /* Debounce: any raw flip restarts the settle timer. */
    if (raw != b->raw_pressed) {
        b->raw_pressed     = raw;
        b->last_change_ms  = now_ms;
    }

    /* Wait for the raw level to be stable for the debounce window
     * before we trust it as a real state change. */
    if ((uint32_t)(now_ms - b->last_change_ms) < DEBOUNCE_MS) {
        return;
    }

    /* Edge handling — only when the debounced state moves. */
    if (b->raw_pressed != b->debounced_pressed) {
        b->debounced_pressed = b->raw_pressed;
        if (b->debounced_pressed) {
            /* Rising edge — start the press timer. */
            b->press_start_ms = now_ms;
            b->long_fired     = false;
        } else {
            /* Falling edge — classify the release:
             *   long_fired         : long-press already fired during
             *                        the hold — no SHORT on release.
             *   has_long_press AND  : long-press button held past the
             *     held >= threshold   threshold but the during-hold
             *                        poll didn't catch it (poll gap);
             *                        fire LONG on release.
             *   else               : short press — either the button
             *                        doesn't have a long-press
             *                        semantic, or the hold was below
             *                        the threshold. */
            const uint32_t held = (uint32_t)(now_ms - b->press_start_ms);
            if (b->long_fired) {
                /* nothing */
            } else if (has_long_press && held >= LONG_PRESS_MS) {
                queue_push(long_event);
            } else {
                queue_push(short_event);
            }
            /* Always reset post-release so the next press starts
             * cleanly even if this one fired long-press already. */
            b->press_start_ms = 0U;
            b->long_fired     = false;
        }
    }

    /* While held, fire long-press exactly once when we cross the
     * threshold. SELECT (has_long_press=false) never fires. */
    if (b->debounced_pressed && !b->long_fired && has_long_press) {
        const uint32_t held = (uint32_t)(now_ms - b->press_start_ms);
        if (held >= LONG_PRESS_MS) {
            queue_push(long_event);
            b->long_fired = true;
        }
    }
}

void display_input_poll(uint32_t now_ms)
{
    process_button(&s_next,   now_ms, sw_a_read(),
                   DISPLAY_EVENT_NEXT_SHORT,
                   true, DISPLAY_EVENT_NEXT_LONG);

    process_button(&s_select, now_ms, sw_b_read(),
                   DISPLAY_EVENT_SELECT_SHORT,
                   false, DISPLAY_EVENT_NEXT_SHORT /* unused */);
}

bool display_input_get_event(display_event_t *out)
{
    if (out == NULL || s_q_count == 0U) {
        return false;
    }
    *out = s_queue[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1U) % EVENT_QUEUE_SIZE);
    s_q_count--;
    return true;
}

void display_input_inject(display_event_t event)
{
    queue_push(event);
}
