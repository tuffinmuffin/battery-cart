/**
 * test_direct_io.c — host-side unit tests for direct_io.c.
 *
 * Demonstrates the Unity/CMock pattern: each helper in direct_io is verified
 * to call the right HAL function with the right port + pin + state. CMock
 * auto-generates the mocks from main.h and tim.h (the stubs in
 * test/support/), so we don't have to hand-write mock implementations.
 */

#include "unity.h"
#include "direct_io.h"
#include "mock_main.h"   /* mocks HAL_GPIO_WritePin/TogglePin */
#include "mock_tim.h"    /* mocks HAL_TIM_PWM_Start, __HAL_TIM_SET_COMPARE,
                            __HAL_TIM_GET_AUTORELOAD */

void setUp(void) {}
void tearDown(void) {}

/* ----- LED ----- */

void test_led_toggle_calls_HAL_GPIO_TogglePin_on_PC15(void)
{
    HAL_GPIO_TogglePin_Expect(MCU_LED_GPIO_Port, MCU_LED_Pin);
    led_toggle();
}

void test_led_enable_writes_pin_high(void)
{
    HAL_GPIO_WritePin_Expect(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_SET);
    led_enable();
}

void test_led_disable_writes_pin_low(void)
{
    HAL_GPIO_WritePin_Expect(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_RESET);
    led_disable();
}

/* ----- Relay K1 ----- */

void test_relay_enable_writes_PA1_high(void)
{
    HAL_GPIO_WritePin_Expect(GPIO_K1_GPIO_Port, GPIO_K1_Pin, GPIO_PIN_SET);
    relay_enable();
}

void test_relay_disable_writes_PA1_low(void)
{
    HAL_GPIO_WritePin_Expect(GPIO_K1_GPIO_Port, GPIO_K1_Pin, GPIO_PIN_RESET);
    relay_disable();
}

/* ----- Bleed FET ----- */

void test_bleed_enable_writes_PA2_high(void)
{
    HAL_GPIO_WritePin_Expect(GPIO_BLEED_GPIO_Port, GPIO_BLEED_Pin, GPIO_PIN_SET);
    bleed_enable();
}

void test_bleed_disable_writes_PA2_low(void)
{
    HAL_GPIO_WritePin_Expect(GPIO_BLEED_GPIO_Port, GPIO_BLEED_Pin, GPIO_PIN_RESET);
    bleed_disable();
}

/* ----- Fan PWM ----- */

void test_fan_init_starts_PWM_with_zero_duty(void)
{
    __HAL_TIM_SET_COMPARE_Expect(&htim1, TIM_CHANNEL_4, 0U);
    HAL_TIM_PWM_Start_ExpectAndReturn(&htim1, TIM_CHANNEL_4, 0U);
    fan_init();
}

void test_fan_set_duty_50pct_writes_half_of_ARR(void)
{
    /* If ARR=1000, 50% should yield CCR=500. */
    __HAL_TIM_GET_AUTORELOAD_ExpectAndReturn(&htim1, 1000U);
    __HAL_TIM_SET_COMPARE_Expect(&htim1, TIM_CHANNEL_4, 500U);
    fan_set_duty(50);
}

void test_fan_set_duty_0pct_writes_zero(void)
{
    __HAL_TIM_GET_AUTORELOAD_ExpectAndReturn(&htim1, 1000U);
    __HAL_TIM_SET_COMPARE_Expect(&htim1, TIM_CHANNEL_4, 0U);
    fan_set_duty(0);
}

void test_fan_set_duty_100pct_writes_full_ARR(void)
{
    __HAL_TIM_GET_AUTORELOAD_ExpectAndReturn(&htim1, 1000U);
    __HAL_TIM_SET_COMPARE_Expect(&htim1, TIM_CHANNEL_4, 1000U);
    fan_set_duty(100);
}

void test_fan_set_duty_over_100_clamps_to_100(void)
{
    /* 250 should be clamped down to 100% (CCR = ARR). */
    __HAL_TIM_GET_AUTORELOAD_ExpectAndReturn(&htim1, 1000U);
    __HAL_TIM_SET_COMPARE_Expect(&htim1, TIM_CHANNEL_4, 1000U);
    fan_set_duty(250);
}

/* ----- LED state readback ----- */

void test_led_state_returns_true_when_pin_high(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_SET);
    TEST_ASSERT_TRUE(led_state());
}

void test_led_state_returns_false_when_pin_low(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_RESET);
    TEST_ASSERT_FALSE(led_state());
}

/* ----- Switch inputs ----- */

void test_sw_a_read_returns_true_when_pressed(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(SW_A_GPIO_Port, SW_A_Pin, GPIO_PIN_SET);
    TEST_ASSERT_TRUE(sw_a_read());
}

void test_sw_a_read_returns_false_when_released(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(SW_A_GPIO_Port, SW_A_Pin, GPIO_PIN_RESET);
    TEST_ASSERT_FALSE(sw_a_read());
}

void test_sw_b_read_returns_true_when_pressed(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(SW_B_GPIO_Port, SW_B_Pin, GPIO_PIN_SET);
    TEST_ASSERT_TRUE(sw_b_read());
}

void test_sw_b_read_returns_false_when_released(void)
{
    HAL_GPIO_ReadPin_ExpectAndReturn(SW_B_GPIO_Port, SW_B_Pin, GPIO_PIN_RESET);
    TEST_ASSERT_FALSE(sw_b_read());
}
