/**
 * ssd1306.h — Polled-I2C2 driver for a 128x32 SSD1116/SSD1306-compatible OLED.
 *
 * Glue between u8g2 and STM32 HAL: this module owns a single u8g2_t instance,
 * the I2C2 byte-transfer callback (HAL_I2C_Master_Transmit, no DMA), and the
 * timing callback used during init. Callers grab the u8g2 pointer with
 * ssd1306_u8g2() and draw with u8g2_FirstPage / u8g2_NextPage / u8g2_DrawXxx.
 *
 * I2C2 is single-master on this board (only the OLED lives there), so no
 * mutex or DMA wait_complete is needed. If a second I2C2 device ever lands,
 * promote to an i2c2_bus.c module mirroring i2c_bus.c.
 */

#ifndef SSD1306_H
#define SSD1306_H

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSD1306_OK = 0,
    SSD1306_ERR_PROBE = 1,  /* device did not ACK its address */
    SSD1306_ERR_BUS   = 2,  /* HAL_I2C_Master_Transmit failed during init */
} ssd1306_status_t;

/* Most 128x32 SSD1306 modules tie SA0 low → 0x3C; a handful tie it high →
 * 0x3D. Caller passes the actual address to ssd1306_init() — display_task
 * tries 0x3C then 0x3D so we can bring up unknown modules without rebuilding. */
#define SSD1306_I2C_ADDR_PRIMARY    0x3CU
#define SSD1306_I2C_ADDR_ALTERNATE  0x3DU

/* Probes the display on I2C2 at the given 7-bit address, runs u8g2's init
 * sequence, clears the panel. Safe to call from a FreeRTOS task only
 * (sleeps via osDelay during init). */
ssd1306_status_t ssd1306_init(uint8_t i2c_addr_7bit);

/* Returns the module-owned u8g2 instance. NULL before init succeeds. */
u8g2_t *ssd1306_u8g2(void);

#ifdef __cplusplus
}
#endif

#endif /* SSD1306_H */
