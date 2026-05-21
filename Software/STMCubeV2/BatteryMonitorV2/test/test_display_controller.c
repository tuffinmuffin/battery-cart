/**
 * test_display_controller.c — host-side unit tests for display_controller.c.
 *
 * Covers the navigation state machine (NEXT cycles MAIN→ALT_*, NEXT-long
 * back to MAIN, SELECT enters MENU, menu cursor advances, etc.), the
 * demo auto-cycle after inactivity, and the fault override.
 *
 * display_controller.c includes ssd1306.h / monitor_state.h /
 * display_render.h / cdc_print.h / display_input.h. Tests mock the
 * ones whose calls the tick path can touch (display_input, cdc_print);
 * render-path symbols (ssd1306, monitor_state, display_render) are
 * mocked too so the test binary links cleanly even though the tests
 * don't call _render().
 */

#include "unity.h"
#include "display_controller.h"

#include "mock_cmsis_os2.h"
#include "mock_display_input.h"
#include "mock_display_render.h"
#include "mock_display_state.h"
#include "mock_monitor_state.h"
#include "mock_ssd1306.h"

#include <stdarg.h>
#include <string.h>

/* cdc_print.h declares cdc_vprintf(const char *, va_list); CMock can't
 * auto-mock va_list arguments. We don't need to assert on log content
 * either — the controller calls cdc_printf for diagnostic output only.
 * Stub it locally as a no-op so the linker resolves the symbol without
 * needing the real cdc_print.c (which would pull in TinyUSB). */
int cdc_printf(const char *fmt, ...);
int cdc_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

/* Internal constants we need to match for cycle-timing tests. Keep in
 * sync with display_controller.c. */
#define DEMO_CYCLE_MS         8000U
#define DEMO_INACTIVITY_MS    30000U
#define FAULT_LOCK_MS         100U

/* Sentinel mutex handle. CMock matches by value, so any non-NULL works.
 * Static across the whole test binary because s_fault_mutex inside
 * display_controller.c is itself a file-scope static (first init() call
 * wins, subsequent inits skip osMutexNew). */
static int s_mutex_storage;
#define FAKE_MUTEX ((osMutexId_t)&s_mutex_storage)

void setUp(void)
{
    /* osMutexNew is only called the first time display_controller_init
     * runs (s_fault_mutex is a file-scope static — it persists across
     * tests). _Ignore covers both the first real call and the no-op
     * subsequent ones without polluting per-test expectation order. */
    osMutexNew_IgnoreAndReturn(FAKE_MUTEX);
    display_controller_init();
}

void tearDown(void) { }

/* Each set_fault / clear_fault / fault_active call takes the lock
 * exactly once. Render also takes it once on every call (to snapshot
 * the fault buffers). CMock won't let Ignore + Expect coexist on the
 * same function, so we register the acquire/release pair concretely
 * for every fault-touching operation. */
static void expect_fault_lock(void)
{
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, FAULT_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);
}

/* Drive a single tick where the input layer reports no events. The
 * controller calls poll once and get_event once (returning false). */
static void tick_with_no_input(uint32_t now_ms)
{
    display_input_poll_Expect(now_ms);
    display_input_get_event_ExpectAnyArgsAndReturn(false);
    display_controller_tick(now_ms);
}

/* Drive a single tick where the input layer produces ONE event. */
static void tick_with_event(uint32_t now_ms, display_event_t event)
{
    display_event_t e = event;
    display_input_poll_Expect(now_ms);
    display_input_get_event_ExpectAnyArgsAndReturn(true);
    display_input_get_event_ReturnThruPtr_out(&e);
    display_input_get_event_ExpectAnyArgsAndReturn(false);
    display_controller_tick(now_ms);
}

/* ---------- init ---------- */

void test_init_starts_in_main_view(void)
{
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
    TEST_ASSERT_EQUAL_UINT8(0U, display_controller_menu_cursor());
    expect_fault_lock();
    TEST_ASSERT_FALSE(display_controller_fault_active());
}

/* ---------- MAIN / ALT_* nav ---------- */

void test_next_short_cycles_main_to_alt_thermal(void)
{
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());
}

void test_next_short_full_cycle_wraps_back_to_main(void)
{
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());
    tick_with_event(100U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_COOLING, display_controller_view());
    tick_with_event(200U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_BATTERY, display_controller_view());
    tick_with_event(300U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
}

void test_next_long_from_any_alt_jumps_back_to_main(void)
{
    /* Cycle into ALT_COOLING then long-press back. */
    tick_with_event(0U,   DISPLAY_EVENT_NEXT_SHORT);
    tick_with_event(100U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_COOLING, display_controller_view());

    tick_with_event(200U, DISPLAY_EVENT_NEXT_LONG);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
}

void test_select_short_enters_menu_from_main(void)
{
    tick_with_event(0U, DISPLAY_EVENT_SELECT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());
    TEST_ASSERT_EQUAL_UINT8(0U, display_controller_menu_cursor());
}

void test_select_short_enters_menu_from_alt_view(void)
{
    tick_with_event(0U,   DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());

    tick_with_event(100U, DISPLAY_EVENT_SELECT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());
}

/* ---------- MENU nav ---------- */

void test_menu_next_short_advances_cursor(void)
{
    tick_with_event(0U,   DISPLAY_EVENT_SELECT_SHORT);
    tick_with_event(100U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_UINT8(1U, display_controller_menu_cursor());
    tick_with_event(200U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_UINT8(2U, display_controller_menu_cursor());
}

/* Cursor wraps at the end of the menu (5 items today: Force test,
 * Reset, Firmware update, About, Status). Verify wrap to 0. */
void test_menu_cursor_wraps_at_end(void)
{
    tick_with_event(0U, DISPLAY_EVENT_SELECT_SHORT);
    /* Advance 5 times — 0→1→2→3→4→0 */
    for (uint32_t t = 100U; t < 600U; t += 100U) {
        tick_with_event(t, DISPLAY_EVENT_NEXT_SHORT);
    }
    TEST_ASSERT_EQUAL_UINT8(0U, display_controller_menu_cursor());
}

void test_menu_next_long_exits_to_main(void)
{
    tick_with_event(0U,   DISPLAY_EVENT_SELECT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());

    tick_with_event(100U, DISPLAY_EVENT_NEXT_LONG);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
}

/* SELECT inside the MENU triggers the (TODO) per-item handler. Today
 * the handler just logs via cdc_printf (stubbed as no-op in this test
 * file — see top), so the test only verifies the controller doesn't
 * crash and the view stays in MENU. */
void test_menu_select_stays_in_menu(void)
{
    tick_with_event(0U, DISPLAY_EVENT_SELECT_SHORT);   /* enter MENU */
    tick_with_event(100U, DISPLAY_EVENT_SELECT_SHORT); /* select item 0 */
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());
}

/* ---------- Auto-cycle demo ---------- */

/* Tick once at t=0, then again at t=DEMO_INACTIVITY_MS+DEMO_CYCLE_MS
 * with no input — should advance from MAIN to ALT_THERMAL. */
void test_auto_cycle_after_inactivity(void)
{
    /* First tick — initial state, no input, no cycle yet. */
    tick_with_no_input(0U);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());

    /* Second tick — past the inactivity gap + the cycle interval. */
    tick_with_no_input(DEMO_INACTIVITY_MS + DEMO_CYCLE_MS);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());
}

/* A button press resets the inactivity timer, so a tick that would
 * have auto-cycled doesn't. */
void test_input_pauses_auto_cycle(void)
{
    tick_with_no_input(0U);
    /* Press something — pauses the demo. */
    tick_with_event(1000U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());

    /* A tick that's past inactivity+cycle from t=0 but the user
     * pressed at t=1000, so the inactivity counter restarted. */
    tick_with_no_input(1000U + DEMO_INACTIVITY_MS - 1U);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());
}

/* Auto-cycle doesn't fire when the user is in MENU / IDLE / FAULT
 * / DEBUG — those need explicit triggers. */
void test_auto_cycle_does_not_fire_in_menu(void)
{
    tick_with_event(0U, DISPLAY_EVENT_SELECT_SHORT);   /* into MENU */
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());

    /* Even past the inactivity gap, MENU shouldn't auto-cycle. */
    tick_with_no_input(DEMO_INACTIVITY_MS + DEMO_CYCLE_MS + 100U);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());
}

/* ---------- Fault override ---------- */

void test_set_fault_marks_fault_active(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", "32 A > 30 A limit");
    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
}

void test_clear_fault_marks_fault_inactive(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);
    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
    expect_fault_lock();
    display_controller_clear_fault();
    expect_fault_lock();
    TEST_ASSERT_FALSE(display_controller_fault_active());
}

/* Fault override doesn't clobber the user's navigated view — when
 * the fault clears, the rendered view returns to wherever the user
 * was. The override is applied only inside _render (not visible here)
 * — view() reports the navigated view always. */
void test_fault_does_not_change_navigated_view(void)
{
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());

    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());
    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
}

void test_set_fault_null_code_keeps_string_empty(void)
{
    expect_fault_lock();
    display_controller_set_fault(NULL, NULL);
    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
    /* Internal buffer is empty — render layer will skip the code
     * line. We can't inspect the buffer from here without a getter,
     * but the fault_active flag is enough for the test contract. */
}

/* ---------- handle_event arms for IDLE / FAULT / DEBUG ----------
 * These views aren't reachable from the current public input/nav
 * path (no producer sets s_state.view to them yet), but their
 * handle_event arms are scaffolding for the future fault-acknowledge
 * + idle-screen-exit + debug-view-exit flows. Cover them via the
 * test-only display_controller_force_view seam so the contract is
 * pinned before producers land.
 */

void test_event_in_idle_returns_to_main(void)
{
    display_controller_force_view(DISPLAY_VIEW_IDLE);
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
}

/* Short-press in FAULT view returns to MAIN but leaves the fault
 * active — the user has only acknowledged the screen, not the
 * condition. Long-press both clears and exits (see next test). */
void test_short_event_in_fault_returns_to_main_without_clearing(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);
    display_controller_force_view(DISPLAY_VIEW_FAULT);

    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);

    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
}

/* Long-press in FAULT view = "acknowledge" — clears the fault AND
 * returns to MAIN. Exercises the inner `if (view==FAULT &&
 * e==NEXT_LONG)` branch in handle_event. CMock is in strict-order
 * mode (test/project.yml), so the clear_fault mutex pair has to be
 * registered between the two display_input_get_event calls — that's
 * where it fires during tick. tick_with_event() can't express that
 * interleaving, so this test inlines the expectations. */
void test_long_event_in_fault_clears_and_returns_to_main(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);
    display_controller_force_view(DISPLAY_VIEW_FAULT);

    /* Tick sequence: poll → get_event(true, NEXT_LONG) → handle_event
     * runs clear_fault (acquire+release) → get_event(false). */
    display_event_t e = DISPLAY_EVENT_NEXT_LONG;
    display_input_poll_Expect(0U);
    display_input_get_event_ExpectAnyArgsAndReturn(true);
    display_input_get_event_ReturnThruPtr_out(&e);
    expect_fault_lock();   /* clear_fault inside handle_event */
    display_input_get_event_ExpectAnyArgsAndReturn(false);
    display_controller_tick(0U);

    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
    expect_fault_lock();
    TEST_ASSERT_FALSE(display_controller_fault_active());
}

void test_event_in_debug_returns_to_main(void)
{
    display_controller_force_view(DISPLAY_VIEW_DEBUG);
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());
}

/* ---------- Render path ----------
 * display_controller_render() is a no-op until ssd1306 is up, then
 * for each effective view it builds the ctx and calls display_render.
 * These tests exercise that the right ctx path runs for each view —
 * the actual ctx contents are covered visually on hardware; here we
 * just want the dispatcher branches executed.
 */

/* A real u8g2_t is heavyweight; tests need a non-NULL pointer for the
 * "ssd1306 is up" branch. The mocks never dereference it. */
static u8g2_t s_fake_u8g2;

void test_render_no_op_when_ssd1306_not_initialised(void)
{
    /* ssd1306_u8g2() returns NULL until init succeeds — render must
     * early-return without touching monitor_state or display_render. */
    ssd1306_u8g2_ExpectAndReturn(NULL);
    display_controller_render();
}

void test_render_main_view(void)
{
    /* Fresh init → view = MAIN; render walks the MAIN ctx-build path. */
    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_label_for_state_ExpectAnyArgsAndReturn("Charging");
    display_render_ExpectAnyArgs();
    display_controller_render();
}

void test_render_alt_thermal_view(void)
{
    tick_with_event(0U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_THERMAL, display_controller_view());

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

void test_render_alt_cooling_view(void)
{
    tick_with_event(0U,   DISPLAY_EVENT_NEXT_SHORT);
    tick_with_event(100U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_COOLING, display_controller_view());

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

void test_render_alt_battery_view(void)
{
    tick_with_event(0U,   DISPLAY_EVENT_NEXT_SHORT);
    tick_with_event(100U, DISPLAY_EVENT_NEXT_SHORT);
    tick_with_event(200U, DISPLAY_EVENT_NEXT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_ALT_BATTERY, display_controller_view());

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

void test_render_menu_view(void)
{
    tick_with_event(0U, DISPLAY_EVENT_SELECT_SHORT);
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MENU, display_controller_view());

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

/* IDLE / DEBUG render dispatch — like the handle_event arms above,
 * these are reachable only via the test seam today. */
void test_render_idle_view(void)
{
    display_controller_force_view(DISPLAY_VIEW_IDLE);

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

void test_render_debug_view(void)
{
    display_controller_force_view(DISPLAY_VIEW_DEBUG);

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

/* Fault override path: navigated view stays e.g. MAIN, but render
 * picks FAULT as the effective view and builds the fault ctx. */
void test_render_fault_override(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", "32 A > 30 A");

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

/* set_fault with a NULL detail string makes the fault ctx detail
 * field NULL — exercises the conditional in the FAULT case. */
void test_render_fault_override_null_detail(void)
{
    expect_fault_lock();
    display_controller_set_fault("FAULT", NULL);

    ssd1306_u8g2_ExpectAndReturn(&s_fake_u8g2);
    monitor_state_get_ExpectAnyArgs();
    expect_fault_lock();
    display_render_ExpectAnyArgs();
    display_controller_render();
}

/* ---------- display_controller_start ----------
 * First call: init + input_init + spawn the task. Subsequent calls
 * must early-return — s_task_handle is a file-scope static, so once
 * spawn succeeds the guard prevents a leaked second task. Both
 * behaviours are tested in one case because s_task_handle persists
 * across tests in the binary (it's the same static), so splitting
 * would mean either: a stale handle leaks the contract between
 * tests, or we need a test-only reset hook. Combining keeps the
 * production code clean. */
void test_start_first_call_spawns_task_subsequent_calls_are_noop(void)
{
    display_input_init_Expect();
    osThreadNew_ExpectAnyArgsAndReturn((osThreadId_t)0x1234);
    display_controller_start();

    /* _start also calls _init internally — view should be MAIN. */
    TEST_ASSERT_EQUAL_INT(DISPLAY_VIEW_MAIN, display_controller_view());

    /* Second + third calls: no new expectations. If start fails to
     * short-circuit it'll call display_input_init / osThreadNew and
     * CMock will flag the unexpected call as a test failure. */
    display_controller_start();
    display_controller_start();
}

/* ---------- Fault mutex serialisation ----------
 * Producers can call set_fault from a separate task (charge controller
 * / safety monitor) while the display task is mid-render. Without
 * serialisation the reader could observe a torn fault_code buffer
 * (strncpy isn't atomic). All fault-touching tests above register
 * acquire/release pairs via expect_fault_lock(); this one pins the
 * contract for the lock-acquire-failure path specifically.
 */
void test_set_fault_drops_when_lock_times_out(void)
{
    /* Acquire fails → set_fault returns early without writing the
     * buffers or matching release. fault_active stays false. */
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, FAULT_LOCK_MS, osErrorTimeout);
    display_controller_set_fault("OVERCURRENT", NULL);

    expect_fault_lock();
    TEST_ASSERT_FALSE(display_controller_fault_active());
}

void test_clear_fault_drops_when_lock_times_out(void)
{
    /* Pre-set a fault successfully (acquire+release pair). */
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);

    /* clear_fault's acquire fails → fault stays active. */
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, FAULT_LOCK_MS, osErrorTimeout);
    display_controller_clear_fault();

    expect_fault_lock();
    TEST_ASSERT_TRUE(display_controller_fault_active());
}

void test_fault_active_returns_false_when_lock_times_out(void)
{
    expect_fault_lock();
    display_controller_set_fault("OVERCURRENT", NULL);

    /* fault_active's acquire fails → defaults to false. */
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, FAULT_LOCK_MS, osErrorTimeout);
    TEST_ASSERT_FALSE(display_controller_fault_active());
}
