/**
 * ina238.h — driver for TI INA238 (and pin-compatible INA237) current /
 *            voltage / power monitor on I2C1.
 *
 * Datasheet: Docs/ina238.pdf (TI SBOSA21).
 *
 * Family compat: INA237 and INA238 share the identical register map and
 * scale factors. The two parts differ only in spec (INA238 has tighter
 * offset, CMRR, and die-temp accuracy). This driver accepts either device
 * ID at probe() time; the detected variant is recorded in `ina238_t` so
 * application code can log or branch on it.
 *
 * Hardware on this board:
 *   - I2C1 (PB8/PB9), shared with PN532 — all bus access goes through i2c_bus.
 *   - INA_ALERT on PC14 (EXTI14). Optional; not used by this driver yet.
 *   - Rshunt = 4 mΩ 1%. With ADCRANGE=0 (±163.84 mV) the bus tops out at
 *     ~40.96 A; the default config calibrates for 40 A full-scale.
 *
 * Threading: every public function in this file takes the bus mutex via
 * i2c_bus_lock(), so it is safe to call from multiple FreeRTOS tasks.
 * Functions block the calling task while the DMA transaction runs.
 *
 * Error model: all functions return an `ina238_status_t`. `INA238_OK` means
 * the bytes on the wire matched expectations; other codes report HAL faults,
 * mutex/DMA timeouts, or unexpected device IDs.
 */

#ifndef INA238_H
#define INA238_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl so we don't drag stm32 HAL headers into host-side unit tests.
 * The real type lives in stm32c0xx_hal_i2c.h; tests provide a stub. */
struct __I2C_HandleTypeDef;
typedef struct __I2C_HandleTypeDef I2C_HandleTypeDef;

/* --- Register addresses (datasheet §7.6) --- */
#define INA238_REG_CONFIG          0x00U
#define INA238_REG_ADC_CONFIG      0x01U
#define INA238_REG_SHUNT_CAL       0x02U
#define INA238_REG_VSHUNT          0x04U
#define INA238_REG_VBUS            0x05U
#define INA238_REG_DIETEMP         0x06U
#define INA238_REG_CURRENT         0x07U
#define INA238_REG_POWER           0x08U
#define INA238_REG_DIAG_ALRT       0x0BU
#define INA238_REG_SOVL            0x0CU
#define INA238_REG_SUVL            0x0DU
#define INA238_REG_BOVL            0x0EU
#define INA238_REG_BUVL            0x0FU
#define INA238_REG_TEMP_LIMIT      0x10U
#define INA238_REG_PWR_LIMIT       0x11U
#define INA238_REG_MANUFACTURER_ID 0x3EU
#define INA238_REG_DEVICE_ID       0x3FU

/* Expected ID register contents. Mismatch ⇒ wrong device on this address.
 *
 * The probe() accepts both INA237 and INA238 — same register map, same
 * scaling. To narrow your driver to a single variant, check
 * `dev->detected_device_id` after probe() succeeds. */
#define INA238_MANUFACTURER_ID     0x5449U  /* "TI" ASCII */
#define INA238_DEVICE_ID           0x2380U  /* INA238, die rev in low nibble */
#define INA238_DEVICE_ID_INA237    0x2370U  /* INA237 variant (accepted)    */
#define INA238_DEVICE_ID_MASK      0xFFF0U  /* mask off die-revision nibble */

/* CONFIG register bits (§7.6.1.1) — we only flag the ones we use. */
#define INA238_CONFIG_ADCRANGE     (1U << 4)  /* 1 = ±40.96 mV, 0 = ±163.84 mV */
#define INA238_CONFIG_RST          (1U << 15) /* write-1 = full reset */

/* SI scale factors used in fixed-point conversions. */
#define INA238_VBUS_LSB_uV         3125  /* 3.125 mV per LSB */
#define INA238_VSHUNT_LSB_nV_RANGE0 5000 /* 5 µV per LSB (ADCRANGE=0) */
#define INA238_VSHUNT_LSB_nV_RANGE1 1250 /* 1.25 µV per LSB (ADCRANGE=1) */
#define INA238_DIETEMP_LSB_mC      125   /* 0.125 °C per LSB (top 12 bits) */

typedef enum {
    INA238_OK = 0,
    INA238_ERR_BUS_BUSY,      /* mutex timeout */
    INA238_ERR_HAL,           /* HAL_I2C_* returned non-OK */
    INA238_ERR_DMA_TIMEOUT,   /* DMA completion semaphore timed out */
    INA238_ERR_BAD_ID,        /* manufacturer/device id mismatch in probe() */
} ina238_status_t;

typedef struct {
    I2C_HandleTypeDef *hi2c;        /* HAL handle for the bus (e.g. &hi2c1) */
    uint8_t i2c_addr_7bit;          /* 0x40..0x4F per A0/A1 strap */
    uint32_t shunt_micro_ohm;       /* Rshunt in µΩ (4000 for a 4 mΩ shunt) */
    uint32_t max_expected_current_ma; /* full-scale current target in mA */
    bool adc_range_low;             /* true = ±40.96 mV, false = ±163.84 mV */
} ina238_config_t;

typedef struct {
    ina238_config_t cfg;
    /* Raw DEVICE_ID register value as read by probe(). Compare against
     * INA238_DEVICE_ID or INA238_DEVICE_ID_INA237 (with mask) to branch
     * on variant. 0 until probe() succeeds. */
    uint16_t detected_device_id;
    /* Per-conversion LSBs cached at configure() time to keep reads cheap. */
    uint32_t current_lsb_nano_a;    /* nA per CURRENT LSB */
    uint32_t shunt_lsb_nv;          /* nV per VSHUNT LSB (5000 or 1250) */
} ina238_t;

/* --- Lifecycle --- */

/* Verify the device responds and matches the expected IDs. Reads
 * MANUFACTURER_ID and DEVICE_ID; returns INA238_ERR_BAD_ID on mismatch.
 * On success, populates `dev->detected_device_id` with the raw value so
 * the caller can tell INA237 from INA238. Call this once before configure(). */
ina238_status_t ina238_probe(ina238_t *dev, const ina238_config_t *cfg);

/* Push CONFIG + SHUNT_CAL based on the config struct. Computes SHUNT_CAL
 * from Rshunt and max_expected_current. Safe to re-run to reconfigure. */
ina238_status_t ina238_configure(ina238_t *dev);

/* Issue a software reset (CONFIG bit 15). All registers revert to defaults. */
ina238_status_t ina238_reset(ina238_t *dev);

/* --- Raw register access (for diagnostics / extension) --- */

ina238_status_t ina238_read_reg16(ina238_t *dev, uint8_t reg, uint16_t *value);
ina238_status_t ina238_write_reg16(ina238_t *dev, uint8_t reg, uint16_t value);

/* POWER is the only 24-bit register; everything else is 16-bit. */
ina238_status_t ina238_read_reg24(ina238_t *dev, uint8_t reg, uint32_t *value);

/* --- Converted readings (post-calibration) --- */

/* Bus voltage in millivolts (0..~85_000). */
ina238_status_t ina238_read_bus_mv(ina238_t *dev, uint32_t *millivolts);

/* Shunt voltage in microvolts, signed (positive = current flows VIN+ → VIN-). */
ina238_status_t ina238_read_shunt_uv(ina238_t *dev, int32_t *microvolts);

/* Die temperature in milli-degrees C (e.g. 27500 = 27.5 °C). */
ina238_status_t ina238_read_die_temp_mc(ina238_t *dev, int32_t *milli_celsius);

/* Current in milliamps (signed). Depends on configure() having run. */
ina238_status_t ina238_read_current_ma(ina238_t *dev, int32_t *milliamps);

/* --- Calibration math (exposed for testing) ---
 *
 * SHUNT_CAL = (819.2 × Imax_mA × Rshunt_mΩ) / 32768   (ADCRANGE=0)
 * SHUNT_CAL = (4 × 819.2 × Imax_mA × Rshunt_mΩ) / 32768 (ADCRANGE=1)
 *
 * Stored as 15-bit unsigned; the calc returns the raw register value
 * clamped to 32767. CURRENT_LSB (nA) = Imax_mA × 1e6 / 32768.
 */
uint16_t ina238_calc_shunt_cal(uint32_t imax_ma, uint32_t rshunt_micro_ohm,
                               bool adc_range_low);
uint32_t ina238_calc_current_lsb_na(uint32_t imax_ma);

#ifdef __cplusplus
}
#endif

#endif /* INA238_H */
