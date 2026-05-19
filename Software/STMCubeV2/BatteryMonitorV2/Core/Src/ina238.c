/**
 * ina238.c — see ina238.h for the rationale and register map.
 *
 * Each public function:
 *   1. Acquires the bus mutex (i2c_bus_lock).
 *   2. Kicks off a DMA transaction via HAL_I2C_Mem_*_DMA.
 *   3. Blocks on the bus's binary completion semaphore.
 *   4. Releases the mutex and returns a status.
 *
 * Endianness: the INA238 sends MSB first; HAL_I2C_Mem_Read writes the buffer
 * in receive order, so buf[0] is the MSB.
 *
 * Calibration math is split into pure functions exposed in the header so the
 * Ceedling host tests can verify it without dragging in HAL.
 */

#include "ina238.h"

#include "i2c.h"        /* I2C_HandleTypeDef, HAL_I2C_Mem_*_DMA */
#include "i2c_bus.h"

#include <string.h>

/* HAL_I2C_Mem_*_DMA takes the 7-bit address pre-shifted into the upper bits.
 * INA238_ADDR_HAL(0x40) → 0x80, the byte the HAL writes onto the wire. */
#define INA238_ADDR_HAL(addr_7bit) ((uint16_t)((addr_7bit) << 1))

static ina238_status_t do_mem_read(ina238_t *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    if (!i2c_bus_lock(I2C_BUS_TIMEOUT_MS)) {
        return INA238_ERR_BUS_BUSY;
    }

    HAL_StatusTypeDef hs = HAL_I2C_Mem_Read_DMA(
        dev->cfg.hi2c, INA238_ADDR_HAL(dev->cfg.i2c_addr_7bit),
        reg, I2C_MEMADD_SIZE_8BIT, buf, len);
    if (hs != HAL_OK) {
        i2c_bus_unlock();
        return INA238_ERR_HAL;
    }

    if (!i2c_bus_wait_complete(I2C_BUS_TIMEOUT_MS)) {
        i2c_bus_unlock();
        return INA238_ERR_DMA_TIMEOUT;
    }

    i2c_bus_unlock();
    /* HAL_I2C_ErrorCallback also releases the semaphore — surface that as ERR_HAL. */
    return (HAL_I2C_GetError(dev->cfg.hi2c) == HAL_I2C_ERROR_NONE)
               ? INA238_OK
               : INA238_ERR_HAL;
}

static ina238_status_t do_mem_write(ina238_t *dev, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    if (!i2c_bus_lock(I2C_BUS_TIMEOUT_MS)) {
        return INA238_ERR_BUS_BUSY;
    }

    /* HAL_I2C_Mem_Write_DMA's buffer param is non-const; cast is safe — the
     * call doesn't mutate it. */
    HAL_StatusTypeDef hs = HAL_I2C_Mem_Write_DMA(
        dev->cfg.hi2c, INA238_ADDR_HAL(dev->cfg.i2c_addr_7bit),
        reg, I2C_MEMADD_SIZE_8BIT, (uint8_t *)buf, len);
    if (hs != HAL_OK) {
        i2c_bus_unlock();
        return INA238_ERR_HAL;
    }

    if (!i2c_bus_wait_complete(I2C_BUS_TIMEOUT_MS)) {
        i2c_bus_unlock();
        return INA238_ERR_DMA_TIMEOUT;
    }

    i2c_bus_unlock();
    return (HAL_I2C_GetError(dev->cfg.hi2c) == HAL_I2C_ERROR_NONE)
               ? INA238_OK
               : INA238_ERR_HAL;
}

/* --- Public raw-register API --- */

ina238_status_t ina238_read_reg16(ina238_t *dev, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    ina238_status_t st = do_mem_read(dev, reg, buf, sizeof(buf));
    if (st == INA238_OK) {
        *value = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    }
    return st;
}

ina238_status_t ina238_read_reg24(ina238_t *dev, uint8_t reg, uint32_t *value)
{
    uint8_t buf[3];
    ina238_status_t st = do_mem_read(dev, reg, buf, sizeof(buf));
    if (st == INA238_OK) {
        *value = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | (uint32_t)buf[2];
    }
    return st;
}

ina238_status_t ina238_write_reg16(ina238_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFFU) };
    return do_mem_write(dev, reg, buf, sizeof(buf));
}

/* --- Calibration (pure math, host-testable) --- */

uint32_t ina238_calc_current_lsb_na(uint32_t imax_ma)
{
    /* CURRENT_LSB (A) = Imax (A) / 2^15
     *   → CURRENT_LSB (nA) = Imax_mA × 1e6 / 32768
     * 64-bit math keeps us safe up to Imax_mA ≈ 4.4e6 (4400 A). */
    return (uint32_t)(((uint64_t)imax_ma * 1000000ULL) / 32768ULL);
}

uint16_t ina238_calc_shunt_cal(uint32_t imax_ma, uint32_t rshunt_micro_ohm,
                               bool adc_range_low)
{
    /* SHUNT_CAL = 819.2e6 × CURRENT_LSB × Rshunt     (ADCRANGE=0)
     *           = 4 × 819.2e6 × CURRENT_LSB × Rshunt (ADCRANGE=1)
     *
     * Substitute CURRENT_LSB = Imax_A / 32768; convert units so we stay in
     * integer math. Imax in mA (×1e-3), Rshunt in µΩ (×1e-6):
     *
     *   SHUNT_CAL = 819.2e6 × (Imax_mA × 1e-3 / 32768) × (Rshunt_uOhm × 1e-6)
     *             = (819.2 × Imax_mA × Rshunt_uOhm) / (32768 × 1000)
     *             = (4096 × Imax_mA × Rshunt_uOhm) / (5 × 32768 × 1000)
     *             = (4096 × Imax_mA × Rshunt_uOhm) / 163_840_000
     *
     * Multiplier ×4 for ADCRANGE=1 lifts it into 16-bit territory faster, so
     * we still clamp to the 15-bit register max (32767). */
    uint64_t num = 4096ULL * imax_ma * rshunt_micro_ohm;
    if (adc_range_low) {
        num *= 4ULL;
    }
    uint64_t cal = num / 163840000ULL;
    if (cal > 32767ULL) {
        cal = 32767ULL;
    }
    return (uint16_t)cal;
}

/* --- Lifecycle --- */

ina238_status_t ina238_probe(ina238_t *dev, const ina238_config_t *cfg)
{
    if (dev == NULL || cfg == NULL || cfg->hi2c == NULL) {
        return INA238_ERR_HAL;
    }
    dev->cfg = *cfg;
    dev->detected_device_id = 0;
    dev->current_lsb_nano_a = ina238_calc_current_lsb_na(cfg->max_expected_current_ma);
    dev->shunt_lsb_nv = cfg->adc_range_low
                           ? INA238_VSHUNT_LSB_nV_RANGE1
                           : INA238_VSHUNT_LSB_nV_RANGE0;

    uint16_t mfg_id = 0;
    ina238_status_t st = ina238_read_reg16(dev, INA238_REG_MANUFACTURER_ID, &mfg_id);
    if (st != INA238_OK) {
        return st;
    }
    if (mfg_id != INA238_MANUFACTURER_ID) {
        return INA238_ERR_BAD_ID;
    }

    uint16_t dev_id = 0;
    st = ina238_read_reg16(dev, INA238_REG_DEVICE_ID, &dev_id);
    if (st != INA238_OK) {
        return st;
    }
    const uint16_t id_masked = dev_id & INA238_DEVICE_ID_MASK;
    if (id_masked != INA238_DEVICE_ID &&
        id_masked != INA238_DEVICE_ID_INA237) {
        return INA238_ERR_BAD_ID;
    }
    dev->detected_device_id = dev_id;
    return INA238_OK;
}

ina238_status_t ina238_configure(ina238_t *dev)
{
    /* Write CONFIG first (sets ADCRANGE), then SHUNT_CAL — the cal value
     * depends on ADCRANGE being already latched. */
    uint16_t config = dev->cfg.adc_range_low ? INA238_CONFIG_ADCRANGE : 0;
    ina238_status_t st = ina238_write_reg16(dev, INA238_REG_CONFIG, config);
    if (st != INA238_OK) {
        return st;
    }
    uint16_t cal = ina238_calc_shunt_cal(dev->cfg.max_expected_current_ma,
                                         dev->cfg.shunt_micro_ohm,
                                         dev->cfg.adc_range_low);
    return ina238_write_reg16(dev, INA238_REG_SHUNT_CAL, cal);
}

ina238_status_t ina238_reset(ina238_t *dev)
{
    return ina238_write_reg16(dev, INA238_REG_CONFIG, INA238_CONFIG_RST);
}

/* --- Converted readings --- */

ina238_status_t ina238_read_bus_mv(ina238_t *dev, uint32_t *millivolts)
{
    uint16_t raw = 0;
    ina238_status_t st = ina238_read_reg16(dev, INA238_REG_VBUS, &raw);
    if (st != INA238_OK) {
        return st;
    }
    /* 3.125 mV/LSB. Use µV intermediate to avoid losing the fractional mV. */
    uint32_t microvolts = (uint32_t)raw * (uint32_t)INA238_VBUS_LSB_uV;
    *millivolts = microvolts / 1000U;
    return INA238_OK;
}

ina238_status_t ina238_read_shunt_uv(ina238_t *dev, int32_t *microvolts)
{
    uint16_t raw = 0;
    ina238_status_t st = ina238_read_reg16(dev, INA238_REG_VSHUNT, &raw);
    if (st != INA238_OK) {
        return st;
    }
    /* Sign-extend the 16-bit signed result, then scale (nV/LSB) into µV. */
    int32_t signed_raw = (int32_t)(int16_t)raw;
    int64_t nv = (int64_t)signed_raw * (int64_t)dev->shunt_lsb_nv;
    *microvolts = (int32_t)(nv / 1000);
    return INA238_OK;
}

ina238_status_t ina238_read_die_temp_mc(ina238_t *dev, int32_t *milli_celsius)
{
    uint16_t raw = 0;
    ina238_status_t st = ina238_read_reg16(dev, INA238_REG_DIETEMP, &raw);
    if (st != INA238_OK) {
        return st;
    }
    /* Top 12 bits are signed °C, bottom 4 reserved. Arithmetic shift on a
     * signed value sign-extends; the compiler we use (gcc/clang) defines
     * this consistently. */
    int16_t top12 = (int16_t)raw >> 4;
    *milli_celsius = (int32_t)top12 * (int32_t)INA238_DIETEMP_LSB_mC;
    return INA238_OK;
}

ina238_status_t ina238_read_current_ma(ina238_t *dev, int32_t *milliamps)
{
    uint16_t raw = 0;
    ina238_status_t st = ina238_read_reg16(dev, INA238_REG_CURRENT, &raw);
    if (st != INA238_OK) {
        return st;
    }
    /* current (nA) = signed_raw × CURRENT_LSB_nA  →  scale down to mA. */
    int32_t signed_raw = (int32_t)(int16_t)raw;
    int64_t nano_a = (int64_t)signed_raw * (int64_t)dev->current_lsb_nano_a;
    *milliamps = (int32_t)(nano_a / 1000000);
    return INA238_OK;
}
