/**
 * direct_io.h — thin wrappers over the board's GPIO outputs and the fan PWM.
 *
 * Pins (per BatteryMonitorV2.ioc):
 *   MCU_LED        PC15  push-pull output, active-high
 *   GPIO_K1        PA1   push-pull output, active-high (relay K1 drive)
 *   GPIO_BLEED     PA2   push-pull output, active-high (bleed FET drive)
 *   FAN_PWM        PA3   TIM1_CH4 PWM output
 *
 * Active-high convention throughout: `enable` -> GPIO_PIN_SET, `disable` -> RESET.
 * If hardware inverts a signal, flip it here, not at the call site.
 */

#ifndef DIRECT_IO_H
#define DIRECT_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- LED (MCU_LED, PC15) --- */
void led_toggle(void);
void led_enable(void);
void led_disable(void);

/* --- Relay K1 (PA1) --- */
void relay_enable(void);
void relay_disable(void);

/* --- Bleed FET (PA2) --- */
void bleed_enable(void);
void bleed_disable(void);

/* --- Fan PWM (TIM1_CH4 on PA3) ---
 *
 * fan_init() must be called once before fan_set_duty(). It starts the PWM
 * with CCR=0 (fan off). fan_set_duty(percent) accepts 0..100; values >100
 * clamp to 100. 100% is effectively 100% (CCR=ARR is one count low per
 * period — undetectable to a fan).
 */
void fan_init(void);
void fan_set_duty(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* DIRECT_IO_H */
