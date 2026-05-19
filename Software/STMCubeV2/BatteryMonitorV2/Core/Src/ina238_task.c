/**
 * ina238_task.c — INA238 (INA237 compat) bring-up smoke test.
 *
 * Probes the device (verifies MANUFACTURER_ID/DEVICE_ID), configures it for
 * the board's 4 mΩ shunt at 40 A full-scale, then emits one line per second
 * over USB-CDC with VBUS, VSHUNT, CURRENT, and die temp. Output format is
 * stable and parseable so scripts/telemetry_check.py can pick it up.
 *
 * The "ina238" tag in the output is the driver name, not necessarily the
 * silicon — probe() also accepts INA237. After a successful probe, the line
 * "ina238: probe OK dev_id=0xNNNN" reports which variant the chip actually
 * is so you can tell at a glance.
 */

#include "ina238_task.h"

#include "cmsis_os2.h"
#include "direct_io.h"    /* relay_enable/disable for load-test toggle */
#include "i2c.h"          /* hi2c1 */
#include "i2c_bus.h"      /* i2c_bus_scan */
#include "ina238.h"
#include "tusb.h"

#include <stdio.h>
#include <string.h>

/* Board-specific config: see schematic. INA238 with A0=A1=GND → 0x40.
 * Rshunt = 4 mΩ 1%. 40 A target keeps us inside the ±163.84 mV ADCRANGE=0
 * window (40 A × 4 mΩ = 160 mV). */
#define INA238_ADDR_7BIT          0x40U
#define INA238_RSHUNT_uOHM        4000U
#define INA238_MAX_CURRENT_mA     40000U

static ina238_t s_ina238;

static osThreadId_t s_task_handle;
static const osThreadAttr_t s_task_attr = {
    .name = "ina238",
    .priority = (osPriority_t)osPriorityNormal,
    .stack_size = 256 * 4,
};

/* Write `len` bytes to USB-CDC, looping until all bytes are queued.
 * Without this loop, tud_cdc_write silently returns short when the TX FIFO
 * is full (rapid back-to-back writes can exceed CFG_TUD_CDC_TX_BUFSIZE),
 * dropping the tail of the write — including the \r\n line terminator. */
static void cdc_write_line(const char *line, int len)
{
    if (!tud_cdc_connected() || len <= 0) {
        return;
    }
    int written = 0;
    while (written < len) {
        uint32_t n = tud_cdc_write(line + written, (uint32_t)(len - written));
        written += (int)n;
        tud_cdc_write_flush();
        if (written < len) {
            /* FIFO full — yield so the USB device task can drain it. */
            osDelay(1);
        }
    }
}

static void Ina238TaskBody(void *argument)
{
    (void)argument;

    /* TODO(bring-up): drop this delay once we have a startup banner / CDC
     * line-state callback. CDC enumerates before the host opens the port, so
     * any probe-failure messages emitted during boot vanish into the void.
     * 5 s gives a human time to attach a terminal after flashing. */
    osDelay(5000);

    const ina238_config_t cfg = {
        .hi2c = &hi2c1,
        .i2c_addr_7bit = INA238_ADDR_7BIT,
        .shunt_micro_ohm = INA238_RSHUNT_uOHM,
        .max_expected_current_ma = INA238_MAX_CURRENT_mA,
        .adc_range_low = false, /* ±163.84 mV range — enough for 40 A on 4 mΩ */
    };

    char line[160];  /* needs room for full scan list (worst-case ~120 chars) */
    int n;

    /* One-shot bus scan up front — if the INA238 is at a different address
     * than expected, or the bus is wedged, this surfaces it immediately. */
    (void)i2c_bus_scan(&hi2c1, line, sizeof(line));
    cdc_write_line(line, (int)strlen(line));
    cdc_write_line("\r\n", 2);

    /* Diagnostic raw reads of the ID registers — bypasses probe()'s value
     * checks and just shows whatever the device returned. Differentiates:
     *   - DMA timeout / HAL error (status != OK) → bus/clock/DMA issue
     *   - All-zeros or all-FFFF → device ACKs address but read transaction
     *     isn't completing properly
     *   - 0x5449 / 0x2380 (or 0x2370) with probe still failing → bug
     *   - Different values → wrong device at this address, or bit-mangling */
    s_ina238.cfg = cfg;  /* read_reg16 needs cfg seeded; probe() also does this */
    uint16_t raw_mfg = 0xFFFFU;
    uint16_t raw_dev = 0xFFFFU;
    ina238_status_t s_mfg = ina238_read_reg16(
        &s_ina238, INA238_REG_MANUFACTURER_ID, &raw_mfg);
    ina238_status_t s_dev = ina238_read_reg16(
        &s_ina238, INA238_REG_DEVICE_ID, &raw_dev);
    n = snprintf(line, sizeof(line),
                 "ina238 raw: MFG[3E]=0x%04X(st=%d) DEV[3F]=0x%04X(st=%d)\r\n",
                 (unsigned)raw_mfg, (int)s_mfg, (unsigned)raw_dev, (int)s_dev);
    cdc_write_line(line, n);

    /* Probe loop — retry on failure rather than wedging the task. The bus may
     * not be electrically settled at first boot. */
    int retry = 0;
    ina238_status_t st;
    while ((st = ina238_probe(&s_ina238, &cfg)) != INA238_OK) {
        n = snprintf(line, sizeof(line),
                     "ina238: probe failed st=%d at 0x%02X, retry %d\r\n",
                     (int)st, (unsigned)cfg.i2c_addr_7bit, retry);
        cdc_write_line(line, n);
        retry++;
        osDelay(1000);
    }
    n = snprintf(line, sizeof(line),
                 "ina238: probe OK dev_id=0x%04X\r\n",
                 (unsigned)s_ina238.detected_device_id);
    cdc_write_line(line, n);

    if (ina238_configure(&s_ina238) != INA238_OK) {
        n = snprintf(line, sizeof(line), "ina238: configure failed\r\n");
        cdc_write_line(line, n);
        /* fall through — registers may still be at reset defaults */
    }

    /* One-shot configuration dump so we can verify the chip's view of its
     * own state matches what we tried to write. CONFIG bit 4 = ADCRANGE (0 =
     * ±163.84 mV, 1 = ±40.96 mV). SHUNT_CAL should match our calc (~4000
     * for 4 mΩ / 40 A target). ADC_CONFIG default 0xFB68 = continuous mode,
     * 1052 µs conv times, no averaging. DIAG_ALRT[14:0] = sticky error /
     * overflow flags; any nonzero bit there is worth investigating. */
    {
        uint16_t r_cfg = 0, r_adc = 0, r_cal = 0, r_diag = 0;
        (void)ina238_read_reg16(&s_ina238, INA238_REG_CONFIG, &r_cfg);
        (void)ina238_read_reg16(&s_ina238, INA238_REG_ADC_CONFIG, &r_adc);
        (void)ina238_read_reg16(&s_ina238, INA238_REG_SHUNT_CAL, &r_cal);
        (void)ina238_read_reg16(&s_ina238, INA238_REG_DIAG_ALRT, &r_diag);
        n = snprintf(line, sizeof(line),
                     "ina238 regs: CONFIG=0x%04X ADC_CONFIG=0x%04X "
                     "SHUNT_CAL=0x%04X DIAG_ALRT=0x%04X\r\n",
                     (unsigned)r_cfg, (unsigned)r_adc,
                     (unsigned)r_cal, (unsigned)r_diag);
        cdc_write_line(line, n);
    }

    /* Bring-up load test: log a few no-load samples, then close K1 so the
     * subsequent readings show the load current. Lets the operator correlate
     * Vshunt / I against an expected resistance from the log. Move out of
     * this task once a higher-level controller exists. */
    uint32_t loops = 0;
    const uint32_t close_relay_at = 5;  /* 5 s of no-load samples first */
    bool k1_on = false;
    osThreadId_t self = osThreadGetId();

    for (;;) {
        /* Pre-line countdown / event banner so the K1 transition can't be
         * missed in CDC scrollback. */
        if (!k1_on) {
            if (loops < close_relay_at) {
                n = snprintf(line, sizeof(line),
                             "ina238: K1 closes in %lu samples\r\n",
                             (unsigned long)(close_relay_at - loops));
                cdc_write_line(line, n);
            } else {
                /* Flank with separator lines so it's obvious in the log. */
                cdc_write_line("====================================\r\n", 38);
                cdc_write_line("ina238: K1 RELAY ENABLED (load test)\r\n", 38);
                cdc_write_line("====================================\r\n", 38);
                relay_enable();
                k1_on = true;
            }
        }
        loops++;

        uint32_t vbus_mv = 0;
        int32_t  vshunt_uv = 0;
        int32_t  current_ma = 0;
        int32_t  temp_mc = 0;

        /* Bring-up: also read the raw VSHUNT / VBUS register words so we
         * can see what the ADC actually reports before any scaling. Rip
         * these reads once the math is trusted. */
        uint16_t raw_vshunt = 0xFFFFU;
        uint16_t raw_vbus = 0xFFFFU;
        (void)ina238_read_reg16(&s_ina238, INA238_REG_VSHUNT, &raw_vshunt);
        (void)ina238_read_reg16(&s_ina238, INA238_REG_VBUS, &raw_vbus);

        ina238_status_t s1 = ina238_read_bus_mv(&s_ina238, &vbus_mv);
        ina238_status_t s2 = ina238_read_shunt_uv(&s_ina238, &vshunt_uv);
        ina238_status_t s3 = ina238_read_current_ma(&s_ina238, &current_ma);
        ina238_status_t s4 = ina238_read_die_temp_mc(&s_ina238, &temp_mc);

        /* HWM in bytes — the CMSIS-RTOS2 FreeRTOS adapter already multiplies
         * by sizeof(StackType_t) internally, so no further conversion. Passing
         * NULL would return 0 because the adapter treats it as "invalid
         * handle"; we pass the real handle captured before the loop. */
        uint32_t stack_hwm_bytes = osThreadGetStackSpace(self);

        if (s1 == INA238_OK && s2 == INA238_OK && s3 == INA238_OK && s4 == INA238_OK) {
            n = snprintf(line, sizeof(line),
                         "ina238 K1=%s vshunt=0x%04X=%lduV vbus=0x%04X=%lumV i=%ldmA tdie=%ldmC stk_free=%luB\r\n",
                         k1_on ? "ON " : "OFF",
                         (unsigned)raw_vshunt, (long)vshunt_uv,
                         (unsigned)raw_vbus,   (unsigned long)vbus_mv,
                         (long)current_ma, (long)temp_mc,
                         (unsigned long)stack_hwm_bytes);
        } else {
            n = snprintf(line, sizeof(line),
                         "ina238 K1=%s read err: s1=%d s2=%d s3=%d s4=%d stk_free=%luB\r\n",
                         k1_on ? "ON " : "OFF",
                         s1, s2, s3, s4,
                         (unsigned long)stack_hwm_bytes);
        }
        cdc_write_line(line, n);

        osDelay(1000);
    }
}

void ina238_task_start(void)
{
    s_task_handle = osThreadNew(Ina238TaskBody, NULL, &s_task_attr);
}
