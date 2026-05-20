/**
 * test/support/u8g2_stubs.c — host-test linker stubs for the u8g2 setup /
 * init / clear functions ssd1306_init calls.
 *
 * Linking the real u8g2 (csrc/) into the test build is possible — it's
 * portable C — but pulls in ~130 .c files of display drivers and helpers
 * for one transitive call. The test_ssd1306 tests we care about for now
 * (byte/delay callbacks + ssd1306_init's probe-fail short-circuit) never
 * reach these u8g2 functions, so empty stubs satisfy the linker without
 * the dependency footprint.
 *
 * Replace this with a real u8g2 link when test_ssd1306 grows tests that
 * actually exercise the post-probe init path (or when the desktop SDL
 * sim lands and brings u8g2-on-host with it for free).
 */

#include "u8g2.h"

/* u8g2.h declares this as `extern const u8g2_cb_t u8g2_cb_r0;`. The real
 * definition (in u8g2_setup.c) carries the rotation cb. We don't call it,
 * so an empty const struct is enough to resolve the reference. */
const u8g2_cb_t u8g2_cb_r0 = {0};

void u8g2_Setup_ssd1306_i2c_128x32_univision_1(u8g2_t *u8g2,
                                               const u8g2_cb_t *rotation,
                                               u8x8_msg_cb byte_cb,
                                               u8x8_msg_cb gpio_and_delay_cb)
{
    (void)rotation;
    /* Wire just enough that u8x8_SetI2CAddress (a macro that writes
     * u8x8->i2c_address) doesn't crash if a future test reaches it. */
    if (u8g2 != NULL) {
        u8g2->u8x8.byte_cb            = byte_cb;
        u8g2->u8x8.gpio_and_delay_cb  = gpio_and_delay_cb;
    }
}

void    u8x8_InitDisplay(u8x8_t *u8x8)        { (void)u8x8; }
void    u8x8_SetPowerSave(u8x8_t *u8x8, uint8_t is_enable)
                                              { (void)u8x8; (void)is_enable; }
void    u8g2_ClearDisplay(u8g2_t *u8g2)       { (void)u8g2; }
