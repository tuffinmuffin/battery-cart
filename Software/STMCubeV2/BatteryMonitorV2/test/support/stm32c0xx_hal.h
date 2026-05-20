/**
 * test/support/stm32c0xx_hal.h — host-test stub replacing Drivers/STM32C0xx_HAL_Driver/Inc/stm32c0xx_hal.h.
 *
 * The real HAL header pulls in stm32c0xx.h which pulls in CMSIS / ARM
 * intrinsics that don't parse on host gcc. Modules that need a HAL
 * surface in tests can include this; today it just forwards to the
 * existing i2c.h stub (which carries HAL_StatusTypeDef + the I2C
 * function prototypes). Grow this stub if a future module needs more.
 */

#ifndef STM32C0XX_HAL_H
#define STM32C0XX_HAL_H

#include "i2c.h"

#endif /* STM32C0XX_HAL_H */
