/**
 * i2c_bus.h — thread-safe DMA wrapper around the shared I2C1 bus.
 *
 * The I2C1 peripheral is shared between the INA238 (current/voltage monitor)
 * and a PN532 (NFC). Multiple FreeRTOS tasks may want the bus at once, so all
 * access is serialised through a mutex. DMA-mode transactions sleep the
 * caller on a binary semaphore that the HAL transfer-complete callbacks
 * release from ISR.
 *
 * Usage from a device driver:
 *
 *     if (!i2c_bus_lock(I2C_BUS_TIMEOUT_MS)) { return ERR_BUSY; }
 *     HAL_StatusTypeDef hs = HAL_I2C_Mem_Read_DMA(&hi2c1, addr<<1, reg, ...);
 *     if (hs != HAL_OK) { i2c_bus_unlock(); return ERR_HAL; }
 *     if (!i2c_bus_wait_complete(I2C_BUS_TIMEOUT_MS)) {
 *         i2c_bus_unlock(); return ERR_TIMEOUT;
 *     }
 *     i2c_bus_unlock();
 *
 * The HAL callbacks (HAL_I2C_Master{Tx,Rx}CpltCallback,
 * HAL_I2C_Mem{Tx,Rx}CpltCallback, HAL_I2C_ErrorCallback) are defined in
 * i2c_bus.c so individual drivers don't fight over the weak symbols.
 */

#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default 100 ms is comfortably longer than any INA238 transaction (a 3-byte
 * read at 400 kHz takes <200 µs) but short enough that a wedged PN532
 * surfaces quickly instead of stalling the caller forever. */
#define I2C_BUS_TIMEOUT_MS  100U

/* Create the mutex + completion semaphore. Call from MX_FREERTOS_Init
 * AFTER osKernelInitialize() (CMSIS-RTOS2 requires the kernel to exist
 * before object creation). Safe to call from a task too; first call wins. */
void i2c_bus_init(void);

/* Acquire exclusive access to I2C1. Returns false on timeout. */
bool i2c_bus_lock(uint32_t timeout_ms);

/* Release exclusive access. Must be called from the task that locked. */
void i2c_bus_unlock(void);

/* Block the caller until the in-flight DMA transaction completes (or fails).
 * Returns true on completion, false on timeout or HAL error.
 * Must only be called between i2c_bus_lock() and i2c_bus_unlock(). */
bool i2c_bus_wait_complete(uint32_t timeout_ms);

/* Sweep 7-bit addresses 0x08..0x77, format `out` as
 *   "scan: 0xNN 0xNN ..."  (or "scan: (none)" if no device ACKs).
 * Returns the count of devices found. Uses blocking HAL_I2C_IsDeviceReady
 * (no DMA), so calls don't fire the bus's completion callback. Acquires
 * the bus mutex for the duration of the scan.
 *
 * Total scan time is ~1 s when no devices respond; faster when ACKs cut
 * the per-address timeout short. Output buffer should be >= 128 bytes to
 * hold the worst case (every address present).
 *
 * The handle is opaque (`void *`) at this layer — pass `&hi2c1`. Avoiding
 * the typed handle keeps i2c.h out of this header, which lets CMock
 * generate a mock without dragging stm32c0xx_hal.h into host tests. */
int i2c_bus_scan(void *hi2c, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* I2C_BUS_H */
