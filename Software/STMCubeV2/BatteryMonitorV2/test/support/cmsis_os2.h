/**
 * test/support/cmsis_os2.h — host-test stub for the CMSIS-RTOS2 surface used
 * by i2c_bus.c (and ina238_task.c when it lands under test).
 *
 * Just the types and functions we actually call. CMock mocks these so we can
 * assert that the driver locks the bus before talking to it, waits on the
 * completion semaphore, and unlocks afterward.
 */

#ifndef CMSIS_OS2_H
#define CMSIS_OS2_H

#include <stddef.h>
#include <stdint.h>

#define osWaitForever  0xFFFFFFFFU
#define osMutexPrioInherit (1U << 1)

typedef enum {
    osOK = 0,
    osError = -1,
    osErrorTimeout = -2,
    osErrorResource = -3,
} osStatus_t;

typedef void *osMutexId_t;
typedef void *osSemaphoreId_t;
typedef void *osThreadId_t;

typedef enum {
    osPriorityNormal = 24,
    osPriorityAboveNormal = 32,
} osPriority_t;

typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
} osMutexAttr_t;

typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
} osSemaphoreAttr_t;

typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
    void *stack_mem;
    uint32_t stack_size;
    osPriority_t priority;
} osThreadAttr_t;

typedef void (*osThreadFunc_t)(void *argument);

osMutexId_t osMutexNew(const osMutexAttr_t *attr);
osStatus_t  osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);
osStatus_t  osMutexRelease(osMutexId_t mutex_id);

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                               const osSemaphoreAttr_t *attr);
osStatus_t      osSemaphoreAcquire(osSemaphoreId_t sem_id, uint32_t timeout);
osStatus_t      osSemaphoreRelease(osSemaphoreId_t sem_id);

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
                         const osThreadAttr_t *attr);
void osDelay(uint32_t ticks);

uint32_t osKernelGetTickCount(void);

#endif /* CMSIS_OS2_H */
