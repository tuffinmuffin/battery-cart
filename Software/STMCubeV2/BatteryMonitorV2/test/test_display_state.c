/**
 * test_display_state.c — host-side unit tests for display_state.c.
 *
 * Tiny static lookup; tests assert every mapping including the
 * intentional TRICKLE → "Charged" rename and the out-of-range
 * catch-all that returns "?".
 */

#include "unity.h"
#include "display_state.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_no_batt_maps_to_no_batt_label(void)
{
    TEST_ASSERT_EQUAL_STRING("No Batt",
                             display_label_for_state(DISPLAY_STATE_NO_BATT));
}

void test_charging_maps_to_charging_label(void)
{
    TEST_ASSERT_EQUAL_STRING("Charging",
                             display_label_for_state(DISPLAY_STATE_CHARGING));
}

/* The internal TRICKLE state intentionally renders as user-visible
 * "Charged" — this test fails if someone "fixes" the rename. */
void test_trickle_maps_to_charged_label(void)
{
    TEST_ASSERT_EQUAL_STRING("Charged",
                             display_label_for_state(DISPLAY_STATE_TRICKLE));
}

void test_test_maps_to_test_label(void)
{
    TEST_ASSERT_EQUAL_STRING("Test",
                             display_label_for_state(DISPLAY_STATE_TEST));
}

void test_fault_maps_to_fault_label(void)
{
    TEST_ASSERT_EQUAL_STRING("Fault",
                             display_label_for_state(DISPLAY_STATE_FAULT));
}

/* Out-of-range value (e.g. a future enum extension we haven't yet
 * mapped) must return "?" — never out-of-bounds access. Cast through
 * int to silence enum-range warnings on the test build. */
void test_out_of_range_returns_question_mark(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "?", display_label_for_state((display_state_t)DISPLAY_STATE_COUNT));
    TEST_ASSERT_EQUAL_STRING(
        "?", display_label_for_state((display_state_t)0xFFU));
}
