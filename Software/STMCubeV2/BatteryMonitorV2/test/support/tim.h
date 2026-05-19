/**
 * test/support/tim.h — host-test stub replacing Core/Inc/tim.h.
 *
 * Provides the TIM_HandleTypeDef and HAL_TIM_PWM_Start symbols direct_io
 * references. The PWM-set-compare / get-autoreload macros from the real
 * STM HAL get redefined as ordinary function calls here so they can be
 * intercepted by CMock.
 */

#ifndef TIM_H
#define TIM_H

#include <stdint.h>

typedef struct {
    struct {
        uint32_t Period;
    } Init;
} TIM_HandleTypeDef;

extern TIM_HandleTypeDef htim1;

/* TIM channel constants — direct_io only uses CH4. */
#define TIM_CHANNEL_4  4U

/* HAL function prototypes (mocked by CMock). */
uint32_t HAL_TIM_PWM_Start(TIM_HandleTypeDef *htim, uint32_t channel);

/* The real HAL exposes these as macros that touch hardware registers. In
 * tests they're ordinary functions so CMock can intercept them. direct_io.c
 * uses them the same way (the call expression is identical). */
void     __HAL_TIM_SET_COMPARE(TIM_HandleTypeDef *htim, uint32_t channel, uint32_t value);
uint32_t __HAL_TIM_GET_AUTORELOAD(TIM_HandleTypeDef *htim);

#endif /* TIM_H */
