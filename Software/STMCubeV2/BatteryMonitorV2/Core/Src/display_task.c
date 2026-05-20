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
 */

#include "display_task.h"

#include "cdc_print.h"
#include "cmsis_os2.h"
#include "ssd1306.h"

#define DISPLAY_TASK_STARTUP_DELAY_MS    5000U
#define DISPLAY_TASK_INIT_RETRY_MS       1000U
#define DISPLAY_TASK_FRAME_INTERVAL_MS   200U   /* 5 Hz */

static osThreadId_t       s_task_handle;
static const osThreadAttr_t s_task_attr = {
    .name = "display",
    .priority = (osPriority_t)osPriorityNormal,
    .stack_size = 256 * 4,
};

static void DisplayTaskBody(void *argument)
{
    (void)argument;

    /* Same CDC enumeration grace as ina238_task — without it any startup
     * log lines vanish before a terminal is attached. */
    osDelay(DISPLAY_TASK_STARTUP_DELAY_MS);

    cdc_printf("display: probing OLED at 0x%02X on I2C2\r\n",
               (unsigned)SSD1306_I2C_ADDR_7BIT);

    int retry = 0;
    ssd1306_status_t st;
    while ((st = ssd1306_init()) != SSD1306_OK) {
        cdc_printf("display: init failed st=%d retry=%d\r\n",
                   (int)st, retry++);
        osDelay(DISPLAY_TASK_INIT_RETRY_MS);
    }
    cdc_printf("display: init OK\r\n");

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
