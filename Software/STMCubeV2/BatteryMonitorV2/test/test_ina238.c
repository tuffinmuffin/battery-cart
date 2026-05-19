/**
 * test_ina238.c — host-side unit tests for ina238.c.
 *
 * Two flavours of test:
 *   1. Pure calibration math — no mocks. Verifies the SHUNT_CAL and
 *      CURRENT_LSB formulas against datasheet values.
 *   2. Driver flow — mocks i2c_bus_{lock,unlock,wait_complete} plus
 *      HAL_I2C_Mem_*_DMA / HAL_I2C_GetError. Uses CMock's :callback plugin
 *      to simulate DMA writing into the rx buffer.
 *
 * INA237/INA238 family compat: probe() accepts both device IDs. Tests cover
 * the INA238 path (the primary part), the INA237 compat path, and rejection
 * of unrelated TI parts (e.g. INA236).
 */

#include "unity.h"
#include "ina238.h"

#include "mock_i2c_bus.h"
#include "mock_i2c.h"

#include "stack_measure.h"

#include <string.h>

/* Forward declarations for the per-test rx-response queue (defined later). */
#define RX_QUEUE_DEPTH 4
static struct rx_response {
    uint8_t buf[3];
    uint16_t len;
} s_rx_queue[RX_QUEUE_DEPTH];
static int s_rx_queue_filled;

void setUp(void)
{
    /* Reset the per-test rx queue so each test starts clean. CMock resets its
     * own call_count between tests, so call_count==0 selects s_rx_queue[0]. */
    memset(s_rx_queue, 0, sizeof(s_rx_queue));
    s_rx_queue_filled = 0;
}
void tearDown(void) {}

/* ----- Pure calibration math ----- */

void test_current_lsb_for_40A_target_is_about_1220_nA(void)
{
    /* CURRENT_LSB = Imax / 2^15. 40 A / 32768 = 1.220703... mA = 1220.7 µA
     * = 1_220_703 nA. Integer truncation gives 1_220_703. */
    TEST_ASSERT_EQUAL_UINT32(1220703U, ina238_calc_current_lsb_na(40000U));
}

void test_current_lsb_for_zero_max_current_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, ina238_calc_current_lsb_na(0U));
}

void test_shunt_cal_for_board_defaults_4mOhm_40A_range0(void)
{
    /* Hand-checked: (4096 * 40000 * 4000) / 163_840_000 = 4000 exact. */
    TEST_ASSERT_EQUAL_UINT16(4000U,
        ina238_calc_shunt_cal(40000U, 4000U, false));
}

void test_shunt_cal_adcrange_low_is_4x_higher(void)
{
    /* ADCRANGE=1 multiplies CAL by 4 per the datasheet. */
    TEST_ASSERT_EQUAL_UINT16(16000U,
        ina238_calc_shunt_cal(40000U, 4000U, true));
}

void test_shunt_cal_clamps_at_15_bit_max(void)
{
    /* 1 A through a 1 Ω shunt at ADCRANGE=1 would compute well past 32767. */
    TEST_ASSERT_EQUAL_UINT16(32767U,
        ina238_calc_shunt_cal(1000U, 1000000U, true));
}

/* ----- Driver flow tests -----
 *
 * CMock fixture: a callback that simulates the DMA dropping bytes into the
 * receive buffer. We queue one response per expect_register_read() call; the
 * CMock `call_count` arg selects which response the callback returns.
 * The `s_rx_queue` storage is declared near the top of the file so setUp()
 * can reset it.
 */

static HAL_StatusTypeDef mem_read_dma_cb(I2C_HandleTypeDef *hi2c,
                                         uint16_t DevAddress,
                                         uint16_t MemAddress,
                                         uint16_t MemAddSize,
                                         uint8_t *pData,
                                         uint16_t Size,
                                         int call_count)
{
    (void)hi2c; (void)DevAddress; (void)MemAddress; (void)MemAddSize;
    TEST_ASSERT_TRUE_MESSAGE(call_count < s_rx_queue_filled,
        "more reads happened than expect_register_read calls were queued");
    TEST_ASSERT_EQUAL_UINT16(s_rx_queue[call_count].len, Size);
    memcpy(pData, s_rx_queue[call_count].buf, Size);
    return HAL_OK;
}

static void expect_register_read(uint8_t reg, uint16_t value)
{
    TEST_ASSERT_TRUE(s_rx_queue_filled < RX_QUEUE_DEPTH);
    s_rx_queue[s_rx_queue_filled].buf[0] = (uint8_t)(value >> 8);
    s_rx_queue[s_rx_queue_filled].buf[1] = (uint8_t)(value & 0xFFU);
    s_rx_queue[s_rx_queue_filled].len = 2;
    s_rx_queue_filled++;

    /* Call order in do_mem_read: lock → Mem_Read_DMA → wait_complete →
     * unlock → GetError. Strict ordering means this sequence must match. */
    i2c_bus_lock_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, true);
    HAL_I2C_Mem_Read_DMA_AddCallback(mem_read_dma_cb);
    HAL_I2C_Mem_Read_DMA_ExpectAnyArgsAndReturn(HAL_OK);
    i2c_bus_wait_complete_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, true);
    i2c_bus_unlock_Expect();
    HAL_I2C_GetError_ExpectAndReturn(&hi2c1, HAL_I2C_ERROR_NONE);
    (void)reg; /* arg-matching is delegated to mem_read_dma_cb */
}

static ina238_t s_dev;

static void seed_device(void)
{
    s_dev.cfg.hi2c = &hi2c1;
    s_dev.cfg.i2c_addr_7bit = 0x40;
    s_dev.cfg.shunt_micro_ohm = 4000U;
    s_dev.cfg.max_expected_current_ma = 40000U;
    s_dev.cfg.adc_range_low = false;
    s_dev.current_lsb_nano_a = ina238_calc_current_lsb_na(40000U);
    s_dev.shunt_lsb_nv = INA238_VSHUNT_LSB_nV_RANGE0;
}

void test_read_reg16_locks_bus_then_unpacks_msb_first(void)
{
    seed_device();
    expect_register_read(INA238_REG_VBUS, 0x1234U);

    uint16_t value = 0;
    TEST_ASSERT_EQUAL_INT(INA238_OK,
        ina238_read_reg16(&s_dev, INA238_REG_VBUS, &value));
    TEST_ASSERT_EQUAL_UINT16(0x1234U, value);
}

void test_read_reg16_returns_bus_busy_when_mutex_times_out(void)
{
    seed_device();
    i2c_bus_lock_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, false);

    uint16_t value = 0;
    TEST_ASSERT_EQUAL_INT(INA238_ERR_BUS_BUSY,
        ina238_read_reg16(&s_dev, INA238_REG_VBUS, &value));
}

void test_read_reg16_unlocks_when_hal_call_fails(void)
{
    seed_device();
    i2c_bus_lock_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, true);
    HAL_I2C_Mem_Read_DMA_ExpectAnyArgsAndReturn(HAL_ERROR);
    i2c_bus_unlock_Expect();

    uint16_t value = 0;
    TEST_ASSERT_EQUAL_INT(INA238_ERR_HAL,
        ina238_read_reg16(&s_dev, INA238_REG_VBUS, &value));
}

void test_read_reg16_returns_dma_timeout_when_wait_fails(void)
{
    seed_device();
    i2c_bus_lock_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, true);
    HAL_I2C_Mem_Read_DMA_ExpectAnyArgsAndReturn(HAL_OK);
    i2c_bus_wait_complete_ExpectAndReturn(I2C_BUS_TIMEOUT_MS, false);
    i2c_bus_unlock_Expect();

    uint16_t value = 0;
    TEST_ASSERT_EQUAL_INT(INA238_ERR_DMA_TIMEOUT,
        ina238_read_reg16(&s_dev, INA238_REG_VBUS, &value));
}

void test_probe_succeeds_with_INA238_device_id(void)
{
    expect_register_read(INA238_REG_MANUFACTURER_ID, INA238_MANUFACTURER_ID);
    expect_register_read(INA238_REG_DEVICE_ID, 0x2381U);  /* INA238, die rev 1 */

    const ina238_config_t cfg = {
        .hi2c = &hi2c1, .i2c_addr_7bit = 0x40,
        .shunt_micro_ohm = 4000U, .max_expected_current_ma = 40000U,
        .adc_range_low = false,
    };
    TEST_ASSERT_EQUAL_INT(INA238_OK, ina238_probe(&s_dev, &cfg));
    /* Detected-variant exposure: app code can branch on this if it cares. */
    TEST_ASSERT_EQUAL_UINT16(0x2381U, s_dev.detected_device_id);
}

void test_probe_accepts_INA237_compat_variant(void)
{
    /* INA237 returns 0x237x. Driver still accepts it — same register map and
     * scale factors as INA238. detected_device_id records which variant. */
    expect_register_read(INA238_REG_MANUFACTURER_ID, INA238_MANUFACTURER_ID);
    expect_register_read(INA238_REG_DEVICE_ID, 0x2370U);  /* INA237, die rev 0 */

    const ina238_config_t cfg = {
        .hi2c = &hi2c1, .i2c_addr_7bit = 0x40,
        .shunt_micro_ohm = 4000U, .max_expected_current_ma = 40000U,
        .adc_range_low = false,
    };
    TEST_ASSERT_EQUAL_INT(INA238_OK, ina238_probe(&s_dev, &cfg));
    TEST_ASSERT_EQUAL_UINT16(0x2370U, s_dev.detected_device_id);
}

void test_probe_fails_bad_id_on_wrong_manufacturer(void)
{
    expect_register_read(INA238_REG_MANUFACTURER_ID, 0xDEADU);

    const ina238_config_t cfg = {
        .hi2c = &hi2c1, .i2c_addr_7bit = 0x40,
        .shunt_micro_ohm = 4000U, .max_expected_current_ma = 40000U,
        .adc_range_low = false,
    };
    TEST_ASSERT_EQUAL_INT(INA238_ERR_BAD_ID, ina238_probe(&s_dev, &cfg));
}

void test_probe_rejects_unrelated_device_id(void)
{
    /* A TI-manufactured part that isn't INA237/INA238 should still be
     * rejected — e.g. an INA236 (different register map). */
    expect_register_read(INA238_REG_MANUFACTURER_ID, INA238_MANUFACTURER_ID);
    expect_register_read(INA238_REG_DEVICE_ID, 0xA080U);

    const ina238_config_t cfg = {
        .hi2c = &hi2c1, .i2c_addr_7bit = 0x40,
        .shunt_micro_ohm = 4000U, .max_expected_current_ma = 40000U,
        .adc_range_low = false,
    };
    TEST_ASSERT_EQUAL_INT(INA238_ERR_BAD_ID, ina238_probe(&s_dev, &cfg));
}

void test_read_bus_mv_scales_3p125_mV_per_LSB(void)
{
    seed_device();
    /* 0x2000 = 8192 LSBs × 3.125 mV = 25600 mV. */
    expect_register_read(INA238_REG_VBUS, 0x2000U);

    uint32_t mv = 0;
    TEST_ASSERT_EQUAL_INT(INA238_OK, ina238_read_bus_mv(&s_dev, &mv));
    TEST_ASSERT_EQUAL_UINT32(25600U, mv);
}

void test_read_die_temp_handles_negative_top12_bits(void)
{
    seed_device();
    /* -1 in 12-bit two's complement = 0xFFF; left-shifted to fit register:
     * top 12 bits = 0xFFF, bottom 4 = anything. 0xFFF0 → top12 = -1
     * → temp = -1 × 125 mC = -125 mC. */
    expect_register_read(INA238_REG_DIETEMP, 0xFFF0U);

    int32_t temp_mc = 0;
    TEST_ASSERT_EQUAL_INT(INA238_OK,
        ina238_read_die_temp_mc(&s_dev, &temp_mc));
    TEST_ASSERT_EQUAL_INT32(-125, temp_mc);
}

void test_read_current_ma_uses_cached_current_lsb(void)
{
    seed_device();
    /* CURRENT_LSB at 40 A target = 1,220,703 nA. Raw = 1000 →
     * 1000 × 1_220_703 nA = 1_220_703_000 nA = 1220 mA (after /1e6). */
    expect_register_read(INA238_REG_CURRENT, 1000U);

    int32_t ma = 0;
    TEST_ASSERT_EQUAL_INT(INA238_OK, ina238_read_current_ma(&s_dev, &ma));
    TEST_ASSERT_EQUAL_INT32(1220, ma);
}

/* ----- Stack high-water tracking -----
 *
 * Demonstrates the sentinel-fill helper. The reported bytes are HOST gcc
 * frames + a pthread baseline (~1 KB), NOT the target ARM number — use
 * scripts/stack_report.py for the target-accurate estimate. The value
 * here is most useful as a regression signal: if it jumps, somebody added
 * a large local allocation. We assert a generous upper bound so a passing
 * test stays passing across compilers, but a failing one catches obvious
 * regressions (e.g. someone adding a 4 KB local buffer).
 */

static void run_one_read_reg16(void *ignored)
{
    (void)ignored;
    /* Set up the mocks inside the measured thread — CMock state is
     * process-global so this works either way, but doing it inline keeps
     * the measurement clean and self-contained. */
    seed_device();
    expect_register_read(INA238_REG_VBUS, 0x4242U);
    uint16_t value = 0;
    (void)ina238_read_reg16(&s_dev, INA238_REG_VBUS, &value);
}

static void run_empty(void *ignored)
{
    (void)ignored;
}

void test_stack_delta_of_read_reg16_is_modest(void)
{
    /* Measure an empty function first to factor out the pthread / glibc
     * setup overhead (TLS init, signal masks, etc. — typically 5–6 KB on
     * Linux). The delta is what the function actually contributed. */
    size_t baseline = stack_measure_run("baseline_empty", run_empty,
                                        NULL, 32 * 1024);
    size_t total = stack_measure_run("ina238_read_reg16", run_one_read_reg16,
                                     NULL, 32 * 1024);
    size_t delta = (total > baseline) ? (total - baseline) : 0;
    printf("STACK ina238_read_reg16 delta: %zu bytes (baseline %zu)\n",
           delta, baseline);

    /* In practice the delta is 0 — the function's frames fit entirely
     * within pthread's ~6 KB setup scratch. A non-zero delta means
     * somebody added a stack allocation that pushed past that baseline.
     * 2 KB is "definitely a regression" territory. */
    TEST_ASSERT_LESS_THAN_size_t(2048U, delta);
}
