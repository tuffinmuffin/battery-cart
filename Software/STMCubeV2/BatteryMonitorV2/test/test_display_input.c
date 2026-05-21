/**
 * test_display_input.c — host-side unit tests for display_input.c.
 *
 * Covers the button state machine + the event queue: debounce,
 * short / long-press classification, the SELECT-has-no-long-press
 * asymmetry, queue full behaviour, and the inject + get API edge
 * cases.
 *
 * sw_a_read() and sw_b_read() are mocked via mock_direct_io.h so
 * each poll() call consumes one ExpectAndReturn for each. Tests
 * advance time by passing synthetic now_ms values to poll().
 */

#include "unity.h"
#include "display_input.h"

#include "mock_direct_io.h"

/* Mirrors display_input.c's constants. Keep in sync. */
#define DEBOUNCE_MS       20U
#define LONG_PRESS_MS     750U

void setUp(void)
{
    display_input_init();
}

void tearDown(void) { }

/* Helper — set both buttons' next poll to the given raw states. */
static void expect_poll(bool sw_a_raw, bool sw_b_raw)
{
    sw_a_read_ExpectAndReturn(sw_a_raw);
    sw_b_read_ExpectAndReturn(sw_b_raw);
}

/* ---------- API basics ---------- */

void test_init_leaves_queue_empty(void)
{
    display_event_t e;
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

void test_get_event_with_null_returns_false(void)
{
    display_input_inject(DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_FALSE(display_input_get_event(NULL));
    /* Inject is still pending; valid drain returns true. */
    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_SHORT, e);
}

void test_inject_then_get_returns_same_event(void)
{
    display_input_inject(DISPLAY_EVENT_SELECT_SHORT);
    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_SELECT_SHORT, e);
    /* Drained. */
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* ---------- Queue overflow ---------- */

/* Capacity is 4. Inject 5; first 4 survive, 5th is dropped. */
void test_queue_drops_when_full(void)
{
    display_input_inject(DISPLAY_EVENT_NEXT_SHORT);
    display_input_inject(DISPLAY_EVENT_NEXT_LONG);
    display_input_inject(DISPLAY_EVENT_SELECT_SHORT);
    display_input_inject(DISPLAY_EVENT_NEXT_SHORT);
    display_input_inject(DISPLAY_EVENT_NEXT_LONG);    /* dropped */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_SHORT, e);
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_LONG, e);
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_SELECT_SHORT, e);
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_SHORT, e);
    /* 5th was dropped; queue is now empty. */
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* ---------- SELECT (SW_B): short-press only ---------- */

/* Press + release within ~100 ms → SHORT. */
void test_select_short_press_release_fires_short(void)
{
    /* t=0: not pressed (debounce baseline) */
    expect_poll(false, false);
    display_input_poll(0U);

    /* t=10: pressed (raw flip starts debounce) */
    expect_poll(false, true);
    display_input_poll(10U);

    /* t=40: pressed still — debounce window (20 ms) elapsed →
     * rising edge confirmed; no event yet. */
    expect_poll(false, true);
    display_input_poll(40U);

    /* t=80: released (raw flip) */
    expect_poll(false, false);
    display_input_poll(80U);

    /* t=110: released still — debounce window elapsed → falling
     * edge confirmed; press duration ~70 ms < LONG_PRESS_MS →
     * enqueue SELECT_SHORT. */
    expect_poll(false, false);
    display_input_poll(110U);

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_SELECT_SHORT, e);
}

/* SELECT has no long-press event — holding it past the threshold
 * just sits there; on release, SHORT still fires regardless of
 * duration. */
void test_select_long_hold_then_release_still_fires_short(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(false, true);
    display_input_poll(10U);

    /* Hold past the long threshold. */
    expect_poll(false, true);
    display_input_poll(40U);    /* edge confirmed (press) */

    expect_poll(false, true);
    display_input_poll(900U);   /* well past 750 ms — no LONG event */

    /* Release */
    expect_poll(false, false);
    display_input_poll(1000U);

    expect_poll(false, false);
    display_input_poll(1030U);  /* edge confirmed (release) */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_SELECT_SHORT, e);
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* ---------- NEXT (SW_A): short + long ---------- */

void test_next_short_press_release_fires_short(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(10U);

    expect_poll(true, false);
    display_input_poll(40U);    /* press edge */

    expect_poll(false, false);
    display_input_poll(100U);

    expect_poll(false, false);
    display_input_poll(130U);   /* release edge → SHORT */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_SHORT, e);
}

void test_next_long_hold_fires_long_at_threshold(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(10U);

    expect_poll(true, false);
    display_input_poll(40U);    /* press edge confirmed */

    /* Still held, well past threshold — LONG fires once. */
    expect_poll(true, false);
    display_input_poll(800U);

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_LONG, e);
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* Long-press fires exactly once per hold — subsequent polls while
 * still held don't requeue. */
void test_next_long_fires_only_once_per_hold(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(10U);

    expect_poll(true, false);
    display_input_poll(40U);    /* press edge */

    expect_poll(true, false);
    display_input_poll(800U);   /* LONG fires */

    expect_poll(true, false);
    display_input_poll(1500U);  /* still held — no further events */

    expect_poll(true, false);
    display_input_poll(3000U);  /* still held — no further events */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_LONG, e);
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* Poll-gap recovery: if the controller skipped polling for the
 * whole hold (e.g. preempted task) the during-hold long-press
 * check never runs. The falling-edge classifier still sees
 * held >= LONG_PRESS_MS with long_fired == false and queues
 * LONG on release — so the event isn't lost.
 *
 * Sequence: press confirmed at t=40, no polls during the hold,
 * release detected at t=920 → held=880 → LONG via release path. */
void test_next_long_release_with_no_held_poll_fires_long(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(10U);

    expect_poll(true, false);
    display_input_poll(40U);    /* press edge confirmed */

    /* No polls between t=40 and the release. */
    expect_poll(false, false);
    display_input_poll(900U);   /* raw release; debounce starts */

    expect_poll(false, false);
    display_input_poll(920U);   /* release edge confirmed; held=880 ≥ 750
                                 * and long_fired=false → LONG via release. */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_LONG, e);
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* After LONG fired and the user releases, SHORT must NOT fire. */
void test_next_long_then_release_no_short(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(10U);

    expect_poll(true, false);
    display_input_poll(40U);    /* press edge */

    expect_poll(true, false);
    display_input_poll(800U);   /* LONG fires */

    expect_poll(false, false);
    display_input_poll(900U);

    expect_poll(false, false);
    display_input_poll(930U);   /* release edge confirmed */

    display_event_t e;
    TEST_ASSERT_TRUE(display_input_get_event(&e));
    TEST_ASSERT_EQUAL_INT(DISPLAY_EVENT_NEXT_LONG, e);
    /* No SHORT on release after LONG already fired. */
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}

/* ---------- Debounce ---------- */

/* Bounce: raw flips multiple times within the debounce window;
 * never settles long enough to confirm. No edge events fire. */
void test_debounce_rejects_bouncing_press(void)
{
    expect_poll(false, false);
    display_input_poll(0U);

    expect_poll(true, false);
    display_input_poll(5U);     /* raw goes high */

    expect_poll(false, false);
    display_input_poll(12U);    /* raw bounces back low (within debounce) */

    expect_poll(true, false);
    display_input_poll(20U);    /* raw bounces high again */

    expect_poll(false, false);
    display_input_poll(28U);    /* raw bounces low; never stabilised */

    expect_poll(false, false);
    display_input_poll(60U);    /* stable low for >20ms — confirms low,
                                 * matches initial state, no event */

    display_event_t e;
    TEST_ASSERT_FALSE(display_input_get_event(&e));
}
