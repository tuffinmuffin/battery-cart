/**
 * charge_state.c — see charge_state.h for the contract.
 *
 * Static FSM behind a snapshot mutex (mirrors monitor_state.c) plus
 * an osMessageQueue for inputs (commands + safety faults). The task
 * body just pulls events and calls charge_state_tick(); the tick
 * function IS the state machine and is host-testable via the CMock
 * cmsis_os2 / direct_io / monitor_state mocks.
 *
 * Telemetry source today is monitor_state_get() — placeholder until
 * the controller owns its own INA238 read pump. The ina238_task
 * that would feed monitor_state is currently disabled in
 * app_freertos.c, so the V/I fields will read zero on hardware
 * until that's wired or the controller pulls direct.
 *
 * Per-state output rows + per-fault-reason output rows live in two
 * lookup tables (STATE_OUTPUTS / FAULT_OUTPUTS) so apply_outputs() is
 * the single place GPIO actually moves. That gives the SPDT
 * "bleed-off before K1 transition" invariant exactly one
 * enforcement point.
 *
 * Real per-state logic (charge termination, OC detection, pretest
 * pulse, battery-removed signature) is intentionally stubbed — this
 * skeleton wires the framework and the basic transitions so the
 * next commits can fill each state in independently.
 */

#include "charge_state.h"

#include "direct_io.h"
#include "monitor_state.h"

#include <limits.h>
#include <string.h>

#define TASK_STACK_WORDS    256U
#define TASK_PRIORITY       osPriorityNormal
#define QUEUE_DEPTH         8U
#define TICK_MS             100U
#define LOCK_MS             100U
#define K1_SETTLE_MS        20U          /* SPDT contact-transit budget */
#define V_BATT_FLOOR_MV     1000U        /* below this Vbus is "no battery", */
                                         /* not a low-charge reading */

#define FAN_DUTY_OFF        0U
#define FAN_DUTY_LOW        30U
#define FAN_DUTY_MED        60U
#define FAN_DUTY_HIGH       100U

typedef struct {
    bool    k1;
    bool    bleed;
    uint8_t fan_duty;
} outputs_t;

typedef struct {
    charge_state_t state;
    charge_state_t prev_state;
    fault_reason_t fault;
    /* TEST entered from CHARGING returns to CHARGING; from TRICKLE or
     * IDLE-driven cycles returns to TRICKLE. Captured on entry into
     * TEST_PREP; read on TEST exit. */
    charge_state_t test_return_to;
    bool           just_entered;
    uint32_t       state_entry_ms;
    uint32_t       charge_time_ms;
    uint32_t       v0_mv;
    /* Tracked outputs — reflected into the snapshot for display. */
    bool           k1_on;
    bool           bleed_on;
    uint8_t        fan_duty_pct;
    /* Debounced battery-present signal. The Vbus heuristic here is a
     * placeholder; real detection cross-checks current draw against
     * the commanded bleed/charge state once those land. */
    bool           battery_present;
    uint8_t        present_count;
    uint8_t        absent_count;
} fsm_t;

static fsm_t              s_fsm;
static charge_snapshot_t  s_snap;
static osMutexId_t        s_snap_mutex;
static osMessageQueueId_t s_evt_queue;
static osThreadId_t       s_task;

static const osMutexAttr_t s_snap_mutex_attr = {
    .name      = "charge_snap",
    .attr_bits = osMutexPrioInherit,
};

static const osThreadAttr_t s_task_attr = {
    .name       = "charge_state",
    .priority   = (osPriority_t)TASK_PRIORITY,
    .stack_size = TASK_STACK_WORDS * 4U,
};

/* Per-state output rows. FAULT's row is unused — see FAULT_OUTPUTS. */
static const outputs_t STATE_OUTPUTS[CHARGE_STATE_COUNT] = {
    [CHARGE_STATE_INIT]           = { true,  false, FAN_DUTY_OFF  },
    [CHARGE_STATE_IDLE]           = { true,  false, FAN_DUTY_LOW  },
    [CHARGE_STATE_CHARGE_PREPARE] = { true,  false, FAN_DUTY_LOW  },
    [CHARGE_STATE_CHARGING]       = { false, false, FAN_DUTY_MED  },
    [CHARGE_STATE_TEST_PREP]      = { true,  false, FAN_DUTY_LOW  },
    [CHARGE_STATE_TEST]           = { true,  true,  FAN_DUTY_HIGH },
    [CHARGE_STATE_TRICKLE]        = { false, false, FAN_DUTY_LOW  },
    [CHARGE_STATE_MONITOR]        = { false, false, FAN_DUTY_LOW  },
    [CHARGE_STATE_FAULT]          = { true,  false, FAN_DUTY_LOW  },
};

static const outputs_t FAULT_OUTPUTS[FAULT_COUNT] = {
    [FAULT_NONE]          = { true, false, FAN_DUTY_LOW  },
    [FAULT_CHRG_TIMEOUT]  = { true, true,  FAN_DUTY_HIGH },
    [FAULT_OVERCURRENT]   = { true, false, FAN_DUTY_LOW  },
    [FAULT_OVERTEMP]      = { true, false, FAN_DUTY_HIGH },
    [FAULT_BATT_REJECTED] = { true, true,  FAN_DUTY_HIGH },
    [FAULT_SAFETY_OTHER]  = { true, false, FAN_DUTY_LOW  },
};

static const char *const STATE_LABELS[CHARGE_STATE_COUNT] = {
    [CHARGE_STATE_INIT]           = "init",
    [CHARGE_STATE_IDLE]           = "idle",
    [CHARGE_STATE_CHARGE_PREPARE] = "prep",
    [CHARGE_STATE_CHARGING]       = "charge",
    [CHARGE_STATE_TEST_PREP]      = "test prep",
    [CHARGE_STATE_TEST]           = "test",
    [CHARGE_STATE_TRICKLE]        = "trickle",
    [CHARGE_STATE_MONITOR]        = "monitor",
    [CHARGE_STATE_FAULT]          = "fault",
};

static const char *const FAULT_LABELS[FAULT_COUNT] = {
    [FAULT_NONE]          = "none",
    [FAULT_CHRG_TIMEOUT]  = "chrg timeout",
    [FAULT_OVERCURRENT]   = "overcurrent",
    [FAULT_OVERTEMP]      = "overtemp",
    [FAULT_BATT_REJECTED] = "batt reject",
    [FAULT_SAFETY_OTHER]  = "safety",
};

/* --- helpers ----------------------------------------------------- */

static outputs_t outputs_for_state(charge_state_t s, fault_reason_t reason)
{
    if (s == CHARGE_STATE_FAULT) {
        if (reason < FAULT_COUNT) {
            return FAULT_OUTPUTS[reason];
        }
        return FAULT_OUTPUTS[FAULT_NONE];
    }
    if (s < CHARGE_STATE_COUNT) {
        return STATE_OUTPUTS[s];
    }
    const outputs_t safe = { true, false, FAN_DUTY_OFF };
    return safe;
}

/* Drive the outputs, enforcing the SPDT sequencing invariant from
 * the header: bleed FET must be OFF before K1 moves, otherwise the
 * break-before-make transit hot-switches ~800 mA across the contact. */
static void apply_outputs(outputs_t out)
{
    bleed_disable();

    if (out.k1) {
        relay_enable();
    } else {
        relay_disable();
    }

    /* Give the relay time to settle before re-asserting bleed. The
     * delay only matters when K1 actually moved. */
    if (out.k1 != s_fsm.k1_on) {
        osDelay(K1_SETTLE_MS);
    }

    if (out.bleed) {
        bleed_enable();
    }

    fan_set_duty(out.fan_duty);

    s_fsm.k1_on        = out.k1;
    s_fsm.bleed_on     = out.bleed;
    s_fsm.fan_duty_pct = out.fan_duty;
}

static void enter_state(charge_state_t next, uint32_t now_ms)
{
    s_fsm.prev_state     = s_fsm.state;
    s_fsm.state          = next;
    s_fsm.state_entry_ms = now_ms;
    apply_outputs(outputs_for_state(next, s_fsm.fault));
}

/* TODO(telemetry): pulled from monitor_state today because the
 * ina238_task is the only producer in the tree. When the controller
 * starts owning the INA238 read pump directly, this swaps to a
 * private snapshot updated inline at the top of each tick. */
static void read_telemetry(uint32_t *vbus_mv, int32_t *current_ma, int32_t *tdie_mc)
{
    monitor_snapshot_t ms;
    monitor_state_get(&ms);
    *vbus_mv    = ms.vbus_mv;
    *current_ma = ms.current_ma;
    *tdie_mc    = ms.tdie_mc;
}

/* Debounced battery-present signal. The Vbus > floor heuristic is a
 * placeholder — real detection has to characterize the charger's
 * no-load output and cross-check against current-vs-commanded. Real
 * version lands when the charger bench data does. */
static void update_battery_present(uint32_t vbus_mv)
{
    if (vbus_mv > V_BATT_FLOOR_MV) {
        if (s_fsm.present_count < T_PRESENT_SAMPLES) {
            s_fsm.present_count++;
        }
        s_fsm.absent_count = 0;
    } else {
        if (s_fsm.absent_count < T_PRESENT_SAMPLES) {
            s_fsm.absent_count++;
        }
        s_fsm.present_count = 0;
    }

    if (s_fsm.present_count >= T_PRESENT_SAMPLES) {
        s_fsm.battery_present = true;
    } else if (s_fsm.absent_count >= T_PRESENT_SAMPLES) {
        s_fsm.battery_present = false;
    }
}

static uint32_t elapsed_s(uint32_t now_ms)
{
    return (now_ms - s_fsm.state_entry_ms) / 1000U;
}

static void publish_snapshot(uint32_t now_ms,
                             uint32_t vbus_mv,
                             int32_t  current_ma,
                             int32_t  tdie_mc)
{
    if (s_snap_mutex == NULL) {
        return;
    }
    if (osMutexAcquire(s_snap_mutex, LOCK_MS) != osOK) {
        return;
    }
    s_snap.state           = s_fsm.state;
    s_snap.fault           = s_fsm.fault;
    s_snap.vbus_mv         = vbus_mv;
    s_snap.current_ma      = current_ma;
    s_snap.tdie_mc         = tdie_mc;
    s_snap.ntc_mc          = INT32_MIN;
    s_snap.k1_on           = s_fsm.k1_on;
    s_snap.bleed_on        = s_fsm.bleed_on;
    s_snap.battery_present = s_fsm.battery_present;
    s_snap.fan_duty_pct    = s_fsm.fan_duty_pct;
    s_snap.time_in_state_s = elapsed_s(now_ms);
    s_snap.charge_time_s   = s_fsm.charge_time_ms / 1000U;
    s_snap.v0_mv           = s_fsm.v0_mv;
    s_snap.timestamp_ms    = now_ms;
    (void)osMutexRelease(s_snap_mutex);
}

/* --- per-state handlers -----------------------------------------
 *
 * Each handler returns the next state (or the current state if no
 * transition fires). Telemetry-driven thresholds (V_CHARGE_RISE_MV,
 * I_OC_MA, T_TAIL_S avg-current termination) are intentionally NOT
 * wired yet — those land per-state in follow-up commits so each one
 * gets focused test coverage. The skeleton wires only the
 * transitions that can be exercised without telemetry: INIT->IDLE,
 * cmd_enter/exit_monitor, settle-timer transitions, and the
 * battery-removed exits.
 */

static charge_state_t handle_init(const charge_event_t *evt,
                                  uint32_t now_ms)
{
    (void)evt;
    (void)now_ms;
    return CHARGE_STATE_IDLE;
}

static charge_state_t handle_idle(const charge_event_t *evt,
                                  uint32_t now_ms,
                                  uint32_t vbus_mv)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (s_fsm.battery_present) {
        /* V0 is "battery voltage at the moment we decided to start
         * charging" — captured here rather than in handle_charge_prepare
         * because just_entered is cleared before per-state dispatch. */
        s_fsm.v0_mv = vbus_mv;
        return CHARGE_STATE_CHARGE_PREPARE;
    }
    return CHARGE_STATE_IDLE;
}

static charge_state_t handle_charge_prepare(const charge_event_t *evt,
                                            uint32_t now_ms)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (!s_fsm.battery_present) {
        return CHARGE_STATE_IDLE;
    }
    /* Pretest pulse + ΔV sag check lands in a follow-up commit; the
     * skeleton transitions straight to CHARGING once we're here. */
    return CHARGE_STATE_CHARGING;
}

static charge_state_t handle_charging(const charge_event_t *evt,
                                      uint32_t now_ms)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (evt->id == EVT_CMD_START_TEST) {
        s_fsm.test_return_to = CHARGE_STATE_CHARGING;
        return CHARGE_STATE_TEST_PREP;
    }
    /* Charge-timeout, OC, and tail-current termination follow. */
    return CHARGE_STATE_CHARGING;
}

static charge_state_t handle_test_prep(const charge_event_t *evt,
                                       uint32_t now_ms)
{
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (!s_fsm.battery_present) {
        return CHARGE_STATE_IDLE;
    }
    if (elapsed_s(now_ms) >= T_TEST_SETTLE_S) {
        return CHARGE_STATE_TEST;
    }
    return CHARGE_STATE_TEST_PREP;
}

static charge_state_t handle_test(const charge_event_t *evt,
                                  uint32_t now_ms)
{
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (!s_fsm.battery_present) {
        return CHARGE_STATE_IDLE;
    }
    if (elapsed_s(now_ms) >= T_TEST_S) {
        return (s_fsm.test_return_to == CHARGE_STATE_CHARGING)
                ? CHARGE_STATE_CHARGING
                : CHARGE_STATE_TRICKLE;
    }
    return CHARGE_STATE_TEST;
}

static charge_state_t handle_trickle(const charge_event_t *evt,
                                     uint32_t now_ms)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_ENTER_MONITOR) {
        return CHARGE_STATE_MONITOR;
    }
    if (evt->id == EVT_CMD_START_TEST) {
        s_fsm.test_return_to = CHARGE_STATE_TRICKLE;
        return CHARGE_STATE_TEST_PREP;
    }
    if (evt->id == EVT_BATTERY_REMOVED || !s_fsm.battery_present) {
        return CHARGE_STATE_IDLE;
    }
    return CHARGE_STATE_TRICKLE;
}

static charge_state_t handle_monitor(const charge_event_t *evt,
                                     uint32_t now_ms)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_EXIT_MONITOR) {
        return CHARGE_STATE_IDLE;
    }
    return CHARGE_STATE_MONITOR;
}

static charge_state_t handle_fault(const charge_event_t *evt,
                                   uint32_t now_ms,
                                   uint32_t vbus_mv)
{
    (void)now_ms;
    if (evt->id == EVT_CMD_RESET_FAULT) {
        s_fsm.fault = FAULT_NONE;
        return CHARGE_STATE_IDLE;
    }
    /* Battery-removed exit covers the K1-can't-energize case from
     * the header's "Hardware quirk" section: if the pack is yanked,
     * the drain path goes inert and we'd otherwise latch on the
     * charger's no-load voltage. */
    if (!s_fsm.battery_present) {
        s_fsm.fault = FAULT_NONE;
        return CHARGE_STATE_IDLE;
    }
    if (vbus_mv > 0U && vbus_mv < V_SAFE_MV) {
        s_fsm.fault = FAULT_NONE;
        return CHARGE_STATE_IDLE;
    }
    return CHARGE_STATE_FAULT;
}

/* --- public ------------------------------------------------------ */

void charge_state_init(void)
{
    if (s_snap_mutex == NULL) {
        s_snap_mutex = osMutexNew(&s_snap_mutex_attr);
    }
    if (s_evt_queue == NULL) {
        s_evt_queue = osMessageQueueNew(QUEUE_DEPTH,
                                        sizeof(charge_event_t),
                                        NULL);
    }
    (void)memset(&s_fsm, 0, sizeof(s_fsm));
    (void)memset(&s_snap, 0, sizeof(s_snap));
    s_fsm.state          = CHARGE_STATE_INIT;
    s_fsm.test_return_to = CHARGE_STATE_TRICKLE;
    s_fsm.just_entered   = true;
    s_snap.ntc_mc        = INT32_MIN;
}

void charge_state_get(charge_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_snap_mutex == NULL) {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    if (osMutexAcquire(s_snap_mutex, LOCK_MS) != osOK) {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_snap;
    (void)osMutexRelease(s_snap_mutex);
}

const char *charge_state_label(charge_state_t s)
{
    if (s < CHARGE_STATE_COUNT && STATE_LABELS[s] != NULL) {
        return STATE_LABELS[s];
    }
    return "?";
}

const char *fault_reason_label(fault_reason_t f)
{
    if (f < FAULT_COUNT && FAULT_LABELS[f] != NULL) {
        return FAULT_LABELS[f];
    }
    return "?";
}

osStatus_t charge_state_post(charge_event_id_t id, uint32_t payload)
{
    if (s_evt_queue == NULL) {
        return osError;
    }
    charge_event_t evt = { .id = id, .payload = payload };
    return osMessageQueuePut(s_evt_queue, &evt, 0U, 0U);
}

void charge_state_safety_fault(fault_reason_t reason)
{
    /* Immediate hardware safe-state — covers the gap before the state
     * machine's next tick picks up the posted event and re-asserts
     * the per-reason outputs from FAULT_OUTPUTS. K1 on isolates the
     * charger, bleed off so we don't add load until the policy layer
     * decides whether to drain. */
    bleed_disable();
    relay_enable();
    fan_set_duty(FAN_DUTY_LOW);

    (void)charge_state_post(EVT_SAFETY_FAULT, (uint32_t)reason);
}

void charge_state_tick(uint32_t now_ms, const charge_event_t *evt)
{
    const charge_event_t local_tick = { .id = EVT_TICK, .payload = 0 };
    const charge_event_t *e = (evt != NULL) ? evt : &local_tick;

    /* Boot path only: charge_state_init() leaves us in INIT with
     * just_entered=true and no apply_outputs yet (init can't safely
     * call it — the underlying GPIO/PWM are owned by direct_io and
     * may not be ready at osKernelInit time). The first tick after
     * the task starts drains the flag and asserts safe outputs.
     * Subsequent transitions go through enter_state() which calls
     * apply_outputs directly. */
    if (s_fsm.just_entered) {
        apply_outputs(outputs_for_state(s_fsm.state, s_fsm.fault));
        s_fsm.just_entered = false;
    }

    uint32_t vbus_mv    = 0;
    int32_t  current_ma = 0;
    int32_t  tdie_mc    = 0;
    read_telemetry(&vbus_mv, &current_ma, &tdie_mc);
    update_battery_present(vbus_mv);

    if (s_fsm.state == CHARGE_STATE_CHARGING) {
        s_fsm.charge_time_ms += TICK_MS;
    } else if (s_fsm.state == CHARGE_STATE_IDLE) {
        s_fsm.charge_time_ms = 0;
    }

    charge_state_t next;

    if (e->id == EVT_SAFETY_FAULT) {
        fault_reason_t r = (fault_reason_t)e->payload;
        if (r >= FAULT_COUNT) {
            r = FAULT_SAFETY_OTHER;
        }
        s_fsm.fault = r;
        next = CHARGE_STATE_FAULT;
    } else if (e->id == EVT_CMD_FORCE_IDLE &&
               s_fsm.state != CHARGE_STATE_FAULT &&
               s_fsm.state != CHARGE_STATE_IDLE) {
        s_fsm.fault = FAULT_NONE;
        next = CHARGE_STATE_IDLE;
    } else {
        switch (s_fsm.state) {
            case CHARGE_STATE_INIT:
                next = handle_init(e, now_ms);
                break;
            case CHARGE_STATE_IDLE:
                next = handle_idle(e, now_ms, vbus_mv);
                break;
            case CHARGE_STATE_CHARGE_PREPARE:
                next = handle_charge_prepare(e, now_ms);
                break;
            case CHARGE_STATE_CHARGING:
                next = handle_charging(e, now_ms);
                break;
            case CHARGE_STATE_TEST_PREP:
                next = handle_test_prep(e, now_ms);
                break;
            case CHARGE_STATE_TEST:
                next = handle_test(e, now_ms);
                break;
            case CHARGE_STATE_TRICKLE:
                next = handle_trickle(e, now_ms);
                break;
            case CHARGE_STATE_MONITOR:
                next = handle_monitor(e, now_ms);
                break;
            case CHARGE_STATE_FAULT:
                next = handle_fault(e, now_ms, vbus_mv);
                break;
            case CHARGE_STATE_COUNT:
            default:
                next = CHARGE_STATE_INIT;
                break;
        }
    }

    if (next != s_fsm.state) {
        enter_state(next, now_ms);
    }

    publish_snapshot(now_ms, vbus_mv, current_ma, tdie_mc);
}

/* GCOVR_EXCL_START
 *
 * Task body — `for(;;)` around osMessageQueueGet + tick. Untestable
 * on the host (the infinite loop never returns; the tick body it
 * calls is covered by test_charge_state.c directly).
 */
static void charge_task_body(void *argument)
{
    (void)argument;
    for (;;) {
        charge_event_t evt;
        const osStatus_t got = osMessageQueueGet(s_evt_queue, &evt, NULL, TICK_MS);
        if (got != osOK) {
            evt.id      = EVT_TICK;
            evt.payload = 0;
        }
        charge_state_tick(osKernelGetTickCount(), &evt);
    }
}
/* GCOVR_EXCL_STOP */

void charge_state_task_start(void)
{
    if (s_task != NULL) {
        return;
    }
    charge_state_init();
    s_task = osThreadNew(charge_task_body, NULL, &s_task_attr);
}
