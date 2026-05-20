/**
 * display_task.c — bring-up smoke test for the 128x32 OLED.
 *
 * This is the placeholder render loop: a frame + horizontal/vertical
 * crosshair + a small box that walks across the bottom. No text yet —
 * fonts land in a later commit, then this loop gets replaced by the
 * real display_render() that paints live INA238 stats from the
 * monitor_state snapshot.
 *
 * Goal of this stage: confirm that ssd1306_init succeeds, that u8g2's
 * page iteration flushes correctly over I2C2, and that the visible
 * pixels match what we asked u8g2 to draw (i.e. the SSD1306 vs
 * SSD1116/SH1106 column-offset quirk would show up here as a 2 px
 * shift on the frame).
 *
 * Address discovery is scan-driven: on every retry the task re-scans
 * I2C2 and only ssd1306_init()s addresses that actually ACK'd. That
 * lets a late-plugged module come up without a reset, and avoids
 * hammering ssd1306_init() against a quiet bus.
 */

#include "display_task.h"

#include "cdc_print.h"
#include "cmsis_os2.h"
#include "i2c.h"                /* &hi2c2 */
#include "ssd1306.h"
#include "stm32c0xx_hal.h"      /* HAL_I2C_IsDeviceReady */

#include <stdio.h>              /* snprintf */

#define DISPLAY_TASK_STARTUP_DELAY_MS    5000U
#define DISPLAY_TASK_INIT_RETRY_MS       1000U
#define DISPLAY_TASK_FRAME_INTERVAL_MS   200U   /* 5 Hz */

/* Per-address probe timing for the scan. 10 ms is what i2c_bus_scan uses on
 * I2C1; same characteristics apply to I2C2. With no devices on the bus the
 * full sweep is ~1.1 s — acceptable as a startup / retry step. */
#define SCAN_PER_ADDR_TIMEOUT_MS         10U
#define SCAN_PER_ADDR_TRIES              1U

/* 7-bit I2C addresses 0x00..0x07 and 0x78..0x7F are reserved by the spec. */
#define I2C_ADDR_FIRST                   0x08U
#define I2C_ADDR_LAST                    0x77U

/* Generous: in practice I2C2 has one device, but if a logic-analyzer dev
 * board got hot-plugged we'd want to see them all. */
#define SCAN_MAX_DEVICES                 8

static osThreadId_t       s_task_handle;
static const osThreadAttr_t s_task_attr = {
    .name = "display",
    .priority = (osPriority_t)osPriorityNormal,
    .stack_size = 256 * 4,
};

/* Sweep I2C2 with polled HAL_I2C_IsDeviceReady; fill addrs[] up to `max`
 * entries with the addresses that ACK'd, and format a human-readable
 * "scan: 0xNN 0xNN ..." line into log_buf. Returns the device count
 * (capped at `max` for the addrs[] copy but unbounded for the log). */
static int scan_i2c2(uint8_t *addrs, size_t max,
                     char *log_buf, size_t log_size)
{
    int found = 0;
    size_t written = (size_t)snprintf(log_buf, log_size, "scan:");

    for (uint8_t addr = I2C_ADDR_FIRST; addr <= I2C_ADDR_LAST; addr++) {
        HAL_StatusTypeDef hs = HAL_I2C_IsDeviceReady(
            &hi2c2,
            (uint16_t)(addr << 1U),
            SCAN_PER_ADDR_TRIES,
            SCAN_PER_ADDR_TIMEOUT_MS);
        if (hs != HAL_OK) {
            continue;
        }
        if ((size_t)found < max) {
            addrs[found] = addr;
        }
        found++;
        if (written < log_size) {
            int n = snprintf(log_buf + written, log_size - written,
                             " 0x%02X", (unsigned)addr);
            if (n > 0) {
                written += (size_t)n;
            }
        }
    }

    if (found == 0 && written < log_size) {
        (void)snprintf(log_buf + written, log_size - written, " (none)");
    }
    return found;
}

/* One pass: scan I2C2, log the result, then call ssd1306_init() on each
 * ACK'd address until one succeeds. Returns the 7-bit address of the
 * working OLED, or 0 if nothing on the bus initialised as an SSD1306. */
static uint8_t scan_and_probe(int retry)
{
    uint8_t addrs[SCAN_MAX_DEVICES];
    char    scan_buf[160];

    int n = scan_i2c2(addrs, sizeof(addrs) / sizeof(addrs[0]),
                      scan_buf, sizeof(scan_buf));
    cdc_printf("display: I2C2 %s (retry=%d)\r\n", scan_buf, retry);

    int cap = (n < (int)(sizeof(addrs) / sizeof(addrs[0])))
                ? n
                : (int)(sizeof(addrs) / sizeof(addrs[0]));
    for (int i = 0; i < cap; i++) {
        ssd1306_status_t st = ssd1306_init(addrs[i]);
        cdc_printf("display: try 0x%02X st=%d\r\n",
                   (unsigned)addrs[i], (int)st);
        if (st == SSD1306_OK) {
            return addrs[i];
        }
    }
    return 0U;
}

static void DisplayTaskBody(void *argument)
{
    (void)argument;

    /* Same CDC enumeration grace as ina238_task — without it any startup
     * log lines vanish before a terminal is attached. */
    osDelay(DISPLAY_TASK_STARTUP_DELAY_MS);

    /* Probe retry loop. Scan takes ~1.1 s when the bus is empty so the
     * effective retry cadence is ~2 s. */
    uint8_t addr = 0U;
    int retry = 0;
    while ((addr = scan_and_probe(retry)) == 0U) {
        cdc_printf("display: no SSD1306 found on I2C2, retry=%d\r\n", retry++);
        osDelay(DISPLAY_TASK_INIT_RETRY_MS);
    }
    cdc_printf("display: init OK at 0x%02X\r\n", (unsigned)addr);

    u8g2_t *u8g2 = ssd1306_u8g2();
    uint32_t frame = 0U;

    for (;;) {
        u8g2_FirstPage(u8g2);
        do {
            /* Frame around the 128x32 panel — exposes column-offset bugs
             * (SSD1116 / SH1106 quirks show as a 2 px shift on the right
             * edge or wraparound). */
            u8g2_DrawFrame(u8g2, 0, 0, 128, 32);

            /* Crosshair so we can eyeball the pixel grid. */
            u8g2_DrawHLine(u8g2, 0, 16, 128);
            u8g2_DrawVLine(u8g2, 64, 0, 32);

            /* 4x4 box sweeps left-to-right at y=22 — animation confirms
             * the page-flush path runs each frame, not just at init. */
            uint8_t bx = (uint8_t)((frame * 4U) % 124U);
            u8g2_DrawBox(u8g2, bx, 22, 4, 4);
        } while (u8g2_NextPage(u8g2));

        frame++;
        osDelay(DISPLAY_TASK_FRAME_INTERVAL_MS);
    }
}

void display_task_start(void)
{
    s_task_handle = osThreadNew(DisplayTaskBody, NULL, &s_task_attr);
}
