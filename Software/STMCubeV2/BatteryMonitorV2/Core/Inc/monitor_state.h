/**
 * monitor_state.h — shared snapshot of the latest battery-monitor readings.
 *
 * The INA238 task (when enabled) is the producer; the display task is
 * the consumer. A single mutex-protected snapshot keeps the producer
 * and consumer decoupled — neither holds the bus mutex while drawing
 * or formatting, and the display can't read a half-updated set of
 * values.
 *
 * All values are integer-encoded (no float math in the producer or the
 * consumer) so the existing integer-only vfprintf is enough for both
 * the CDC log and the OLED render.
 */

#ifndef MONITOR_STATE_H
#define MONITOR_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t vbus_mv;        /* INA238 VBUS in mV (0..81920 for 81.92 V range)   */
    int32_t  current_ma;     /* INA238 calibrated current in mA, signed          */
    int32_t  vshunt_uv;      /* INA238 raw shunt voltage in uV, signed           */
    int32_t  tdie_mc;        /* INA238 die temperature in milli-degrees C, signed */
    bool     k1_on;          /* K1 relay state (active-high)                      */
    bool     bleed_on;       /* Bleed FET state (active-high)                     */
    uint32_t fan_duty_pct;   /* Fan PWM duty, 0..100                              */
    uint32_t timestamp_ms;   /* osKernelGetTickCount() at the producer write      */
} monitor_snapshot_t;

/* Create the mutex. Idempotent: only first call wins. Must come before
 * any task that calls update / get. Call from MX_FREERTOS_Init after
 * osKernelInitialize(). */
void monitor_state_init(void);

/* Producer side: copy `s` into the shared snapshot under lock.
 * Safe to call from any task. If `s` is NULL the call is a no-op. */
void monitor_state_update(const monitor_snapshot_t *s);

/* Consumer side: copy the shared snapshot into `out` under lock.
 * Safe to call from any task. If `out` is NULL the call is a no-op.
 * Zeroed result if monitor_state_init() hasn't been called yet. */
void monitor_state_get(monitor_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_STATE_H */
