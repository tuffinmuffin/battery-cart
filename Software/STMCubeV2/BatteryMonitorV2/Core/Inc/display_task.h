/**
 * display_task.h — FreeRTOS task driving the 128x32 OLED via ssd1306 driver.
 *
 * Starts the display task. In this first commit the task draws a static
 * smoke pattern (frame + crosshair + a moving box) so we can verify the
 * I2C2 path + u8g2 init + page flush end-to-end on real hardware without
 * needing any font tables yet.
 */

#ifndef DISPLAY_TASK_H
#define DISPLAY_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void display_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_TASK_H */
