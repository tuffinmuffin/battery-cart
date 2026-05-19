/**
 * ina238_task.h — FreeRTOS task that brings up the INA238 (or INA237 compat)
 * and emits readings over USB-CDC as a hardware smoke test.
 *
 * Owns the device instance, the task handle, and the readout loop. Kept
 * separate from app_freertos.c so the FreeRTOS scaffolding doesn't bloat
 * with per-sensor application code.
 */

#ifndef INA238_TASK_H
#define INA238_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the task. Call from MX_FREERTOS_Init after i2c_bus_init(). */
void ina238_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* INA238_TASK_H */
