/**
 * cdc_print.h — printf-style logger over USB-CDC.
 *
 * Owns a single mutex-protected format buffer so any FreeRTOS task can
 * cdc_printf() without allocating its own char[N] scratch. The mutex
 * also serialises whole lines, preventing two tasks' output from
 * interleaving mid-format.
 *
 * Blocking model:
 *   - If no host is connected (tud_cdc_connected() == false), bytes are
 *     dropped silently and the call returns immediately. We don't block
 *     waiting for a phantom reader.
 *   - If a host is connected but the TinyUSB TX FIFO is full, cdc_write
 *     yields (osDelay(1)) and retries until all bytes are queued.
 *
 * libc retargeting: this module also overrides the weak `_write` from
 * syscalls.c, so the standard stdio family — `printf`, `puts`, `putchar`,
 * `fprintf(stderr, ...)`, etc. — all flow through CDC with the same
 * mutex and blocking semantics as cdc_printf. Prefer cdc_printf for new
 * code (no libc-buffering surprises), but plain printf is fine too.
 *
 * Memory: 256 B shared format buffer + one mutex CB (~80 B for FreeRTOS).
 */

#ifndef CDC_PRINT_H
#define CDC_PRINT_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the print mutex. Call from MX_FREERTOS_Init after the kernel is
 * initialised. Safe to call more than once; first call wins. */
void cdc_print_init(void);

/* Blocking write of raw bytes to USB-CDC. See file header for the
 * blocking semantics. Use this for pre-formatted strings so we don't
 * pay the printf cost twice. */
void cdc_write(const void *buf, size_t len);

/* printf to USB-CDC. Formats into the shared buffer then writes via
 * cdc_write. Returns vsnprintf's return value (may exceed the internal
 * buffer length if truncated). */
int cdc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* va_list variant — handy if you want to wrap cdc_printf in a logger. */
int cdc_vprintf(const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* CDC_PRINT_H */
