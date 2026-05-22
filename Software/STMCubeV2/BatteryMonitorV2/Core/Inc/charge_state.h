/**
 * charge_state.h — battery charging state machine.
 *
 * Owns K1 / bleed / fan from boot onward. Runs as its own FreeRTOS
 * task; commands + safety faults arrive on a single event queue.
 * Consumers (display, CDC) read state via charge_state_get().
 *
 * Design reference (topology, hardware quirks, Mermaid state diagram,
 * per-state I/O tables, FAULT-by-reason table, safety contract, and
 * tunable rationale) lives in docs/charge_state.md. Keep that file in
 * sync with the enum below and the switch cases in charge_state.c —
 * the project's state-machine-reviewer agent diffs them.
 */

#ifndef CHARGE_STATE_H
#define CHARGE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- States ------------------------------------------------------- */

typedef enum {
    CHARGE_STATE_INIT = 0,
    CHARGE_STATE_IDLE,
    CHARGE_STATE_CHARGE_PREPARE,
    CHARGE_STATE_CHARGING,
    CHARGE_STATE_TEST_PREP,
    CHARGE_STATE_TEST,
    CHARGE_STATE_TRICKLE,
    CHARGE_STATE_MONITOR,
    CHARGE_STATE_FAULT,
    CHARGE_STATE_COUNT,
} charge_state_t;

typedef enum {
    FAULT_NONE = 0,
    FAULT_CHRG_TIMEOUT,
    FAULT_OVERCURRENT,
    FAULT_OVERTEMP,
    FAULT_BATT_REJECTED,
    FAULT_SAFETY_OTHER,
    FAULT_COUNT,
} fault_reason_t;

/* --- Events ------------------------------------------------------- */

typedef enum {
    EVT_TICK = 0,
    EVT_CMD_START_TEST,
    EVT_CMD_RESET_FAULT,
    EVT_CMD_FORCE_IDLE,
    EVT_CMD_ENTER_MONITOR,
    EVT_CMD_EXIT_MONITOR,
    EVT_SAFETY_FAULT,            /* payload: fault_reason_t */
    EVT_BATTERY_REMOVED,
} charge_event_id_t;

typedef struct {
    charge_event_id_t id;
    uint32_t          payload;
} charge_event_t;

/* --- Snapshot exposed to consumers (display, CDC) ---------------- */

typedef struct {
    charge_state_t state;
    fault_reason_t fault;
    uint32_t       vbus_mv;
    int32_t        current_ma;
    int32_t        tdie_mc;
    int32_t        ntc_mc;          /* INT32_MIN until NTC driver lands */
    bool           k1_on;
    bool           bleed_on;
    bool           battery_present;     /* derived; see hardware-quirk note */
    uint8_t        fan_duty_pct;
    uint32_t       time_in_state_s;
    uint32_t       charge_time_s;   /* cumulative time in CHARGING this cycle */
    uint32_t       v0_mv;           /* battery V captured on CHARGE_PREPARE entry */
    uint32_t       timestamp_ms;
} charge_snapshot_t;

/* --- Tunables (review against bench data; T_NTC_MAX is TBR) ------ */

#define V_PRESENT_MV          10500    /* IDLE -> CHARGE_PREPARE */
#define V_REVERSE_MV          (-500)
#define V_SAFE_MV             10000    /* FAULT -> IDLE drain target */
#define V_CHARGE_RISE_MV      12700    /* CHARGING 30 s sanity threshold */
#define T_PRESENT_SAMPLES     5
#define T_MIN_CHARGE_S        180
#define T_TAIL_S              180
#define I_END_MA              100
#define T_CHARGE_TIMEOUT_S    30
#define I_OC_MA               6500
#define T_OC_MS               2000
#define T_TEST_SETTLE_S       7
#define T_TEST_S              60
#define T_NTC_MAX_C           60       /* TBR — depends on bleed thermal */
#define T_BATT_GONE_S         3        /* derived-signal debounce */

/* --- Lifecycle --------------------------------------------------- */

/* Create mutex + event queue. Idempotent. Call from MX_FREERTOS_Init
 * after osKernelInitialize(), before any consumer/producer call. */
void charge_state_init(void);

/* Spawn the state-machine task. */
void charge_state_task_start(void);

/* --- Consumer API ------------------------------------------------ */

/* Copy the current snapshot. NULL out is a no-op; zeroed result if
 * called before charge_state_init(). */
void        charge_state_get(charge_snapshot_t *out);
const char *charge_state_label(charge_state_t s);
const char *fault_reason_label(fault_reason_t f);

/* --- Producer API (commands) ------------------------------------- */

/* Post an event to the state-machine queue. Safe to call from any
 * task (not ISR). Returns osOK on success, osErrorResource if the
 * queue is full. */
osStatus_t  charge_state_post(charge_event_id_t id, uint32_t payload);

/* Run one iteration of the state machine. The task body calls this
 * in a loop; host tests call it directly to exercise transitions
 * without a real queue. NULL evt is treated as an internal EVT_TICK. */
void        charge_state_tick(uint32_t now_ms, const charge_event_t *evt);

/* --- Safety monitor interface ------------------------------------ */

/* Drive outputs to a conservative safe state (K1 on isolating the
 * charger, bleed off, fan as needed) AND post EVT_SAFETY_FAULT with
 * the given reason. Idempotent. The state machine formalizes the
 * FAULT transition on its next tick and may re-assert outputs per
 * the reason table above. */
void charge_state_safety_fault(fault_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_STATE_H */
