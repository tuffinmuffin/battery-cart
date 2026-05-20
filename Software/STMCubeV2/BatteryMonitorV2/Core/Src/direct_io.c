/**
 * direct_io.c — see direct_io.h for the rationale and pin map.
 */

#include "direct_io.h"

#include "main.h"
#include "tim.h"

/* ----- LED ----- */

void led_toggle(void)
{
    HAL_GPIO_TogglePin(MCU_LED_GPIO_Port, MCU_LED_Pin);
}

void led_enable(void)
{
    HAL_GPIO_WritePin(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_SET);
}

void led_disable(void)
{
    HAL_GPIO_WritePin(MCU_LED_GPIO_Port, MCU_LED_Pin, GPIO_PIN_RESET);
}

bool led_state(void)
{
    /* HAL_GPIO_ReadPin on an output pin reads the IDR which on STM32
     * mirrors the driven output level — exactly what we want here so
     * the display heartbeat tracks the actual LED line, not a
     * shadow variable that could drift. */
    return HAL_GPIO_ReadPin(MCU_LED_GPIO_Port, MCU_LED_Pin) == GPIO_PIN_SET;
}

/* ----- Relay K1 ----- */

void relay_enable(void)
{
    HAL_GPIO_WritePin(GPIO_K1_GPIO_Port, GPIO_K1_Pin, GPIO_PIN_SET);
}

void relay_disable(void)
{
    HAL_GPIO_WritePin(GPIO_K1_GPIO_Port, GPIO_K1_Pin, GPIO_PIN_RESET);
}

/* ----- Bleed FET ----- */

void bleed_enable(void)
{
    HAL_GPIO_WritePin(GPIO_BLEED_GPIO_Port, GPIO_BLEED_Pin, GPIO_PIN_SET);
}

void bleed_disable(void)
{
    HAL_GPIO_WritePin(GPIO_BLEED_GPIO_Port, GPIO_BLEED_Pin, GPIO_PIN_RESET);
}

/* ----- Fan PWM (TIM1_CH4) ----- */

void fan_init(void)
{
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, 0);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4);
}

void fan_set_duty(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    /* Scale 0..100% across the timer's reload range. CCR == ARR gives ~100%
     * (one timer count low per period, undetectable to a fan). */
    const uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);
    const uint32_t ccr = ((uint32_t)percent * arr) / 100U;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, ccr);
}

/* ----- Switches (front-panel buttons) ----- */

bool sw_a_read(void)
{
    return HAL_GPIO_ReadPin(SW_A_GPIO_Port, SW_A_Pin) == GPIO_PIN_SET;
}

bool sw_b_read(void)
{
    return HAL_GPIO_ReadPin(SW_B_GPIO_Port, SW_B_Pin) == GPIO_PIN_SET;
}
