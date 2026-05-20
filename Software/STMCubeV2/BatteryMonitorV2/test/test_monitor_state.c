/**
 * test_monitor_state.c — host-side unit tests for monitor_state.c.
 *
 * Three concerns:
 *   1. Pre-init guards (s_mutex == NULL) — update is a no-op,
 *      get zeros the out param. These tests must run BEFORE any
 *      test that calls monitor_state_init() because the s_mutex
 *      static persists across tests inside one source file.
 *   2. init creates the mutex exactly once (idempotent).
 *   3. update / get round-trip preserves all fields, with NULL
 *      arg guards on both sides and proper handling of mutex
 *      acquire failure.
 */

#include "unity.h"
#include "monitor_state.h"

#include "mock_cmsis_os2.h"

#include <string.h>

/* Sentinel mutex pointer — CMock matches by value, so any non-NULL works. */
static int s_mutex_storage;
#define FAKE_MUTEX ((osMutexId_t)&s_mutex_storage)

#define MONITOR_STATE_LOCK_MS 100U

void setUp(void)   { }
void tearDown(void) { }

/* ---------- Pre-init guards (must run first) ---------- */

void test_get_before_init_zeros_out(void)
{
    monitor_snapshot_t out = {
        .vbus_mv = 0xDEADBEEFU,
        .current_ma = -1,
        .k1_on = true,
    };
    monitor_state_get(&out);
    monitor_snapshot_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &out, sizeof(out));
}

void test_update_before_init_is_noop(void)
{
    /* No mock expectations — update must early-out before touching
     * the mutex API. If it doesn't, CMock fails the test. */
    monitor_snapshot_t s = { .vbus_mv = 12345U };
    monitor_state_update(&s);
}

void test_get_null_out_is_noop(void)
{
    /* Same: must short-circuit before any mock call. */
    monitor_state_get(NULL);
}

void test_update_null_in_is_noop(void)
{
    monitor_state_update(NULL);
}

/* ---------- Init creates the mutex ---------- */

void test_init_creates_mutex_once(void)
{
    osMutexNew_ExpectAnyArgsAndReturn(FAKE_MUTEX);
    monitor_state_init();

    /* Second call: must early-out via the `if (s_mutex == NULL)` guard.
     * Zero further mock expectations registered, so any leak fails. */
    monitor_state_init();
}

/* ---------- Update / get round-trip ---------- */

void test_update_then_get_round_trips(void)
{
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    const monitor_snapshot_t in = {
        .vbus_mv      = 12345U,
        .current_ma   = -678,
        .vshunt_uv    = 124,
        .tdie_mc      = 23500,
        .k1_on        = true,
        .bleed_on     = false,
        .fan_duty_pct = 50U,
        .timestamp_ms = 1000U,
    };
    monitor_state_update(&in);

    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    monitor_snapshot_t out;
    monitor_state_get(&out);

    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
}

void test_second_update_overwrites_first(void)
{
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);
    monitor_snapshot_t a = { .vbus_mv = 100U };
    monitor_state_update(&a);

    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);
    monitor_snapshot_t b = { .vbus_mv = 999U, .k1_on = true };
    monitor_state_update(&b);

    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS, osOK);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);
    monitor_snapshot_t out;
    monitor_state_get(&out);

    TEST_ASSERT_EQUAL_UINT32(999U, out.vbus_mv);
    TEST_ASSERT_TRUE(out.k1_on);
}

/* ---------- Mutex-acquire failure paths ---------- */

void test_update_drops_when_lock_times_out(void)
{
    /* Acquire fails → update is silently dropped, no release call. */
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS,
                                   osErrorTimeout);
    monitor_snapshot_t s = { .vbus_mv = 42U };
    monitor_state_update(&s);
}

void test_get_zeros_out_when_lock_times_out(void)
{
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, MONITOR_STATE_LOCK_MS,
                                   osErrorTimeout);
    monitor_snapshot_t out = { .vbus_mv = 0xDEADU };
    monitor_state_get(&out);
    monitor_snapshot_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &out, sizeof(out));
}
