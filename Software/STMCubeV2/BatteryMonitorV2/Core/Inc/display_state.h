/**
 * display_state.h — internal charge-state enum + user-visible label map.
 *
 * Placeholder for the future charge controller's own state enum. The
 * mapping keeps internal names technically precise (`TRICKLE` = the
 * float / maintenance phase) and display strings user-friendly
 * (`Charged` = "your battery is full, you can take it off").
 *
 * When the real charge state machine lands, either:
 *   (a) this enum is replaced by the controller's `charge_state_t`
 *       and the map shifts to take that type, or
 *   (b) this stays as a translation layer between the controller's
 *       internal enum and the display's vocabulary.
 *
 * Caller must not free returned strings (static storage).
 */

#ifndef DISPLAY_STATE_H
#define DISPLAY_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_STATE_NO_BATT = 0,
    DISPLAY_STATE_CHARGING,
    DISPLAY_STATE_TRICKLE,    /* renders as "Charged" */
    DISPLAY_STATE_TEST,
    DISPLAY_STATE_FAULT,
    DISPLAY_STATE_COUNT,
} display_state_t;

/* Returns the user-visible label for a state. Out-of-range values
 * return "?" so the caller can pass anything safely. */
const char *display_label_for_state(display_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_STATE_H */
