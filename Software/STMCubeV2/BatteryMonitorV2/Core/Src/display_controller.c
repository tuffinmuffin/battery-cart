/**
 * display_controller.c — see display_controller.h for the contract.
 *
 * State machine + FreeRTOS task body. The split between _tick (pure
 * state logic) and _render (calls into display_render) lets host
 * tests exercise nav + demo cycling without a real u8g2.
 *
 * Demo block is TEMP — the inject_demo_data() function below and
 * the label/serial/show_voltage rotation in build_main_ctx() get
 * ripped when a real charge controller starts feeding monitor_state.
 */

#include "display_controller.h"

#include "cdc_print.h"
#include "cmsis_os2.h"
#include "display_input.h"
#include "display_render.h"
#include "display_state.h"
#include "monitor_state.h"
#include "ssd1306.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define STARTUP_DELAY_MS         5000U
#define INIT_RETRY_MS            1000U
#define FRAME_INTERVAL_MS        200U
#define DEMO_CYCLE_MS            8000U   /* between auto-cycle steps */
#define DEMO_INACTIVITY_MS       30000U  /* idle gap before demo resumes */

#define FAULT_CODE_LEN           32U
#define FAULT_DETAIL_LEN         64U
#define FAULT_LOCK_MS            100U   /* matches monitor_state pattern */

typedef struct {
    display_view_t  view;
    uint8_t         menu_cursor;
    uint32_t        last_input_ms;
    uint32_t        last_cycle_ms;
    bool            fault_active;
    char            fault_code[FAULT_CODE_LEN];
    char            fault_detail[FAULT_DETAIL_LEN];
    uint32_t        demo_frame;     /* TEMP demo sweep counter */
} controller_state_t;

static controller_state_t s_state;

/* Fault fields (active flag + code/detail buffers) are written by a
 * future charge controller / safety monitor task and read by the
 * display task in _render. Without serialisation the reader can see a
 * torn string mid-strncpy. Mirrors monitor_state's pattern. */
static osMutexId_t s_fault_mutex;
static const osMutexAttr_t s_fault_mutex_attr = {
    .name      = "display_fault",
    .attr_bits = osMutexPrioInherit,
};

/* Menu items — placeholder labels for the design's menu structure.
 * Selecting any item today just logs to CDC; real handlers
 * (force-test trigger, NVIC_SystemReset, DFU jump, etc.) land
 * with the menu confirm sub-menu work. */
static const char *const kMenuItems[] = {
    "Force test",
    "Reset",
    "Firmware update",
    "About",
    "Status",
};
#define MENU_ITEM_COUNT  (sizeof(kMenuItems) / sizeof(kMenuItems[0]))

/* Demo cycle order — only the headline views (MAIN + ALTs) cycle
 * automatically. MENU/IDLE/FAULT/DEBUG need real triggers. */
static const display_view_t kCycleOrder[] = {
    DISPLAY_VIEW_MAIN,
    DISPLAY_VIEW_ALT_THERMAL,
    DISPLAY_VIEW_ALT_COOLING,
    DISPLAY_VIEW_ALT_BATTERY,
};
#define CYCLE_LEN  (sizeof(kCycleOrder) / sizeof(kCycleOrder[0]))

static osThreadId_t       s_task_handle;
static const osThreadAttr_t s_task_attr = {
    .name = "display",
    .priority = (osPriority_t)osPriorityNormal,
    .stack_size = 256 * 4,
};

/* --- helpers ----------------------------------------------------- */

/* Find the index of `view` in kCycleOrder, or 0 if not found. */
static uint8_t cycle_index_of(display_view_t view)
{
    for (size_t i = 0; i < CYCLE_LEN; i++) {
        if (kCycleOrder[i] == view) {
            return (uint8_t)i;
        }
    }
    return 0;
}

static void advance_cycle(uint32_t now_ms)
{
    const uint8_t idx = cycle_index_of(s_state.view);
    s_state.view = kCycleOrder[(idx + 1U) % CYCLE_LEN];
    s_state.last_cycle_ms = now_ms;
}

static void handle_event(display_event_t e, uint32_t now_ms)
{
    s_state.last_input_ms = now_ms;
    s_state.last_cycle_ms = now_ms;   /* pause demo cycle too */

    switch (s_state.view) {
        case DISPLAY_VIEW_MAIN:
        case DISPLAY_VIEW_ALT_THERMAL:
        case DISPLAY_VIEW_ALT_COOLING:
        case DISPLAY_VIEW_ALT_BATTERY:
            if (e == DISPLAY_EVENT_NEXT_SHORT) {
                advance_cycle(now_ms);
            } else if (e == DISPLAY_EVENT_NEXT_LONG) {
                s_state.view = DISPLAY_VIEW_MAIN;
            } else if (e == DISPLAY_EVENT_SELECT_SHORT) {
                s_state.view = DISPLAY_VIEW_MENU;
                s_state.menu_cursor = 0;
            }
            break;

        case DISPLAY_VIEW_MENU:
            if (e == DISPLAY_EVENT_NEXT_SHORT) {
                s_state.menu_cursor =
                    (uint8_t)((s_state.menu_cursor + 1U) % MENU_ITEM_COUNT);
            } else if (e == DISPLAY_EVENT_NEXT_LONG) {
                s_state.view = DISPLAY_VIEW_MAIN;
            } else if (e == DISPLAY_EVENT_SELECT_SHORT) {
                /* TODO: execute item / enter confirm sub-menu. For
                 * now just log so the wiring is observable. */
                cdc_printf("display: menu select \"%s\"\r\n",
                           kMenuItems[s_state.menu_cursor]);
            }
            break;

        case DISPLAY_VIEW_IDLE:
        case DISPLAY_VIEW_FAULT:
            /* Any event → return to MAIN. For FAULT, the controller
             * also clears the fault (long-press acknowledge). */
            if (s_state.view == DISPLAY_VIEW_FAULT &&
                e == DISPLAY_EVENT_NEXT_LONG) {
                display_controller_clear_fault();
            }
            s_state.view = DISPLAY_VIEW_MAIN;
            break;

        case DISPLAY_VIEW_DEBUG:
        default:
            /* DEBUG view is reachable only via direct injection
             * today; treat any event as "exit to MAIN". */
            s_state.view = DISPLAY_VIEW_MAIN;
            break;
    }
}

/* TEMP: synth a snapshot for demo. Real charge controller will
 * publish to monitor_state and we'll just read it. */
static void inject_demo_data(monitor_snapshot_t *snap)
{
    const uint32_t f = s_state.demo_frame;
    snap->vbus_mv      = 12000U + (f % 300U) * 10U;
    snap->current_ma   = (int32_t)((f % 400U) * 25) - 2500;
    snap->k1_on        = ((f / 25U) & 1U) != 0U;
    snap->bleed_on     = ((f / 50U) & 1U) != 0U;
    snap->fan_duty_pct = (uint32_t)((f * 2U) % 100U);
    snap->tdie_mc      = 24000 + (int32_t)((f % 200U) * 100);
}

static void build_main_ctx(display_main_ctx_t *out,
                           const monitor_snapshot_t *snap)
{
    /* Demo: rotate a synthetic state every 20 frames (4 s @ 5 Hz);
     * matches the previous display_task cycle so visual behaviour
     * is preserved. */
    static const display_state_t kDemoStates[] = {
        DISPLAY_STATE_CHARGING,
        DISPLAY_STATE_NO_BATT,
        DISPLAY_STATE_TRICKLE,
    };
    static const char *const kDemoSerials[] = {
        "S/N: AB12C",
        "S/N: -----",
        "S/N: AB12C",
    };
    const uint32_t cycle = s_state.demo_frame / 20U;
    const size_t   nstates = sizeof(kDemoStates) / sizeof(kDemoStates[0]);
    const display_state_t demo_state = kDemoStates[cycle % nstates];

    out->status_label  = display_label_for_state(demo_state);
    out->charge_time_s = s_state.demo_frame / (1000U / FRAME_INTERVAL_MS);
    out->serial        = kDemoSerials[cycle % nstates];
    out->charge_pct    = 0xFFU;                /* unknown today */
    out->show_voltage  = (cycle & 1U) == 0U;
    out->snap          = snap;
}

/* --- public API -------------------------------------------------- */

void display_controller_init(void)
{
    (void)memset(&s_state, 0, sizeof(s_state));
    s_state.view = DISPLAY_VIEW_MAIN;
    if (s_fault_mutex == NULL) {
        s_fault_mutex = osMutexNew(&s_fault_mutex_attr);
    }
}

void display_controller_set_fault(const char *code, const char *detail)
{
    if (s_fault_mutex != NULL &&
        osMutexAcquire(s_fault_mutex, FAULT_LOCK_MS) != osOK) {
        return;
    }
    s_state.fault_active = true;
    if (code != NULL) {
        (void)strncpy(s_state.fault_code, code, FAULT_CODE_LEN - 1U);
        s_state.fault_code[FAULT_CODE_LEN - 1U] = '\0';
    } else {
        s_state.fault_code[0] = '\0';
    }
    if (detail != NULL) {
        (void)strncpy(s_state.fault_detail, detail, FAULT_DETAIL_LEN - 1U);
        s_state.fault_detail[FAULT_DETAIL_LEN - 1U] = '\0';
    } else {
        s_state.fault_detail[0] = '\0';
    }
    if (s_fault_mutex != NULL) {
        (void)osMutexRelease(s_fault_mutex);
    }
}

void display_controller_clear_fault(void)
{
    if (s_fault_mutex != NULL &&
        osMutexAcquire(s_fault_mutex, FAULT_LOCK_MS) != osOK) {
        return;
    }
    s_state.fault_active = false;
    s_state.fault_code[0] = '\0';
    s_state.fault_detail[0] = '\0';
    if (s_fault_mutex != NULL) {
        (void)osMutexRelease(s_fault_mutex);
    }
}

display_view_t display_controller_view(void)
{
    return s_state.view;
}

uint8_t display_controller_menu_cursor(void)
{
    return s_state.menu_cursor;
}

bool display_controller_fault_active(void)
{
    /* bool read is single-byte on Cortex-M; lock just to keep the
     * "all fault-field access goes through the mutex" invariant
     * consistent (and so this returns false the instant the writer
     * is mid-clear, not a half-stale true). */
    if (s_fault_mutex != NULL &&
        osMutexAcquire(s_fault_mutex, FAULT_LOCK_MS) != osOK) {
        return false;
    }
    const bool active = s_state.fault_active;
    if (s_fault_mutex != NULL) {
        (void)osMutexRelease(s_fault_mutex);
    }
    return active;
}

void display_controller_tick(uint32_t now_ms)
{
    display_input_poll(now_ms);

    display_event_t e;
    while (display_input_get_event(&e)) {
        handle_event(e, now_ms);
    }

    /* Auto-cycle demo — only when nobody's pressed anything in a
     * while AND only between MAIN/ALT_* views (skip MENU/IDLE/etc.). */
    if ((uint32_t)(now_ms - s_state.last_input_ms) >= DEMO_INACTIVITY_MS &&
        (uint32_t)(now_ms - s_state.last_cycle_ms) >= DEMO_CYCLE_MS) {
        switch (s_state.view) {
            case DISPLAY_VIEW_MAIN:
            case DISPLAY_VIEW_ALT_THERMAL:
            case DISPLAY_VIEW_ALT_COOLING:
            case DISPLAY_VIEW_ALT_BATTERY:
                advance_cycle(now_ms);
                break;
            default:
                /* Don't auto-cycle out of MENU / IDLE / FAULT / DEBUG. */
                break;
        }
    }

    /* Demo sweep counter lives here (not in _render) so _render stays
     * a pure draw — a future force-refresh that calls _render twice
     * between ticks won't double-advance the demo. TEMP: rip when the
     * real producer wires up. */
    s_state.demo_frame++;
}

void display_controller_render(void)
{
    u8g2_t *u8g2 = ssd1306_u8g2();
    if (u8g2 == NULL) {
        return;
    }

    monitor_snapshot_t snap;
    monitor_state_get(&snap);
    inject_demo_data(&snap);   /* TEMP — rip when real producer lands */

    /* Snapshot the fault state under the lock into render-task-local
     * scratch so the actual draw runs without holding the lock and
     * a producer's strncpy can't race us mid-build. */
    bool effective_fault = false;
    char fault_code[FAULT_CODE_LEN]   = {0};
    char fault_detail[FAULT_DETAIL_LEN] = {0};
    if (s_fault_mutex == NULL ||
        osMutexAcquire(s_fault_mutex, FAULT_LOCK_MS) == osOK) {
        effective_fault = s_state.fault_active;
        if (effective_fault) {
            (void)memcpy(fault_code,   s_state.fault_code,   FAULT_CODE_LEN);
            (void)memcpy(fault_detail, s_state.fault_detail, FAULT_DETAIL_LEN);
        }
        if (s_fault_mutex != NULL) {
            (void)osMutexRelease(s_fault_mutex);
        }
    }

    /* Fault override picks the rendered view, but doesn't clobber
     * s_state.view (so we return to the user's view when the fault
     * is acked). */
    const display_view_t effective_view =
        effective_fault ? DISPLAY_VIEW_FAULT : s_state.view;

    display_ctx_t ctx;
    ctx.view = effective_view;

    switch (effective_view) {
        case DISPLAY_VIEW_MAIN:
            build_main_ctx(&ctx.u.main, &snap);
            break;
        case DISPLAY_VIEW_ALT_THERMAL:
            ctx.u.thermal.snap = &snap;
            break;
        case DISPLAY_VIEW_ALT_COOLING:
            ctx.u.cooling.snap = &snap;
            break;
        case DISPLAY_VIEW_ALT_BATTERY:
            /* TODO: use real NFC serial once PN532 lands. */
            ctx.u.battery.serial = "S/N: AB12C";
            break;
        case DISPLAY_VIEW_MENU:
            ctx.u.menu.cursor = s_state.menu_cursor;
            ctx.u.menu.count  = (uint8_t)MENU_ITEM_COUNT;
            ctx.u.menu.items  = kMenuItems;
            break;
        case DISPLAY_VIEW_IDLE:
            ctx.u.idle.frame = s_state.demo_frame;
            break;
        case DISPLAY_VIEW_FAULT:
            ctx.u.fault.code   = fault_code;
            ctx.u.fault.detail = fault_detail[0] != '\0' ? fault_detail : NULL;
            break;
        case DISPLAY_VIEW_DEBUG:
        default:
            /* Stub views consume no ctx fields today. */
            break;
    }

    display_render(u8g2, &ctx);
}

static void controller_task_body(void *argument)
{
    (void)argument;

    /* Same CDC enumeration grace as the prior display_task — without
     * it any startup log lines vanish before a terminal is attached. */
    osDelay(STARTUP_DELAY_MS);

    /* Probe + init loop. Retry on failure rather than wedging the
     * task — the panel may not be electrically settled at boot. */
    int retry = 0;
    ssd1306_status_t st;
    while ((st = ssd1306_init(SSD1306_I2C_ADDR_DEFAULT)) != SSD1306_OK) {
        cdc_printf("display: init failed st=%d at 0x%02X, retry %d\r\n",
                   (int)st, (unsigned)SSD1306_I2C_ADDR_DEFAULT, retry++);
        osDelay(INIT_RETRY_MS);
    }
    cdc_printf("display: init OK at 0x%02X\r\n",
               (unsigned)SSD1306_I2C_ADDR_DEFAULT);

    /* Initial input timestamp — first frame should NOT auto-cycle
     * immediately; user gets 30 s of idle before the demo kicks in. */
    s_state.last_input_ms = osKernelGetTickCount();

    for (;;) {
        const uint32_t now_ms = osKernelGetTickCount();
        display_controller_tick(now_ms);
        display_controller_render();
        osDelay(FRAME_INTERVAL_MS);
    }
}

void display_controller_start(void)
{
    /* Idempotent: skip if the task already exists. Boot-only path
     * today, but a future restart-on-fault or watchdog flow could
     * land here twice; without the guard the second osThreadNew
     * would leak the prior handle (and waste another 1 KB stack). */
    if (s_task_handle != NULL) {
        return;
    }
    display_controller_init();
    display_input_init();
    s_task_handle = osThreadNew(controller_task_body, NULL, &s_task_attr);
}
