/**
 * test_i2c_bus.c — host-side unit tests for i2c_bus.c.
 *
 * Verifies the lock/unlock/wait wrappers call the right CMSIS-RTOS2 primitives,
 * and that the overridden HAL callbacks release the completion semaphore.
 *
 * Note on test isolation: i2c_bus.c has a `s_initialised` static. Each test
 * calls i2c_bus_init() first so the mocks express the expected init flow;
 * subsequent calls early-out (we don't re-mock New() inside one test).
 */

#include "unity.h"
#include "i2c_bus.h"

#include "mock_cmsis_os2.h"
#include "mock_i2c.h"

#include <string.h>

/* Sentinel pointers — CMock matches by value, so any non-NULL stand-in works.
 * Distinct values keep failures legible when the wrong handle is passed. */
static int s_mutex_storage;
static int s_sem_storage;
#define FAKE_MUTEX ((osMutexId_t)&s_mutex_storage)
#define FAKE_SEM   ((osSemaphoreId_t)&s_sem_storage)

/* --- i2c_bus_scan helper state ---
 *
 * Scan walks 0x08..0x77, so each test would otherwise need ~112 mock
 * expectations. We use CMock's :callback plugin via _Stub() to point every
 * HAL_I2C_IsDeviceReady invocation at a single helper; the helper consults
 * a per-test ack-set populated via seed_scan_acks(). _Stub() replaces the
 * mock entirely - no per-call Expect needed, no strict-ordering tracking
 * for this function.
 */
static uint8_t s_scan_ack_addrs[8];
static size_t  s_scan_ack_count;

static bool scan_addr_acks(uint8_t addr)
{
    for (size_t i = 0; i < s_scan_ack_count; i++) {
        if (s_scan_ack_addrs[i] == addr) {
            return true;
        }
    }
    return false;
}

static HAL_StatusTypeDef scan_is_ready_stub(I2C_HandleTypeDef *hi2c,
                                            uint16_t dev_addr_shifted,
                                            uint32_t trials,
                                            uint32_t timeout,
                                            int call_count)
{
    (void)hi2c; (void)trials; (void)timeout; (void)call_count;
    /* HAL takes the 7-bit address pre-shifted into bits [7:1]. */
    uint8_t addr_7bit = (uint8_t)(dev_addr_shifted >> 1);
    return scan_addr_acks(addr_7bit) ? HAL_OK : HAL_ERROR;
}

static void seed_scan_acks(const uint8_t *addrs, size_t n)
{
    TEST_ASSERT_TRUE_MESSAGE(n <= sizeof(s_scan_ack_addrs),
        "ack set too large; bump s_scan_ack_addrs");
    if (addrs != NULL) {
        memcpy(s_scan_ack_addrs, addrs, n);
    }
    s_scan_ack_count = n;
}

void setUp(void)
{
    /* i2c_bus.c keeps a `s_initialised` static across tests. We can't reset
     * it from here, so we mock init in EVERY test as if it's the first call;
     * after the first test in the binary, the real code early-exits and
     * those Expects never fire. To keep this robust we wrap init in a helper
     * that gates on a per-binary flag. */
    s_scan_ack_count = 0;
}
void tearDown(void) {}

static bool s_did_init;

static void ensure_init(void)
{
    if (s_did_init) {
        return;
    }
    osMutexNew_ExpectAnyArgsAndReturn(FAKE_MUTEX);
    osSemaphoreNew_ExpectAnyArgsAndReturn(FAKE_SEM);
    i2c_bus_init();
    s_did_init = true;
}

void test_lock_calls_osMutexAcquire_with_timeout(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 100U, osOK);
    TEST_ASSERT_TRUE(i2c_bus_lock(100U));
}

void test_lock_returns_false_on_timeout(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 50U, osErrorTimeout);
    TEST_ASSERT_FALSE(i2c_bus_lock(50U));
}

void test_unlock_calls_osMutexRelease(void)
{
    ensure_init();
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);
    i2c_bus_unlock();
}

void test_wait_complete_acquires_semaphore(void)
{
    ensure_init();
    osSemaphoreAcquire_ExpectAndReturn(FAKE_SEM, 100U, osOK);
    TEST_ASSERT_TRUE(i2c_bus_wait_complete(100U));
}

void test_wait_complete_returns_false_on_timeout(void)
{
    ensure_init();
    osSemaphoreAcquire_ExpectAndReturn(FAKE_SEM, 100U, osErrorTimeout);
    TEST_ASSERT_FALSE(i2c_bus_wait_complete(100U));
}

void test_mem_tx_cplt_callback_releases_semaphore(void)
{
    ensure_init();
    osSemaphoreRelease_ExpectAndReturn(FAKE_SEM, osOK);
    HAL_I2C_MemTxCpltCallback(&hi2c1);
}

void test_mem_rx_cplt_callback_releases_semaphore(void)
{
    ensure_init();
    osSemaphoreRelease_ExpectAndReturn(FAKE_SEM, osOK);
    HAL_I2C_MemRxCpltCallback(&hi2c1);
}

void test_master_tx_cplt_callback_releases_semaphore(void)
{
    /* The bus hooks all four DMA completion callbacks - Mem and Master
     * variants - because device drivers may use either entry point. */
    ensure_init();
    osSemaphoreRelease_ExpectAndReturn(FAKE_SEM, osOK);
    HAL_I2C_MasterTxCpltCallback(&hi2c1);
}

void test_master_rx_cplt_callback_releases_semaphore(void)
{
    ensure_init();
    osSemaphoreRelease_ExpectAndReturn(FAKE_SEM, osOK);
    HAL_I2C_MasterRxCpltCallback(&hi2c1);
}

void test_error_callback_still_releases_semaphore(void)
{
    /* The wait-side surfaces the failure via HAL_I2C_GetError(); the bus
     * just needs to wake the waiting task. */
    ensure_init();
    osSemaphoreRelease_ExpectAndReturn(FAKE_SEM, osOK);
    HAL_I2C_ErrorCallback(&hi2c1);
}

/* --- i2c_bus_scan ---
 *
 * Scan API contract:
 *   - Returns the count of ACKing 7-bit addresses found.
 *   - Always writes a "scan:" prefix when out_size > 0.
 *   - On lock failure, writes "scan: bus busy" and returns 0 (no scan).
 *   - On no devices, appends " (none)" to the prefix.
 *   - Otherwise appends " 0xXX" per ACKing address, in ascending order
 *     (the loop walks 0x08..0x77).
 */

void test_scan_returns_zero_for_null_buffer(void)
{
    /* No init / lock interaction expected - the guard fires before either. */
    TEST_ASSERT_EQUAL_INT(0, i2c_bus_scan(&hi2c1, NULL, 16U));
}

void test_scan_returns_zero_for_zero_buffer_size(void)
{
    char buf[1] = { 'x' };
    TEST_ASSERT_EQUAL_INT(0, i2c_bus_scan(&hi2c1, buf, 0U));
    /* Buffer must be untouched on this path. */
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

void test_scan_reports_bus_busy_when_lock_times_out(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osErrorTimeout);

    char buf[32] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, found);
    TEST_ASSERT_EQUAL_STRING("scan: bus busy", buf);
}

void test_scan_reports_none_when_no_device_acks(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osOK);
    HAL_I2C_IsDeviceReady_Stub(scan_is_ready_stub);
    seed_scan_acks(NULL, 0);
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    char buf[64] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, found);
    TEST_ASSERT_EQUAL_STRING("scan: (none)", buf);
}

void test_scan_lists_a_single_device(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osOK);
    HAL_I2C_IsDeviceReady_Stub(scan_is_ready_stub);
    /* INA238 address on this board. */
    const uint8_t acks[] = { 0x40U };
    seed_scan_acks(acks, sizeof(acks));
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    char buf[64] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(1, found);
    TEST_ASSERT_EQUAL_STRING("scan: 0x40", buf);
}

void test_scan_lists_multiple_devices_in_ascending_order(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osOK);
    HAL_I2C_IsDeviceReady_Stub(scan_is_ready_stub);
    /* Seed order doesn't matter - scan walks 0x08..0x77 so output is
     * sorted regardless. PN532 (0x24), INA238 (0x40), arbitrary (0x50). */
    const uint8_t acks[] = { 0x50U, 0x24U, 0x40U };
    seed_scan_acks(acks, sizeof(acks));
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    char buf[64] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(3, found);
    TEST_ASSERT_EQUAL_STRING("scan: 0x24 0x40 0x50", buf);
}

void test_scan_skips_reserved_addresses_below_0x08_and_above_0x77(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osOK);
    HAL_I2C_IsDeviceReady_Stub(scan_is_ready_stub);
    /* These addresses are outside the 0x08..0x77 sweep range. Even if a
     * device sat there, scan must not probe it (I2C spec reserves them). */
    const uint8_t acks[] = { 0x00U, 0x07U, 0x78U, 0x7FU };
    seed_scan_acks(acks, sizeof(acks));
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    char buf[64] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, found);
    TEST_ASSERT_EQUAL_STRING("scan: (none)", buf);
}

void test_scan_truncates_gracefully_when_buffer_too_small(void)
{
    ensure_init();
    osMutexAcquire_ExpectAndReturn(FAKE_MUTEX, 2000U, osOK);
    HAL_I2C_IsDeviceReady_Stub(scan_is_ready_stub);
    const uint8_t acks[] = { 0x24U, 0x40U, 0x50U };
    seed_scan_acks(acks, sizeof(acks));
    osMutexRelease_ExpectAndReturn(FAKE_MUTEX, osOK);

    /* Just enough room for "scan: 0x24" + NUL. Further addresses still
     * count toward `found` (the caller's source of truth) but the output
     * string is truncated rather than overflowed. */
    char buf[11] = { 0 };
    int found = i2c_bus_scan(&hi2c1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(3, found);
    /* snprintf NUL-terminates; the trailing addresses get dropped. */
    TEST_ASSERT_EQUAL_STRING("scan: 0x24", buf);
}
