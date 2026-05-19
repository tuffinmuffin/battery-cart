/**
 * test/support/test_globals.c — variable storage for symbols the host-test
 * stubs declare extern. Linked into every test binary.
 */

#include "main.h"
#include "tim.h"

/* Unique `_id` per port so CMock's default pointer-arg memcmp distinguishes
 * one port from another. Values are arbitrary — just need to differ. */
GPIO_TypeDef MCU_LED_GPIO_Port_storage    = {1};
GPIO_TypeDef GPIO_K1_GPIO_Port_storage    = {2};
GPIO_TypeDef GPIO_BLEED_GPIO_Port_storage = {3};
GPIO_TypeDef FAN_PWM_GPIO_Port_storage    = {4};

/* The real htim1 is allocated in Core/Src/tim.c; tests don't pull that file
 * in (it depends on real HAL types). Provide a zero-initialised placeholder
 * so CMock's pointer-match against &htim1 works and direct_io can read
 * htim1.Init.Period if it wants to. */
TIM_HandleTypeDef htim1 = {{0}};
