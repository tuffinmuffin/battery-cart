/**
 * ssd1306.c — see ssd1306.h header doc.
 *
 * u8g2 calls our two callbacks during init and on every u8g2_NextPage flush.
 * The byte callback stages bytes into s_tx_buf between BYTE_START_TRANSFER /
 * BYTE_END_TRANSFER markers and fires one HAL_I2C_Master_Transmit at END.
 * That matches u8g2's CAD (Command/Argument/Data) protocol for I2C: each
 * transfer is one logical START..STOP burst on the wire.
 *
 * For a 128x32 SSD1306 with page-buffer `_1` setup, the largest single
 * transfer is ~130 bytes (1 control byte + 128 pixel bytes plus a couple of
 * address-set cmds inline). s_tx_buf at 256 B gives plenty of headroom.
 */

#include "ssd1306.h"

#include "i2c.h"           /* &hi2c2 */
#include "cmsis_os2.h"     /* osDelay */
#include "stm32c0xx_hal.h" /* HAL_I2C_Master_Transmit, HAL_StatusTypeDef */
#include "u8x8.h"

#include <stdbool.h>       /* bool, true, false */
#include <string.h>        /* memcpy */

#define SSD1306_TX_BUF_LEN          256U
#define SSD1306_HAL_TX_TIMEOUT_MS   100U
#define SSD1306_PROBE_TRIES         3U
#define SSD1306_PROBE_TIMEOUT_MS    50U

static u8g2_t  s_u8g2;
static bool    s_initialized;

static uint8_t s_tx_buf[SSD1306_TX_BUF_LEN];
static size_t  s_tx_len;

/* u8g2 byte callback — drives the I2C2 wire. arg_int is the byte count for
 * MSG_BYTE_SEND; arg_ptr points at the source bytes. Returns 1 on success,
 * 0 on protocol error (which u8g2 logs but otherwise tolerates). */
static uint8_t u8x8_byte_stm32_hw_i2c(u8x8_t *u8x8, uint8_t msg,
                                      uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
        case U8X8_MSG_BYTE_SEND:
            if (s_tx_len + arg_int > SSD1306_TX_BUF_LEN) {
                /* Bug: u8g2 sent more bytes between START and END than we
                 * sized for. Drop the rest to keep the protocol roughly
                 * intact rather than scribbling past the buffer. */
                return 0;
            }
            memcpy(&s_tx_buf[s_tx_len], arg_ptr, arg_int);
            s_tx_len += arg_int;
            break;

        case U8X8_MSG_BYTE_INIT:
            /* I2C2 peripheral was initialized by MX_I2C2_Init() in main. */
            break;

        case U8X8_MSG_BYTE_SET_DC:
            /* I2C displays carry the D/C# bit inside the control byte; the
             * dedicated GPIO message is meaningless on this bus. */
            break;

        case U8X8_MSG_BYTE_START_TRANSFER:
            s_tx_len = 0;
            break;

        case U8X8_MSG_BYTE_END_TRANSFER:
            if (s_tx_len > 0U) {
                HAL_StatusTypeDef hs = HAL_I2C_Master_Transmit(
                    &hi2c2,
                    u8x8_GetI2CAddress(u8x8),
                    s_tx_buf,
                    (uint16_t)s_tx_len,
                    SSD1306_HAL_TX_TIMEOUT_MS);
                s_tx_len = 0;
                if (hs != HAL_OK) {
                    return 0;
                }
            }
            break;

        default:
            return 0;
    }
    return 1;
}

/* u8g2 GPIO/delay callback — millisecond delays during init, no GPIOs to
 * wiggle (the module has its own reset; no D/C# pin on I2C). */
static uint8_t u8x8_gpio_and_delay_stm32(u8x8_t *u8x8, uint8_t msg,
                                         uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            /* GPIOs configured by MX_GPIO_Init(). */
            break;

        case U8X8_MSG_DELAY_MILLI:
            osDelay(arg_int);
            break;

        case U8X8_MSG_DELAY_10MICRO:
            /* 10 µs * arg_int; below the 1 ms SysTick floor and only used
             * during init. Round up to 1 ms — slows init by at most a few
             * ms which is invisible to a 5 s display task startup. */
            osDelay(1);
            break;

        case U8X8_MSG_DELAY_100NANO:
        case U8X8_MSG_DELAY_NANO:
            /* Sub-microsecond — far below our timing resolution. nop. */
            break;

        case U8X8_MSG_GPIO_RESET:
        case U8X8_MSG_GPIO_CS:
        case U8X8_MSG_GPIO_DC:
        case U8X8_MSG_GPIO_I2C_CLOCK:
        case U8X8_MSG_GPIO_I2C_DATA:
            /* I2C-only display — no dedicated reset/CS/DC pins wired. */
            break;

        default:
            return 0;
    }
    return 1;
}

ssd1306_status_t ssd1306_init(uint8_t i2c_addr_7bit)
{
    /* Verify the device ACKs its address before we send any init bytes —
     * gives a clean "PROBE" error instead of a downstream init failure if
     * the module is unplugged, mis-soldered, or strapped to a different
     * I2C address. */
    if (HAL_I2C_IsDeviceReady(&hi2c2,
                              (uint16_t)(i2c_addr_7bit << 1U),
                              SSD1306_PROBE_TRIES,
                              SSD1306_PROBE_TIMEOUT_MS) != HAL_OK) {
        return SSD1306_ERR_PROBE;
    }

    /* Hardware-verified setup: u8g2's SSD1306 univision 128x32 driver
     * matches this panel's init expectations (column mapping, charge
     * pump, segment-remap). Other variants tried but reverted:
     *   - sh1106_i2c_128x32_visionox_1: caused widespread render
     *     artifacts (column offset incompatible with this panel).
     *
     * Known cosmetic: a single stray pixel appears at panel (0,0)
     * after the first frame draws — confirmed not from our drawing
     * code (it persists through blank u8g2_FirstPage/NextPage
     * flushes) and not present before the first non-init data write.
     * Likely a panel hardware quirk (stuck pixel activated when
     * display goes active) or a u8g2 / SSD1306 init quirk we
     * haven't fully traced. One pixel out of 4096; deferred. */
    u8g2_Setup_ssd1306_i2c_128x32_univision_1(
        &s_u8g2,
        U8G2_R0,
        u8x8_byte_stm32_hw_i2c,
        u8x8_gpio_and_delay_stm32);

    /* Tell u8g2 the 8-bit slave address (HAL convention). u8g2 stores it on
     * the u8x8 sub-struct; our byte callback reads it back via
     * u8x8_GetI2CAddress on every transfer. */
    u8x8_SetI2CAddress(u8g2_GetU8x8(&s_u8g2),
                       (uint8_t)(i2c_addr_7bit << 1U));

    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);  /* wake panel from sleep */
    u8g2_ClearDisplay(&s_u8g2);     /* zero GDDRAM so no garbage pixels */

    s_initialized = true;
    return SSD1306_OK;
}

u8g2_t *ssd1306_u8g2(void)
{
    return s_initialized ? &s_u8g2 : NULL;
}
