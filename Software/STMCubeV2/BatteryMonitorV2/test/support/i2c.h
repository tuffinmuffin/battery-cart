/**
 * test/support/i2c.h — host-test stub replacing Core/Inc/i2c.h.
 *
 * The real header pulls in stm32c0xx_hal which doesn't parse on host gcc.
 * Here we provide just enough surface for ina238.c and i2c_bus.c to compile
 * and be mocked by CMock:
 *   - Opaque I2C_HandleTypeDef (size matters only because CMock memcmps it)
 *   - HAL_StatusTypeDef enum
 *   - HAL I2C error codes and a constant for I2C_MEMADD_SIZE_8BIT
 *   - HAL_I2C_Mem_{Read,Write}_DMA + HAL_I2C_GetError prototypes
 *
 * Storage for hi2c1/hi2c2 lives in test/support/test_globals.c.
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

typedef struct __I2C_HandleTypeDef {
    uint8_t _id;
} I2C_HandleTypeDef;

typedef enum {
    HAL_OK    = 0,
    HAL_ERROR = 1,
    HAL_BUSY  = 2,
    HAL_TIMEOUT = 3,
} HAL_StatusTypeDef;

#define HAL_I2C_ERROR_NONE 0U

#define I2C_MEMADD_SIZE_8BIT  0x00000001U

extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;

HAL_StatusTypeDef HAL_I2C_Mem_Read_DMA(I2C_HandleTypeDef *hi2c,
                                       uint16_t DevAddress,
                                       uint16_t MemAddress,
                                       uint16_t MemAddSize,
                                       uint8_t *pData,
                                       uint16_t Size);

HAL_StatusTypeDef HAL_I2C_Mem_Write_DMA(I2C_HandleTypeDef *hi2c,
                                        uint16_t DevAddress,
                                        uint16_t MemAddress,
                                        uint16_t MemAddSize,
                                        uint8_t *pData,
                                        uint16_t Size);

uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *hi2c);

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c,
                                        uint16_t DevAddress,
                                        uint32_t Trials,
                                        uint32_t Timeout);

#endif /* I2C_H */
