/**
 * display_task.c — FreeRTOS task that drives the OLED render loop.
 *
 * TODO(replace): this whole module is a bring-up placeholder. The
 * real render layer will be driven by the charge-state machine
 * (status text + view selection + screen router) and consume real
 * monitor_state values from the INA238 producer. Treat everything
 * here as scaffolding: the init/retry, the 5 Hz cadence, the demo
 * data injection, and the V/A toggle are all expected to be
 * replaced wholesale once the real controller exists.
 *
 * Coverage: file-level excluded (GCOVR_EXCL_FILE below) and not in
 * the test-required manifest. Don't add tests against this surface
 * — the API will change.
 *
 * Current behaviour (for hardware bring-up only):
 *   - one-shot ssd1306_init at startup with retry on failure
 *   - 5 Hz render loop: monitor_state_get -> display_render
 *   - demo block injects sweeping vbus/current/k1/bleed/fan/temp
 *     and rotates 3 status labels every 4 s
 */

/* GCOVR_EXCL_FILE — placeholder task body; see TODO above. */

#include "display_task.h"

#include "cdc_print.h"
#include "cmsis_os2.h"
#include "display_render.h"
#include "monitor_state.h"
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

    /* Probe retry loop. ssd1306_init at the known board address; retry
     * with a 1 s cadence if the panel isn't electrically settled yet. */
    int retry = 0;
    ssd1306_status_t st;
    while ((st = ssd1306_init(SSD1306_I2C_ADDR_DEFAULT)) != SSD1306_OK) {
        cdc_printf("display: init failed st=%d at 0x%02X, retry %d\r\n",
                   (int)st, (unsigned)SSD1306_I2C_ADDR_DEFAULT, retry++);
        osDelay(DISPLAY_TASK_INIT_RETRY_MS);
    }
    cdc_printf("display: init OK at 0x%02X\r\n",
               (unsigned)SSD1306_I2C_ADDR_DEFAULT);

    u8g2_t *u8g2 = ssd1306_u8g2();
    uint32_t frame = 0U;

    /* TEMP demo cycle: with the INA238 producer disabled the snapshot
     * stays at zeros, so we inject a sweep here and rotate the status
     * label every 4 s to exercise the layout against realistic text.
     * The 5 Hz render rate means 20 frames == 4 s. Drop both this
     * demo block and the cycle counter once a real charge-state
     * machine starts feeding monitor_state. */
    static const char *const kDemoLabels[] = {
        "Charging",
        "No Batt",
        "Trickle",
    };
    /* Demo battery serials matched to each label — "No Batt" shows
     * dashes because the NFC reader wouldn't have a tag to read.
     * Replace with the real PN532 read result once that driver lands.
     *
     * Strings sized at 10 chars total ("S/N: " + 5 chars) so the slot
     * stays comfortably clear of the F/K/B/heart tray on the right. */
    static const char *const kDemoSerials[] = {
        "S/N: AB12C",
        "S/N: -----",
        "S/N: AB12C",
    };
    const uint32_t cycle_frames = 20U;

    for (;;) {
        monitor_snapshot_t snap;
        monitor_state_get(&snap);

        snap.vbus_mv      = 12000U + (frame % 300U) * 10U;          /* 12.00..14.99 V */
        snap.current_ma   = (int32_t)((frame % 400U) * 25) - 2500;  /* -2.5 .. +7.5 A */
        snap.k1_on        = ((frame / 25U) & 1U) != 0U;
        snap.bleed_on     = ((frame / 50U) & 1U) != 0U;
        snap.fan_duty_pct = (uint32_t)((frame * 2U) % 100U);
        snap.tdie_mc      = 24000 + (int32_t)((frame % 200U) * 100);  /* 24..44 C */

        const uint32_t cycle = frame / cycle_frames;
        const size_t   nlbl  = sizeof(kDemoLabels) / sizeof(kDemoLabels[0]);
        const char *label    = kDemoLabels [cycle % nlbl];
        const char *serial   = kDemoSerials[cycle % nlbl];
        const bool show_v    = (cycle & 1U) == 0U;

        /* Render runs at 5 Hz (DISPLAY_TASK_FRAME_INTERVAL_MS=200 ms),
         * so frame/5 gives whole seconds since the task started.
         * Stand-in for a real charge-state-machine timer. */
        const uint32_t charge_time_s =
            frame / (1000U / DISPLAY_TASK_FRAME_INTERVAL_MS);

        display_render(u8g2, label, charge_time_s, serial, show_v, &snap);

        frame++;
        osDelay(DISPLAY_TASK_FRAME_INTERVAL_MS);
    }
}

void display_task_start(void)
{
    s_task_handle = osThreadNew(DisplayTaskBody, NULL, &s_task_attr);
}
