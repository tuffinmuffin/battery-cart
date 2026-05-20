/**
 * test/support/tusb.h — host-test stub replacing Middlewares/tinyusb/src/tusb.h.
 *
 * The real header pulls in hundreds of lines of MCU-specific TinyUSB wiring
 * that doesn't parse on host gcc. Here we expose just the CDC surface
 * cdc_print.c calls, so CMock can auto-generate mocks for them and tests
 * can assert on the bytes that would have gone out over USB.
 */

#ifndef TUSB_H
#define TUSB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool     tud_cdc_connected(void);
uint32_t tud_cdc_write(const void *buf, uint32_t bufsize);
uint32_t tud_cdc_write_flush(void);

#endif /* TUSB_H */
