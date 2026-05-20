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

/* Sentinel pointers — CMock matches by value, so any non-NULL stand-in works.
 * Distinct values keep failures legible when the wrong handle is passed. */
static int s_mutex_storage;
static int s_sem_storage;
#define FAKE_MUTEX ((osMutexId_t)&s_mutex_storage)
#define FAKE_SEM   ((osSemaphoreId_t)&s_sem_storage)

void setUp(void)
{
    /* i2c_bus.c keeps a `s_initialised` static across tests. We can't reset
     * it from here, so we mock init in EVERY test as if it's the first call;
     * after the first test in the binary, the real code early-exits and
     * those Expects never fire. To keep this robust we wrap init in a helper
     * that gates on a per-binary flag. */
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

/* ----- Pre-init guards -----
 *
 * i2c_bus.c keeps an `s_initialised` static that gates every public op
 * plus the five HAL completion callbacks. These tests run before any
 * ensure_init() call (Ceedling preserves source order, so they're
 * placed first), with no mock expectations set - any RTOS call that
 * leaked through the !s_initialised guard would fail with CMock's
 * "unexpected call" error. The lock/wait variants return false; the
 * void-returning ones simply do nothing.
 */

void test_lock_returns_false_before_init(void)
{
    TEST_ASSERT_FALSE(i2c_bus_lock(100U));
}

void test_unlock_is_noop_before_init(void)
{
    i2c_bus_unlock();
}

void test_wait_complete_returns_false_before_init(void)
{
    TEST_ASSERT_FALSE(i2c_bus_wait_complete(100U));
}

void test_mem_tx_callback_is_noop_before_init(void)
{
    HAL_I2C_MemTxCpltCallback(&hi2c1);
}

void test_mem_rx_callback_is_noop_before_init(void)
{
    HAL_I2C_MemRxCpltCallback(&hi2c1);
}

void test_master_tx_callback_is_noop_before_init(void)
{
    HAL_I2C_MasterTxCpltCallback(&hi2c1);
}

void test_master_rx_callback_is_noop_before_init(void)
{
    HAL_I2C_MasterRxCpltCallback(&hi2c1);
}

void test_error_callback_is_noop_before_init(void)
{
    HAL_I2C_ErrorCallback(&hi2c1);
}

void test_init_is_idempotent_after_first_success(void)
{
    /* The very first ensure_init() registered exactly one osMutexNew +
     * osSemaphoreNew expectation and called i2c_bus_init() once. This
     * test calls it again with ZERO new expectations - the
     * `if (s_initialised) return;` guard must fire before either
     * RTOS-create function is consulted, otherwise CMock surfaces an
     * "unexpected call" failure. */
    ensure_init();
    i2c_bus_init();
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

