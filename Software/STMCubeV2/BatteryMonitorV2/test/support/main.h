/**
 * test/support/main.h — host-test stub replacing Core/Inc/main.h.
 *
 * Provides just enough HAL surface for direct_io.c to compile on the host:
 *   - opaque GPIO_TypeDef + GPIO_PinState enum
 *   - the four pin / port symbols direct_io references (as #defines so the
 *     linker doesn't need to resolve them — CMock matches by value)
 *   - HAL_GPIO_WritePin / HAL_GPIO_TogglePin prototypes (CMock mocks these)
 *
 * The real Core/Inc/main.h pulls in the STM32 CMSIS chain which has ARM-
 * specific intrinsics that don't parse with host gcc. Ceedling's :paths:
 * :include: puts this directory first, so #include "main.h" from inside
 * direct_io.c lands here instead of the real header.
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

/* GPIO peripheral type — complete (CMock needs sizeof on dereferenced
 * pointer args to memcmp them). Single-byte `_id` is just a tag so memcmp
 * distinguishes one port from another. */
typedef struct GPIO_TypeDef {
    uint8_t _id;
} GPIO_TypeDef;

typedef enum {
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET   = 1,
} GPIO_PinState;

/* Storage allocated in test/support/test_globals.c. Each port has a unique
 * `_id` so a CMock _Expect against the wrong port fails (memcmp differs). */
extern GPIO_TypeDef MCU_LED_GPIO_Port_storage;
extern GPIO_TypeDef GPIO_K1_GPIO_Port_storage;
extern GPIO_TypeDef GPIO_BLEED_GPIO_Port_storage;
extern GPIO_TypeDef FAN_PWM_GPIO_Port_storage;
extern GPIO_TypeDef SW_A_GPIO_Port_storage;
extern GPIO_TypeDef SW_B_GPIO_Port_storage;

#define MCU_LED_GPIO_Port      (&MCU_LED_GPIO_Port_storage)
#define GPIO_K1_GPIO_Port      (&GPIO_K1_GPIO_Port_storage)
#define GPIO_BLEED_GPIO_Port   (&GPIO_BLEED_GPIO_Port_storage)
#define FAN_PWM_GPIO_Port      (&FAN_PWM_GPIO_Port_storage)
#define SW_A_GPIO_Port         (&SW_A_GPIO_Port_storage)
#define SW_B_GPIO_Port         (&SW_B_GPIO_Port_storage)

#define MCU_LED_Pin     ((uint16_t)0x8000U)  /* PC15 */
#define GPIO_K1_Pin     ((uint16_t)0x0002U)  /* PA1 */
#define GPIO_BLEED_Pin  ((uint16_t)0x0004U)  /* PA2 */
#define FAN_PWM_Pin     ((uint16_t)0x0008U)  /* PA3 */
#define SW_A_Pin        ((uint16_t)0x0020U)  /* PA5 */
#define SW_B_Pin        ((uint16_t)0x0001U)  /* PB0 */

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

#endif /* MAIN_H */
