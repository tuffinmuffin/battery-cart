/**
 * display_state.c — see display_state.h for the contract.
 *
 * Indexed-table lookup with a default catch-all so a future enum
 * value the producer didn't update us about renders as "?" rather
 * than dereferencing past the table.
 */

#include "display_state.h"

static const char *const kLabels[DISPLAY_STATE_COUNT] = {
    [DISPLAY_STATE_NO_BATT]  = "No Batt",
    [DISPLAY_STATE_CHARGING] = "Charging",
    [DISPLAY_STATE_TRICKLE]  = "Charged",   /* see header — user-visible */
    [DISPLAY_STATE_TEST]     = "Test",
    [DISPLAY_STATE_FAULT]    = "Fault",
};

const char *display_label_for_state(display_state_t state)
{
    if ((unsigned)state >= (unsigned)DISPLAY_STATE_COUNT) {
        return "?";
    }
    return kLabels[state];
}
