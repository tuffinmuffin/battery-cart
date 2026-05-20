/**
 * i2c_bus.c — see i2c_bus.h for the rationale.
 *
 * Owns the FreeRTOS primitives that serialise access to I2C1 and signal DMA
 * completion to the calling task. Overrides the weak HAL callbacks so device
 * drivers don't fight over them.
 *
 * Notes on the HAL callbacks:
 *   - HAL_I2C_Mem_*_DMA fires HAL_I2C_Mem{Tx,Rx}CpltCallback at end-of-transfer.
 *   - HAL_I2C_Master_*_DMA fires HAL_I2C_Master{Tx,Rx}CpltCallback.
 *   We hook all four because the driver may use either entry point.
 *   - HAL_I2C_ErrorCallback fires on bus error / NACK / arbitration loss.
 *     We still release the semaphore so the waiting task wakes; it sees the
 *     handle's ErrorCode via HAL_I2C_GetError() if it cares.
 */

#include "i2c_bus.h"

#include "cmsis_os2.h"
#include "i2c.h"   /* hi2c1 extern */

#include <stdio.h>

static osMutexId_t s_bus_mutex;
static osSemaphoreId_t s_xfer_done;
static bool s_initialised;

static const osMutexAttr_t s_bus_mutex_attr = {
    .name = "i2c1_bus",
    .attr_bits = osMutexPrioInherit, /* avoid priority inversion vs. PN532 task */
};

static const osSemaphoreAttr_t s_xfer_done_attr = {
    .name = "i2c1_xfer",
};

void i2c_bus_init(void)
{
    if (s_initialised) {
        return;
    }
    s_bus_mutex = osMutexNew(&s_bus_mutex_attr);
    /* Binary semaphore initial count 0 → first acquire blocks until callback gives. */
    s_xfer_done = osSemaphoreNew(1U, 0U, &s_xfer_done_attr);
    s_initialised = (s_bus_mutex != NULL) && (s_xfer_done != NULL);
}

bool i2c_bus_lock(uint32_t timeout_ms)
{
    if (!s_initialised) {
        return false;
    }
    return osMutexAcquire(s_bus_mutex, timeout_ms) == osOK;
}

void i2c_bus_unlock(void)
{
    if (!s_initialised) {
        return;
    }
    (void)osMutexRelease(s_bus_mutex);
}

bool i2c_bus_wait_complete(uint32_t timeout_ms)
{
    if (!s_initialised) {
        return false;
    }
    return osSemaphoreAcquire(s_xfer_done, timeout_ms) == osOK;
}

/* --- HAL completion callbacks ----------------------------------------------
 * The HAL fires these from the I2C ISR after the transfer (DMA or interrupt-
 * mode). Release the binary semaphore so the task blocked in
 * i2c_bus_wait_complete() unblocks. osSemaphoreRelease() is ISR-safe in
 * CMSIS-RTOS2 / FreeRTOS — it routes through xSemaphoreGiveFromISR internally.
 *
 * We don't filter by hi2c instance here; only I2C1 has DMA wired, so any
 * completion we see came from a transaction we kicked off under the mutex.
 * If I2C2 ever joins, gate on `hi2c->Instance == I2C1`.
 */

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (!s_initialised) {
        return;
    }
    (void)osSemaphoreRelease(s_xfer_done);
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (!s_initialised) {
        return;
    }
    (void)osSemaphoreRelease(s_xfer_done);
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (!s_initialised) {
        return;
    }
    (void)osSemaphoreRelease(s_xfer_done);
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (!s_initialised) {
        return;
    }
    (void)osSemaphoreRelease(s_xfer_done);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    /* Wake the waiting task; it will surface the failure via HAL_I2C_GetError(). */
    if (!s_initialised) {
        return;
    }
    (void)osSemaphoreRelease(s_xfer_done);
}

/* --- Bus scan ------------------------------------------------------------
 * HAL_I2C_IsDeviceReady does a polled START + addr + ACK check with no DMA
 * involvement. We sweep 0x08..0x77 (the general-purpose 7-bit range —
 * 0x00..0x07 and 0x78..0x7F are reserved by the I2C spec). Each call
 * holds the bus for at most `timeout_ms` if the address NACKs, much less
 * if it ACKs. With 10 ms per address and a sparse bus, expect ~1.1 s total.
 *
 * Coverage waiver: scan is a bringup / debug utility, not a runtime-critical
 * path. test_i2c_bus.c still exercises it as a functional regression check,
 * but the lines don't drag on the production coverage metric. Drop the
 * waiver if scan ever becomes part of a runtime health check.
 */

/* GCOVR_EXCL_START */
int i2c_bus_scan(void *hi2c, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    /* Public API is opaque; the polled HAL call needs the typed handle. */
    I2C_HandleTypeDef *handle = (I2C_HandleTypeDef *)hi2c;
    if (!i2c_bus_lock(2000U)) {
        (void)snprintf(out, out_size, "scan: bus busy");
        return 0;
    }

    int found = 0;
    size_t written = (size_t)snprintf(out, out_size, "scan:");

    for (uint8_t addr = 0x08U; addr <= 0x77U; addr++) {
        HAL_StatusTypeDef hs = HAL_I2C_IsDeviceReady(
            handle, (uint16_t)(addr << 1), 1U, 10U);
        if (hs != HAL_OK) {
            continue;
        }
        found++;
        if (written < out_size) {
            int n = snprintf(out + written, out_size - written,
                             " 0x%02X", (unsigned)addr);
            if (n > 0) {
                written += (size_t)n;
            }
        }
    }

    if (found == 0 && written < out_size) {
        (void)snprintf(out + written, out_size - written, " (none)");
    }

    i2c_bus_unlock();
    return found;
}
/* GCOVR_EXCL_STOP */
