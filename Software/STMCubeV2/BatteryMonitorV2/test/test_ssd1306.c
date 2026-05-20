/**
 * test_ssd1306.c — host-side unit tests for ssd1306.c.
 *
 * Direct switch-case coverage for the two u8g2 callbacks the driver
 * exposes:
 *
 *   u8x8_byte_stm32_hw_i2c       — marshals u8g2's CAD protocol into
 *                                  HAL_I2C_Master_Transmit calls,
 *                                  buffering between START_TRANSFER
 *                                  and END_TRANSFER.
 *   u8x8_gpio_and_delay_stm32    — translates u8g2 delay/GPIO messages
 *                                  into osDelay or no-ops.
 *
 * Both callbacks were made non-static in ssd1306.c so this file can
 * call them directly with a synthetic u8x8_t and assert per-branch
 * behaviour without spinning up the full u8g2 init path. Each test
 * exercises one switch case in isolation so a regression points at
 * the exact branch.
 */

#include "unity.h"
#include "ssd1306.h"
#include "u8x8.h"

#include "mock_cmsis_os2.h"
#include "mock_i2c.h"

#include <string.h>

/* Forward declarations — the callbacks aren't in ssd1306.h since
 * production code reaches them only through the u8g2 setup
 * constructor's function-pointer arguments. Tests are the lone
 * direct caller. */
uint8_t u8x8_byte_stm32_hw_i2c(u8x8_t *u8x8, uint8_t msg,
                               uint8_t arg_int, void *arg_ptr);
uint8_t u8x8_gpio_and_delay_stm32(u8x8_t *u8x8, uint8_t msg,
                                  uint8_t arg_int, void *arg_ptr);

/* u8g2 8-bit address convention: (7-bit address) << 1. 0x3C → 0x78. */
#define EXPECTED_I2C_ADDR_8BIT  0x78U

#define SSD1306_HAL_TX_TIMEOUT_MS  100U

static u8x8_t s_u8x8;

/* Capture for HAL_I2C_Master_Transmit so SEND/END tests can assert on
 * the bytes the driver actually sent. */
#define CAP_BUF_LEN  300
static struct {
    int      call_count;
    uint16_t last_addr;
    uint16_t last_size;
    uint32_t last_timeout;
    uint8_t  last_buf[CAP_BUF_LEN];
} s_tx_cap;

static HAL_StatusTypeDef hal_tx_capture_ok(I2C_HandleTypeDef *hi2c,
                                           uint16_t addr,
                                           uint8_t *data,
                                           uint16_t size,
                                           uint32_t timeout,
                                           int call_count)
{
    (void)hi2c;
    (void)call_count;
    s_tx_cap.last_addr    = addr;
    s_tx_cap.last_size    = size;
    s_tx_cap.last_timeout = timeout;
    if (size <= CAP_BUF_LEN && data != NULL) {
        memcpy(s_tx_cap.last_buf, data, size);
    }
    s_tx_cap.call_count++;
    return HAL_OK;
}

static HAL_StatusTypeDef hal_tx_capture_err(I2C_HandleTypeDef *hi2c,
                                            uint16_t addr,
                                            uint8_t *data,
                                            uint16_t size,
                                            uint32_t timeout,
                                            int call_count)
{
    (void)hi2c; (void)addr; (void)data; (void)size; (void)timeout; (void)call_count;
    s_tx_cap.call_count++;
    return HAL_ERROR;
}

void setUp(void)
{
    memset(&s_u8x8, 0, sizeof(s_u8x8));
    s_u8x8.i2c_address = EXPECTED_I2C_ADDR_8BIT;
    memset(&s_tx_cap, 0, sizeof(s_tx_cap));
}

void tearDown(void) { }

/* ---------- Byte callback: SEND / START / END flow --------------- */

/* START_TRANSFER followed immediately by END_TRANSFER with no SEND
 * in between → the accumulator stays at 0, so the driver must NOT
 * call HAL (an empty I2C transmit is a protocol no-op the chip
 * shouldn't see). No mock expectation = CMock fails if it's called. */
void test_byte_empty_transfer_does_not_call_hal(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_START_TRANSFER, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_END_TRANSFER, 0, NULL));
    TEST_ASSERT_EQUAL_INT(0, s_tx_cap.call_count);
}

/* SEND of N bytes between START / END produces exactly one HAL call,
 * with the bytes intact and the address pulled from u8x8->i2c_address. */
void test_byte_send_then_end_transmits_buffered_bytes(void)
{
    uint8_t payload[] = { 0x00, 0xAE, 0xD5, 0x80 };

    HAL_I2C_Master_Transmit_AddCallback(hal_tx_capture_ok);
    HAL_I2C_Master_Transmit_ExpectAnyArgsAndReturn(HAL_OK);

    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_START_TRANSFER, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_SEND, sizeof(payload), payload));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_END_TRANSFER, 0, NULL));

    TEST_ASSERT_EQUAL_INT(1, s_tx_cap.call_count);
    TEST_ASSERT_EQUAL_UINT16(EXPECTED_I2C_ADDR_8BIT, s_tx_cap.last_addr);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), s_tx_cap.last_size);
    TEST_ASSERT_EQUAL_UINT32(SSD1306_HAL_TX_TIMEOUT_MS, s_tx_cap.last_timeout);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, s_tx_cap.last_buf, sizeof(payload));
}

/* Multiple SEND calls concatenate into one transmit at END. Lets u8g2
 * push command + payload as separate writes without forcing extra
 * I2C STARTs (matches the SSD1306 control-byte + data convention). */
void test_byte_multiple_sends_concat_into_one_transmit(void)
{
    uint8_t hdr[]  = { 0x40 };               /* control byte */
    uint8_t data[] = { 0xAA, 0xBB, 0xCC };
    uint8_t expected[] = { 0x40, 0xAA, 0xBB, 0xCC };

    HAL_I2C_Master_Transmit_AddCallback(hal_tx_capture_ok);
    HAL_I2C_Master_Transmit_ExpectAnyArgsAndReturn(HAL_OK);

    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_START_TRANSFER, 0, NULL);
    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_SEND, sizeof(hdr),  hdr);
    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_SEND, sizeof(data), data);
    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_END_TRANSFER, 0, NULL);

    TEST_ASSERT_EQUAL_INT(1, s_tx_cap.call_count);
    TEST_ASSERT_EQUAL_UINT16(sizeof(expected), s_tx_cap.last_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_tx_cap.last_buf, sizeof(expected));
}

/* SEND with a chunk that would overflow the 256-byte staging buffer
 * returns 0 WITHOUT writing past the buffer end. (We can't directly
 * observe s_tx_buf — but a fresh START guarantees s_tx_len=0, so
 * a single SEND of 257 bytes triggers the overflow path.) */
void test_byte_send_overflow_returns_zero_and_no_hal(void)
{
    uint8_t big[300];
    memset(big, 0xA5, sizeof(big));

    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_START_TRANSFER, 0, NULL);

    /* arg_int is uint8_t (max 255) — single SEND can't overflow on its
     * own. Two SENDs of 200 each will (200 + 200 > 256). */
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_SEND, 200, big));
    TEST_ASSERT_EQUAL_UINT8(0, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_SEND, 200, big));

    /* END after overflow still flushes whatever fit before the overflow,
     * so set up a callback for that. Address still correct. */
    HAL_I2C_Master_Transmit_AddCallback(hal_tx_capture_ok);
    HAL_I2C_Master_Transmit_ExpectAnyArgsAndReturn(HAL_OK);
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_END_TRANSFER, 0, NULL));
    TEST_ASSERT_EQUAL_UINT16(200, s_tx_cap.last_size);
}

/* HAL returning ERROR on END_TRANSFER must propagate as a 0 return so
 * u8g2 can log the failure. The next START resets state cleanly so a
 * later transmit isn't poisoned. */
void test_byte_end_transfer_returns_zero_on_hal_error(void)
{
    uint8_t payload[] = { 0xAB };

    HAL_I2C_Master_Transmit_AddCallback(hal_tx_capture_err);
    HAL_I2C_Master_Transmit_ExpectAnyArgsAndReturn(HAL_ERROR);

    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_START_TRANSFER, 0, NULL);
    (void)u8x8_byte_stm32_hw_i2c(&s_u8x8, U8X8_MSG_BYTE_SEND, 1, payload);
    TEST_ASSERT_EQUAL_UINT8(0, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_END_TRANSFER, 0, NULL));
}

/* SET_DC and INIT messages are deliberately no-ops on this I2C-only
 * transport (D/C# lives in the control byte; CubeMX did the periph
 * init). Both should still return 1 so u8g2 doesn't treat them as
 * errors. */
void test_byte_no_op_messages_return_one(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_INIT, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, U8X8_MSG_BYTE_SET_DC, 0, NULL));
}

/* Any message we don't recognise should return 0 — better to surface
 * a u8g2 protocol mismatch than silently swallow it. */
void test_byte_unknown_message_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, u8x8_byte_stm32_hw_i2c(
        &s_u8x8, 0xFF /* invalid */, 0, NULL));
}

/* ---------- Delay / GPIO callback ------------------------------- */

/* DELAY_MILLI must call osDelay with arg_int as the tick count.
 * u8g2 uses this during init and the SSD1306 init sequence relies
 * on the post-power-on settle time. */
void test_delay_milli_calls_osDelay_with_arg(void)
{
    osDelay_Expect(7U);
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_DELAY_MILLI, 7, NULL));
}

/* DELAY_10MICRO sub-tick — we round up to 1 ms (the floor of our
 * SysTick resolution). Only used during init so a few extra ms is
 * invisible. */
void test_delay_10micro_rounds_up_to_one_ms(void)
{
    osDelay_Expect(1U);
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_DELAY_10MICRO, 5, NULL));
}

/* Sub-microsecond delays (100NANO, NANO) are below SysTick resolution
 * and must be true no-ops — calling osDelay would block for a full ms
 * needlessly. CMock fails the test if osDelay is unexpectedly called. */
void test_delay_sub_microsecond_messages_are_noops(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_DELAY_100NANO, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_DELAY_NANO, 0, NULL));
}

/* All GPIO messages and the GPIO_AND_DELAY_INIT message must be
 * no-ops on this I2C-only display (no RESET / CS / DC / SPI lines
 * are physically wired). Returning 1 keeps u8g2 happy. */
void test_gpio_and_init_messages_are_noops(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_AND_DELAY_INIT, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_RESET,     0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_CS,        0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_DC,        0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_I2C_CLOCK, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, u8x8_gpio_and_delay_stm32(
        &s_u8x8, U8X8_MSG_GPIO_I2C_DATA,  0, NULL));
}

/* Unknown message: must return 0 so a u8g2 protocol mismatch surfaces. */
void test_delay_unknown_message_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, u8x8_gpio_and_delay_stm32(
        &s_u8x8, 0xFF /* invalid */, 0, NULL));
}

/* ---------- ssd1306_init: probe failure -------------------------- */

/* Probe failure (device doesn't ACK) must short-circuit init and
 * return SSD1306_ERR_PROBE without touching u8g2. ssd1306_u8g2()
 * stays NULL because the init didn't complete. NB this test runs
 * BEFORE the probe-success test below; once that one sets
 * s_initialized=true the static stays set for the rest of the
 * binary. */
void test_ssd1306_init_returns_err_probe_on_no_ack(void)
{
    HAL_I2C_IsDeviceReady_ExpectAndReturn(
        &hi2c2,
        (uint16_t)(0x3CU << 1U),
        3U,    /* SSD1306_PROBE_TRIES */
        50U,   /* SSD1306_PROBE_TIMEOUT_MS */
        HAL_ERROR);
    TEST_ASSERT_EQUAL_INT(SSD1306_ERR_PROBE, ssd1306_init(0x3CU));
    TEST_ASSERT_NULL(ssd1306_u8g2());
}

/* Probe success: HAL_I2C_IsDeviceReady returns HAL_OK; u8g2 setup +
 * init helpers are linked from test/support/u8g2_stubs.c so the
 * post-probe path runs to completion. ssd1306_init returns OK and
 * ssd1306_u8g2() now returns the module's instance pointer. */
void test_ssd1306_init_succeeds_on_probe_ok(void)
{
    HAL_I2C_IsDeviceReady_ExpectAndReturn(
        &hi2c2,
        (uint16_t)(0x3CU << 1U),
        3U,    /* SSD1306_PROBE_TRIES */
        50U,   /* SSD1306_PROBE_TIMEOUT_MS */
        HAL_OK);
    TEST_ASSERT_EQUAL_INT(SSD1306_OK, ssd1306_init(0x3CU));
    TEST_ASSERT_NOT_NULL(ssd1306_u8g2());
}
