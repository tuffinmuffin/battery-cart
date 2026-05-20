/**
 * stack_measure.c — see stack_measure.h for the contract and caveats.
 *
 * Linux/pthread implementation: stacks grow DOWN, so we fill the lowest
 * addresses with sentinel bytes and scan from the low end upward for the
 * first byte the pthread overwrote. The position of that first dirty byte
 * relative to the buffer end gives us "bytes used".
 *
 * The whole helper is gated on `__linux__` because pthread_attr_setstack
 * isn't portable. Other hosts get a stub that prints "not supported" and
 * returns 0 so tests don't have to ifdef.
 */

#include "stack_measure.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <pthread.h>
#include <limits.h>  /* PTHREAD_STACK_MIN */

#define SENTINEL_BYTE 0xA5

typedef struct {
    stack_measure_fn func;
    void *arg;
} thread_ctx_t;

static void *thread_entry(void *raw)
{
    thread_ctx_t *ctx = raw;
    ctx->func(ctx->arg);
    return NULL;
}

size_t stack_measure_run(const char *name,
                         stack_measure_fn func,
                         void *arg,
                         size_t stack_size)
{
    if (stack_size < (size_t)PTHREAD_STACK_MIN) {
        stack_size = (size_t)PTHREAD_STACK_MIN;
    }

    /* pthread requires stack base to be page-aligned. 4 KB is universal. */
    void *stack_buf = NULL;
    if (posix_memalign(&stack_buf, 4096, stack_size) != 0 || stack_buf == NULL) {
        fprintf(stderr, "STACK %s: alloc failed (%zu bytes)\n",
                name, stack_size);
        return 0;
    }
    memset(stack_buf, SENTINEL_BYTE, stack_size);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, stack_buf, stack_size);

    thread_ctx_t ctx = { .func = func, .arg = arg };
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, thread_entry, &ctx);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        free(stack_buf);
        fprintf(stderr, "STACK %s: pthread_create failed (%d)\n", name, rc);
        return 0;
    }
    pthread_join(tid, NULL);
    pthread_attr_destroy(&attr);

    /* Scan from the low end (where the deepest stack write would land) for
     * the first non-sentinel byte. Index of that byte = unused trailing
     * sentinel bytes; bytes used = stack_size - index. */
    const uint8_t *bytes = stack_buf;
    size_t first_dirty = 0;
    while (first_dirty < stack_size && bytes[first_dirty] == SENTINEL_BYTE) {
        first_dirty++;
    }
    size_t used = stack_size - first_dirty;

    free(stack_buf);

    double pct = (used * 100.0) / (double)stack_size;
    printf("STACK %s: %zu / %zu bytes (%.1f%%)\n", name, used, stack_size, pct);
    return used;
}

#else  /* non-Linux host */

size_t stack_measure_run(const char *name,
                         stack_measure_fn func,
                         void *arg,
                         size_t stack_size)
{
    (void)func; (void)arg; (void)stack_size;
    fprintf(stderr, "STACK %s: not supported on this host\n", name);
    return 0;
}

#endif
