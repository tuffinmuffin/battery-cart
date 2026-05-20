/**
 * cdc_print.c — see cdc_print.h for the contract.
 *
 * The mutex guards both the shared format buffer AND the act of writing
 * to USB-CDC, so concurrent tasks' lines stay intact (no interleaving)
 * and one task's partial write can't be clobbered by another's format.
 */

#include "cdc_print.h"

#include "cmsis_os2.h"
#include "tusb.h"

#include <stdio.h>
#include <stdint.h>

/* 256 bytes covers the longest line in current use (the i2c bus scan can
 * approach 200 chars when many addresses respond) with margin. Bump if a
 * future format string would truncate. */
#define CDC_PRINT_BUFSIZE 256

/* Hard cap on how long a caller will wait for the mutex. 500 ms is generous
 * for a 1 Hz logger but short enough that a stuck task surfaces quickly. */
#define CDC_PRINT_LOCK_MS 500U

static char s_buf[CDC_PRINT_BUFSIZE];
static osMutexId_t s_mutex;

static const osMutexAttr_t s_mutex_attr = {
    .name = "cdc_print",
    .attr_bits = osMutexPrioInherit,
};

void cdc_print_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = osMutexNew(&s_mutex_attr);
    }
}

void cdc_write(const void *buf, size_t len)
{
    if (!tud_cdc_connected() || len == 0 || buf == NULL) {
        return;
    }
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;
    while (written < len) {
        uint32_t n = tud_cdc_write(p + written, (uint32_t)(len - written));
        written += (size_t)n;
        tud_cdc_write_flush();
        if (written < len) {
            /* FIFO full — yield so the USB device task can drain it. */
            osDelay(1);
        }
    }
}

int cdc_vprintf(const char *fmt, va_list ap)
{
    /* Defensive guard: vsnprintf dereferences `fmt` without a NULL check, so
     * a stray cdc_printf(NULL) - easy to hit via a misbehaving format-string
     * helper - would crash on target. Cheaper to short-circuit here than to
     * debug a wild dereference after the fact. */
    if (fmt == NULL) {
        return -1;
    }
    if (s_mutex == NULL || osMutexAcquire(s_mutex, CDC_PRINT_LOCK_MS) != osOK) {
        return -1;
    }
    int n = vsnprintf(s_buf, sizeof(s_buf), fmt, ap);
    if (n > 0) {
        /* vsnprintf returns the formatted length even if truncated. Cap to
         * the actual buffer fill so we don't read past the null. */
        size_t to_write = ((size_t)n < sizeof(s_buf)) ? (size_t)n
                                                      : sizeof(s_buf) - 1U;
        cdc_write(s_buf, to_write);
    }
    (void)osMutexRelease(s_mutex);
    return n;
}

int cdc_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = cdc_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* --- libc retargeting ----------------------------------------------------
 *
 * Override the weak _write in syscalls.c so the whole stdio family — printf,
 * puts, putchar, fputs, fprintf(stderr, ...) — flows through USB-CDC.
 *
 * Both stdout (fd=1) and stderr (fd=2) route to CDC; we don't filter by fd
 * because there's only one transport on this board. We take the same mutex
 * cdc_printf uses so concurrent printf() calls from different tasks don't
 * interleave bytes inside a single line. If init hasn't run (very early
 * boot), drop silently and report success — never let stdio crash the boot.
 *
 * Tagged `used` to defeat aggressive LTO from dropping a definition whose
 * symbol is only referenced through a libc weak alias. */

__attribute__((used))
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- picolibc retarget hook; name is mandated by libc. */
int _write(int file, char *ptr, int len)
{
    (void)file;
    if (ptr == NULL || len <= 0) {
        return 0;
    }
    if (s_mutex == NULL) {
        /* Pre-init: drop silently rather than blocking on a NULL mutex. */
        return len;
    }
    if (osMutexAcquire(s_mutex, CDC_PRINT_LOCK_MS) != osOK) {
        return 0;
    }
    cdc_write(ptr, (size_t)len);
    (void)osMutexRelease(s_mutex);
    return len;
}
