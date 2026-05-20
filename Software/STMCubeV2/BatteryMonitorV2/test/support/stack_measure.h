/**
 * stack_measure.h — host-test helper to estimate worst-case stack usage
 * of a function under test.
 *
 * Mechanism: allocate a dedicated stack region, prefill it with a sentinel
 * pattern (0xA5), run the function on that stack via pthread, then scan
 * upward from the low address for the first untouched byte to compute the
 * high-water mark.
 *
 * Caveats (read these or be misled):
 *   - These numbers are HOST gcc frames, not ARM/Thumb frames. They're
 *     1.3–2× larger than what arm-clang produces for the same code due to
 *     SysV ABI, 64-bit pointer width, and SIMD/general-purpose register
 *     save areas. Use them for regression detection ("did this PR balloon
 *     function X's stack?"), not for absolute budgeting.
 *   - pthread setup on glibc eagerly dirties ~6 KB of the stack (TLS init,
 *     signal masks, etc.) BEFORE your function ever runs. Always pair
 *     stack_measure_run() of the function under test with a
 *     stack_measure_run() of an empty stub, and assert on the delta. Small
 *     functions (anything well under 6 KB) will report delta == 0; that's
 *     correct — they fit within pthread's existing scratch region. The
 *     tool catches regressions that push the high-water mark *past* the
 *     baseline, e.g. a newly-added 4 KB local buffer.
 *   - The function must be self-contained: any CMock expectations are set
 *     up in the calling thread but consumed in the measured pthread.
 *     CMock state is process-global so this works, but don't pthread_join
 *     after the measurement returns without restoring CMock state.
 */

#ifndef STACK_MEASURE_H
#define STACK_MEASURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Function-under-test signature. Use the void* to pass any context you need. */
typedef void (*stack_measure_fn)(void *arg);

/* Run `func(arg)` on an isolated, sentinel-filled stack of `stack_size`
 * bytes; print `STACK <name>: <used>/<total> bytes (<pct>%)` to stdout.
 *
 * stack_size must be ≥ PTHREAD_STACK_MIN (typically 16 KB on Linux). The
 * helper aligns the allocation to a 4 KB boundary as pthread requires.
 *
 * Returns the high-water mark in bytes (used), or 0 on allocation failure.
 */
size_t stack_measure_run(const char *name,
                         stack_measure_fn func,
                         void *arg,
                         size_t stack_size);

#ifdef __cplusplus
}
#endif

#endif /* STACK_MEASURE_H */
