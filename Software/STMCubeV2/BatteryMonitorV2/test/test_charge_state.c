/**
 * test_charge_state.c — host-side tests for charge_state.c.
 *
 * Covers the label tables, init lifecycle, and the transitions that
 * are wired in the skeleton: INIT->IDLE on the first tick, IDLE
 * battery debounce → CHARGE_PREPARE, CHARGE_PREPARE → CHARGING, the
 * MONITOR enter/exit pair, and the SAFETY_FAULT preemption →
 * cmd_reset_fault exit. Per-state telemetry-driven transitions
 * (charge timeout, OC fault, tail-current termination, NTC overtemp)
 * land with the .c implementations and get their own tests there.
 *
 * Mocks are set wholesale in setUp: cmsis_os2 + direct_io + the
 * monitor_state read pump all default to Ignore so the tick-level
 * tests below only set explicit Expects on monitor_state_get (one
 * per tick) via the expect_telemetry() helper.
 */

#include "unity.h"
#include "charge_state.h"

#include "mock_cmsis_os2.h"
#include "mock_direct_io.h"
#include "mock_monitor_state.h"

#include <string.h>

static int s_mutex_storage;
#define FAKE_MUTEX  ((osMutexId_t)&s_mutex_storage)

static int s_queue_storage;
#define FAKE_QUEUE  ((osMessageQueueId_t)&s_queue_storage)

/* Scratch snapshot returned via monitor_state_get_ReturnThruPtr. Lives
 * at file scope so the pointer stays valid across the CMock call —
 * giving it auto storage would dangle by the time the mock fires. */
static monitor_snapshot_t s_telemetry;

static void expect_telemetry(uint32_t vbus_mv, int32_t current_ma)
{
    s_telemetry.vbus_mv    = vbus_mv;
    s_telemetry.current_ma = current_ma;
    s_telemetry.tdie_mc    = 0;
    monitor_state_get_ExpectAnyArgs();
    monitor_state_get_ReturnThruPtr_out(&s_telemetry);
}

void setUp(void)
{
    /* charge_state_init's osMutexNew / osMessageQueueNew are only
     * called the very first time (the .c keeps the handles in file
     * statics and short-circuits on subsequent inits). Ignore covers
     * both: the first invocation returning a fake handle, and the
     * no-op subsequent ones. */
    osMutexNew_IgnoreAndReturn(FAKE_MUTEX);
    osMessageQueueNew_IgnoreAndReturn(FAKE_QUEUE);
    osMutexAcquire_IgnoreAndReturn(osOK);
    osMutexRelease_IgnoreAndReturn(osOK);
    osMessageQueuePut_IgnoreAndReturn(osOK);
    osDelay_Ignore();

    bleed_disable_Ignore();
    bleed_enable_Ignore();
    relay_enable_Ignore();
    relay_disable_Ignore();
    fan_set_duty_Ignore();

    charge_state_init();
}

void tearDown(void) { }

/* ---------- label tables ---------- */

void test_charge_state_label_covers_all_states(void)
{
    TEST_ASSERT_EQUAL_STRING("init",      charge_state_label(CHARGE_STATE_INIT));
    TEST_ASSERT_EQUAL_STRING("idle",      charge_state_label(CHARGE_STATE_IDLE));
    TEST_ASSERT_EQUAL_STRING("prep",      charge_state_label(CHARGE_STATE_CHARGE_PREPARE));
    TEST_ASSERT_EQUAL_STRING("charge",    charge_state_label(CHARGE_STATE_CHARGING));
    TEST_ASSERT_EQUAL_STRING("test prep", charge_state_label(CHARGE_STATE_TEST_PREP));
    TEST_ASSERT_EQUAL_STRING("test",      charge_state_label(CHARGE_STATE_TEST));
    TEST_ASSERT_EQUAL_STRING("trickle",   charge_state_label(CHARGE_STATE_TRICKLE));
    TEST_ASSERT_EQUAL_STRING("monitor",   charge_state_label(CHARGE_STATE_MONITOR));
    TEST_ASSERT_EQUAL_STRING("fault",     charge_state_label(CHARGE_STATE_FAULT));
}

void test_charge_state_label_out_of_range_returns_question(void)
{
    TEST_ASSERT_EQUAL_STRING("?", charge_state_label((charge_state_t)999));
    TEST_ASSERT_EQUAL_STRING("?", charge_state_label(CHARGE_STATE_COUNT));
}

void test_fault_reason_label_covers_all(void)
{
    TEST_ASSERT_EQUAL_STRING("none",         fault_reason_label(FAULT_NONE));
    TEST_ASSERT_EQUAL_STRING("chrg timeout", fault_reason_label(FAULT_CHRG_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("overcurrent",  fault_reason_label(FAULT_OVERCURRENT));
    TEST_ASSERT_EQUAL_STRING("overtemp",     fault_reason_label(FAULT_OVERTEMP));
    TEST_ASSERT_EQUAL_STRING("batt reject",  fault_reason_label(FAULT_BATT_REJECTED));
    TEST_ASSERT_EQUAL_STRING("safety",       fault_reason_label(FAULT_SAFETY_OTHER));
}

void test_fault_reason_label_out_of_range_returns_question(void)
{
    TEST_ASSERT_EQUAL_STRING("?", fault_reason_label((fault_reason_t)999));
    TEST_ASSERT_EQUAL_STRING("?", fault_reason_label(FAULT_COUNT));
}

/* ---------- snapshot lifecycle ---------- */

void test_snapshot_get_with_null_is_a_no_op(void)
{
    /* No expectations — the early return short-circuits before any
     * mutex op. CMock would flag an unexpected call if that's wrong. */
    charge_state_get(NULL);
}

/* ---------- INIT -> IDLE on first tick ---------- */

void test_first_tick_transitions_init_to_idle(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_IDLE, snap.state);
    TEST_ASSERT_EQUAL_INT(FAULT_NONE, snap.fault);
    /* IDLE outputs: K1 on, bleed off, fan low (30 %). */
    TEST_ASSERT_TRUE(snap.k1_on);
    TEST_ASSERT_FALSE(snap.bleed_on);
    TEST_ASSERT_EQUAL_UINT8(30U, snap.fan_duty_pct);
    /* NTC sentinel — until the driver lands. */
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, snap.ntc_mc);
}

/* ---------- IDLE battery debounce ---------- */

void test_idle_stays_idle_without_battery(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);
    /* In IDLE; below the V_BATT_FLOOR threshold. */

    for (uint32_t i = 0; i < 10U; i++) {
        expect_telemetry(0U, 0);
        charge_state_tick(100U * (i + 1U), NULL);
    }

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_IDLE, snap.state);
    TEST_ASSERT_FALSE(snap.battery_present);
}

void test_idle_to_charge_prepare_after_battery_debounce(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);   /* INIT -> IDLE */

    /* T_PRESENT_SAMPLES consecutive ticks above V_BATT_FLOOR before
     * the debouncer flips battery_present. The same tick the flag
     * flips, handle_idle sees it and transitions to CHARGE_PREPARE. */
    for (uint32_t i = 0; i < T_PRESENT_SAMPLES; i++) {
        expect_telemetry(12000U, 0);
        charge_state_tick(100U * (i + 1U), NULL);
    }

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_CHARGE_PREPARE, snap.state);
    TEST_ASSERT_TRUE(snap.battery_present);
    /* V0 latched at the moment of the IDLE->PREPARE decision. */
    TEST_ASSERT_EQUAL_UINT32(12000U, snap.v0_mv);
}

/* ---------- CHARGE_PREPARE -> CHARGING (skeleton: immediate) ---------- */

void test_charge_prepare_advances_to_charging_next_tick(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);
    for (uint32_t i = 0; i < T_PRESENT_SAMPLES; i++) {
        expect_telemetry(12500U, 0);
        charge_state_tick(100U * (i + 1U), NULL);
    }
    /* Now in CHARGE_PREPARE — next tick falls through to CHARGING. */

    expect_telemetry(12500U, 0);
    charge_state_tick(2000U, NULL);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_CHARGING, snap.state);
    /* CHARGING outputs: K1 off (charger to battery), bleed off, fan
     * medium (60 %). */
    TEST_ASSERT_FALSE(snap.k1_on);
    TEST_ASSERT_FALSE(snap.bleed_on);
    TEST_ASSERT_EQUAL_UINT8(60U, snap.fan_duty_pct);
}

/* ---------- MONITOR enter / exit ---------- */

void test_idle_to_monitor_on_cmd_enter_monitor(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t evt = { .id = EVT_CMD_ENTER_MONITOR, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &evt);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_MONITOR, snap.state);
    /* MONITOR: K1 off (charger naturally connected), bleed off,
     * fan low. */
    TEST_ASSERT_FALSE(snap.k1_on);
    TEST_ASSERT_FALSE(snap.bleed_on);
}

void test_monitor_to_idle_on_cmd_exit_monitor(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t enter = { .id = EVT_CMD_ENTER_MONITOR, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &enter);

    charge_event_t exit = { .id = EVT_CMD_EXIT_MONITOR, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(200U, &exit);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_IDLE, snap.state);
}

/* ---------- safety fault preemption ---------- */

void test_safety_fault_event_transitions_to_fault_state(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t fault = {
        .id      = EVT_SAFETY_FAULT,
        .payload = (uint32_t)FAULT_OVERCURRENT,
    };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &fault);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_FAULT, snap.state);
    TEST_ASSERT_EQUAL_INT(FAULT_OVERCURRENT, snap.fault);
}

void test_safety_fault_out_of_range_payload_falls_back_to_safety_other(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t fault = {
        .id      = EVT_SAFETY_FAULT,
        .payload = 999U,   /* out of range */
    };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &fault);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_FAULT, snap.state);
    TEST_ASSERT_EQUAL_INT(FAULT_SAFETY_OTHER, snap.fault);
}

void test_fault_to_idle_on_cmd_reset(void)
{
    /* Drive into FAULT, then assert cmd_reset_fault clears the fault
     * and returns to IDLE. cmd_reset_fault is checked BEFORE the
     * battery_present exit in handle_fault, so this isolates the
     * reset path independent of telemetry. */
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t fault = {
        .id      = EVT_SAFETY_FAULT,
        .payload = (uint32_t)FAULT_OVERCURRENT,
    };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &fault);

    charge_event_t reset = { .id = EVT_CMD_RESET_FAULT, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(200U, &reset);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_IDLE, snap.state);
    TEST_ASSERT_EQUAL_INT(FAULT_NONE, snap.fault);
}

/* ---------- force-idle hatch ---------- */

void test_cmd_force_idle_from_monitor_returns_to_idle(void)
{
    expect_telemetry(0U, 0);
    charge_state_tick(0U, NULL);

    charge_event_t enter = { .id = EVT_CMD_ENTER_MONITOR, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(100U, &enter);

    charge_event_t force = { .id = EVT_CMD_FORCE_IDLE, .payload = 0 };
    expect_telemetry(0U, 0);
    charge_state_tick(200U, &force);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_IDLE, snap.state);
}

void test_cmd_force_idle_does_not_escape_fault(void)
{
    /* FAULT requires explicit reset_fault (or V<V_SAFE / battery
     * removed); the generic force_idle hatch must not bypass that
     * because the operator's intent for FAULT is "I acknowledge the
     * fault", not "I want to skip the safety check". To isolate
     * THAT path the test first builds battery_present (otherwise
     * the no-battery exit would clear the fault for the wrong
     * reason) and holds Vbus > V_SAFE_MV so the drain exit doesn't
     * fire either. */
    expect_telemetry(13000U, 0);
    charge_state_tick(0U, NULL);
    for (uint32_t i = 0; i < T_PRESENT_SAMPLES; i++) {
        expect_telemetry(13000U, 0);
        charge_state_tick(100U * (i + 1U), NULL);
    }

    charge_event_t fault = {
        .id      = EVT_SAFETY_FAULT,
        .payload = (uint32_t)FAULT_OVERCURRENT,
    };
    expect_telemetry(13000U, 0);
    charge_state_tick(700U, &fault);

    charge_event_t force = { .id = EVT_CMD_FORCE_IDLE, .payload = 0 };
    expect_telemetry(13000U, 0);
    charge_state_tick(800U, &force);

    charge_snapshot_t snap;
    charge_state_get(&snap);
    TEST_ASSERT_EQUAL_INT(CHARGE_STATE_FAULT, snap.state);
    TEST_ASSERT_EQUAL_INT(FAULT_OVERCURRENT, snap.fault);
}
