/**
 * monitor_state.c — see monitor_state.h for the contract.
 *
 * Tiny: one static snapshot, one mutex, struct copy in and out. The
 * mutex pattern matches cdc_print and i2c_bus — priority-inheriting
 * so a high-priority consumer doesn't get stalled if a low-priority
 * producer is mid-update.
 */

#include "monitor_state.h"

#include "cmsis_os2.h"

#include <string.h>

/* 100 ms covers any conceivable consumer/producer call (the only work
 * inside the lock is a struct memcpy — microseconds) while still
 * surfacing a wedged task quickly. */
#define MONITOR_STATE_LOCK_MS  100U

static monitor_snapshot_t s_snap;
static osMutexId_t        s_mutex;

static const osMutexAttr_t s_mutex_attr = {
    .name      = "monitor_state",
    .attr_bits = osMutexPrioInherit,
};

void monitor_state_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = osMutexNew(&s_mutex_attr);
    }
}

void monitor_state_update(const monitor_snapshot_t *s)
{
    if (s == NULL || s_mutex == NULL) {
        return;
    }
    if (osMutexAcquire(s_mutex, MONITOR_STATE_LOCK_MS) != osOK) {
        return;
    }
    s_snap = *s;
    (void)osMutexRelease(s_mutex);
}

void monitor_state_get(monitor_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_mutex == NULL) {
        /* Pre-init or init failed — caller gets a zero snapshot rather
         * than uninitialised stack memory. */
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    if (osMutexAcquire(s_mutex, MONITOR_STATE_LOCK_MS) != osOK) {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_snap;
    (void)osMutexRelease(s_mutex);
}
