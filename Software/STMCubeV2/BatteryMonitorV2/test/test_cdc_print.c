/**
 * test_cdc_print.c — host-side unit tests for cdc_print.c.
 *
 * Three concerns:
 *   1. cdc_write's TinyUSB FIFO loop - drops on disconnect / null / zero
 *      length, single-shot when the FIFO accepts everything, and a
 *      yield-and-retry loop when the FIFO partially absorbs the write.
 *   2. cdc_printf's mutex-protected format-and-write path, including
 *      truncation when the format string overflows the 256 B shared buffer.
 *   3. _write() libc retarget hook - early-return guards for bad input,
 *      pre-init silent-drop, and the post-init lock-and-write path.
 *
 * Ordering note: cdc_print.c keeps an `s_mutex` static. The "pre-init"
 * tests (where s_mutex == NULL) must run before any test that calls
 * cdc_print_init(). Ceedling preserves source order, so just keeping
 * those tests at the top of the file is enough.
 */

#include "unity.h"
#include "cdc_print.h"

#include "mock_cmsis_os2.h"
#include "mock_tusb.h"

#include <string.h>

/* The libc retarget hook isn't in cdc_print.h; declare locally so the test
 * can invoke it. Picolibc's contract is (int fd, char *ptr, int len). */
int _write(int file, char *ptr, int len);

/* Sentinel mutex pointer - CMock matches by value, so any non-NULL works. */
static int s_mutex_storage;
#define FAKE_MUTEX ((osMutexId_t)&s_mutex_storage)

#define CDC_PRINT_LOCK_MS 500U

/* Capture state for the tud_cdc_write stub. Each test seeds the per-call
 * return values, then asserts on the captured request lengths to verify
 * the partial-write loop / truncation cap. */
#define CDC_WRITE_LOG_DEPTH 8
static struct cdc_write_log_entry {
    const void *buf;
    uint32_t bufsize;
} s_cdc_write_log[CDC_WRITE_LOG_DEPTH];
static uint32_t s_cdc_write_returns[CDC_WRITE_LOG_DEPTH];
static int s_cdc_write_calls;

static uint32_t tud_cdc_write_stub(const void *buf, uint32_t bufsize,
                                   int call_count)
{
    TEST_ASSERT_TRUE_MESSAGE(call_count < CDC_WRITE_LOG_DEPTH,
        "cdc_write log overflow; bump CDC_WRITE_LOG_DEPTH");
    s_cdc_write_log[call_count].buf = buf;
    s_cdc_write_log[call_count].bufsize = bufsize;
    s_cdc_write_calls = call_count + 1;
    return s_cdc_write_returns[call_count];
}

void setUp(void)
{
    memset(s_cdc_write_log, 0, sizeof(s_cdc_write_log));
    memset(s_cdc_write_returns, 0, sizeof(s_cdc_write_returns));
    s_cdc_write_calls = 0;
}
void tearDown(void) {}

/* ----- Pre-init tests -----
 *
 * cdc_write doesn't touch s_mutex, so it's safe to test before init.
 * cdc_printf / _write also have pre-init branches that early-exit when
 * s_mutex is still NULL - those must be exercised before ensure_init().
 */

void test_cdc_write_does_nothing_when_disconnected(void)
{
    tud_cdc_connected_ExpectAndReturn(false);
    cdc_write("ignored", 7U);
    /* No tud_cdc_write / flush / osDelay expected. */
}

void test_cdc_write_drops_zero_length(void)
{
    /* The guard is `!connected || len==0 || buf==NULL`; C short-circuits
     * left-to-right so tud_cdc_connected is consulted first. */
    tud_cdc_connected_ExpectAndReturn(true);
    cdc_write("ignored", 0U);
}

void test_cdc_write_drops_null_buffer(void)
{
    tud_cdc_connected_ExpectAndReturn(true);
    cdc_write(NULL, 8U);
}

void test_cdc_write_emits_full_payload_in_one_call(void)
{
    /* Happy path: FIFO accepts all 5 bytes immediately, loop exits with
     * no osDelay. */
    tud_cdc_connected_ExpectAndReturn(true);
    s_cdc_write_returns[0] = 5U;
    tud_cdc_write_Stub(tud_cdc_write_stub);
    tud_cdc_write_flush_ExpectAndReturn(0U);

    cdc_write("hello", 5U);
    TEST_ASSERT_EQUAL_INT(1, s_cdc_write_calls);
    TEST_ASSERT_EQUAL_UINT32(5U, s_cdc_write_log[0].bufsize);
}

void test_cdc_write_loops_with_delay_when_fifo_partial(void)
{
    /* FIFO accepts 3 bytes on the first call, then the remaining 2. The
     * driver must call tud_cdc_write_flush after each chunk and osDelay(1)
     * between iterations when bytes remain. */
    tud_cdc_connected_ExpectAndReturn(true);
    s_cdc_write_returns[0] = 3U;
    s_cdc_write_returns[1] = 2U;
    tud_cdc_write_Stub(tud_cdc_write_stub);
    tud_cdc_write_flush_ExpectAndReturn(0U);
    osDelay_Expect(1U);
    tud_cdc_write_flush_ExpectAndReturn(0U);

    cdc_write("hello", 5U);
    TEST_ASSERT_EQUAL_INT(2, s_cdc_write_calls);
    /* First call asks for the full 5 bytes; second asks only for what's
     * left after the partial accept. */
    TEST_ASSERT_EQUAL_UINT32(5U, s_cdc_write_log[0].bufsize);
    TEST_ASSERT_EQUAL_UINT32(2U, s_cdc_write_log[1].bufsize);
}

void test_cdc_printf_returns_minus_one_when_not_initialised(void)
{
    /* s_mutex still NULL: short-circuit before osMutexAcquire so the
     * `mock_cmsis_os2` mocks aren't consulted. vsnprintf is also not
     * called - no shared-buffer touch. */
    int rc = cdc_printf("anything %d", 42);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test__write_returns_zero_for_null_ptr(void)
{
    /* Bad-input guard fires regardless of init state. */
    TEST_ASSERT_EQUAL_INT(0, _write(1, NULL, 8));
}

void test__write_returns_zero_for_zero_len(void)
{
    char buf[] = "x";
    TEST_ASSERT_EQUAL_INT(0, _write(1, buf, 0));
}

void test__write_returns_zero_for_negative_len(void)
{
    char buf[] = "x";
    TEST_ASSERT_EQUAL_INT(0, _write(1, buf, -1));
}

void test__write_returns_len_when_pre_init_silently_drops(void)
{
    /* ptr valid, len > 0, but s_mutex == NULL: silent drop, report
     * success so stdio doesn't crash the boot. */
    char buf[] = "abc";
    TEST_ASSERT_EQUAL_INT(3, _write(1, buf, 3));
}

/* ----- Init tests ----- */

static bool s_did_init;

static void ensure_init(void)
{
    if (s_did_init) {
        return;
    }
    osMutexNew_ExpectAnyArgsAndReturn(FAKE_MUTEX);
    cdc_print_init();
    s_did_init = true;
}

void test_cdc_print_init_creates_mutex_on_first_call(void)
{
    /* First call must allocate the mutex via osMutexNew. */
    ensure_init();
}

void test_cdc_print_init_is_idempotent(void)
{
    /* Subsequent calls early-out via `if (s_mutex == NULL)`; no osMutexNew
     * expected. If the guard regressed, CMock would flag the unexpected
     * call. */
    cdc_print_init();
}

/* ----- Post-init tests ----- */

void test_cdc_printf_locks_formats_writes_unlocks(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS, osOK);
    /* "tick 7\n" → 7 bytes. */
    tud_cdc_connected_ExpectAndReturn(true);
    s_cdc_write_returns[0] = 7U;
    tud_cdc_write_Stub(tud_cdc_write_stub);
    tud_cdc_write_flush_ExpectAndReturn(0U);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    int rc = cdc_printf("tick %d\n", 7);
    TEST_ASSERT_EQUAL_INT(7, rc);
    TEST_ASSERT_EQUAL_INT(1, s_cdc_write_calls);
    TEST_ASSERT_EQUAL_UINT32(7U, s_cdc_write_log[0].bufsize);
}

void test_cdc_printf_returns_minus_one_when_mutex_acquire_fails(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS,
                                   osErrorTimeout);
    /* No format / write / release on this path - the mutex check is the
     * very first thing cdc_vprintf does. */

    int rc = cdc_printf("never formatted: %d", 1);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_cdc_printf_rejects_null_format_without_touching_mutex(void)
{
    /* Defensive guard: vsnprintf has no NULL check, so cdc_printf(NULL)
     * would crash on target. The NULL-fmt branch fires BEFORE the mutex
     * acquire, so this post-init test registers ZERO osMutexAcquire /
     * osMutexRelease expectations - any leak through the guard into the
     * lock path would surface as CMock's "unexpected call" failure.
     * Indirected via a runtime pointer so the
     * `__attribute__((format(printf,1,2)))` doesn't compile-warn. */
    ensure_init();
    const char *bad = NULL;
    int rc = cdc_printf(bad);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_cdc_printf_skips_cdc_write_when_format_produces_zero_bytes(void)
{
    /* vsnprintf("%s", "") returns 0 - the `if (n > 0)` guard then skips
     * cdc_write entirely (no point flushing a zero-byte payload through
     * USB), but we still lock + release the mutex so callers that race
     * an empty line don't get a phantom unlock failure later. */
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS, osOK);
    /* No tud_cdc_* / osDelay expected - cdc_write must NOT be reached. */
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    int rc = cdc_printf("%s", "");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void test_cdc_printf_caps_write_at_buffer_size_minus_one(void)
{
    /* Force vsnprintf to want 300 bytes - the shared buffer is 256, so
     * the driver caps the cdc_write call at 255 (sizeof(s_buf) - 1).
     * The function still returns 300 (vsnprintf's "would-have-been"
     * length) so callers know they were truncated. */
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS, osOK);
    tud_cdc_connected_ExpectAndReturn(true);
    s_cdc_write_returns[0] = 255U;
    tud_cdc_write_Stub(tud_cdc_write_stub);
    tud_cdc_write_flush_ExpectAndReturn(0U);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    /* "%-300d" produces 300 chars: "1" left-padded to width 300. */
    int rc = cdc_printf("%-300d", 1);
    TEST_ASSERT_EQUAL_INT(300, rc);
    TEST_ASSERT_EQUAL_INT(1, s_cdc_write_calls);
    TEST_ASSERT_EQUAL_UINT32(255U, s_cdc_write_log[0].bufsize);
}

void test__write_routes_payload_through_cdc_write(void)
{
    ensure_init();
    /* Happy path: lock, write via cdc_write (which mocks all three TinyUSB
     * calls), unlock, return len. */
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS, osOK);
    tud_cdc_connected_ExpectAndReturn(true);
    s_cdc_write_returns[0] = 6U;
    tud_cdc_write_Stub(tud_cdc_write_stub);
    tud_cdc_write_flush_ExpectAndReturn(0U);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    char payload[] = "hello!";
    int rc = _write(1, payload, 6);
    TEST_ASSERT_EQUAL_INT(6, rc);
    TEST_ASSERT_EQUAL_INT(1, s_cdc_write_calls);
    TEST_ASSERT_EQUAL_UINT32(6U, s_cdc_write_log[0].bufsize);
}

void test__write_returns_zero_when_mutex_acquire_fails(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, CDC_PRINT_LOCK_MS,
                                   osErrorTimeout);
    /* No release on this path - we never held the mutex. */

    char payload[] = "abc";
    int rc = _write(1, payload, 3);
    TEST_ASSERT_EQUAL_INT(0, rc);
}
