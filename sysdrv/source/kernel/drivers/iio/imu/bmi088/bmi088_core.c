// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * Bosch BMI088 6-Axis IMU — core driver
 *
 * Supports: accelerometer (±3/6/12/24 g), gyroscope (±125/250/500/1000/2000 °/s),
 *           on-chip temperature sensor.
 *
 * The BMI088 presents two independent register maps (acc / gyr) via separate
 * I2C slave addresses.  The bus-specific files (bmi088_i2c.c / bmi088_spi.c)
 * create the two regmaps and call bmi088_core_probe().
 *
 * Copyright (c) 2024  <your name>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#include "bmi088.h"

/* --------------------------------------------------------------------------
 * Accelerometer register map  (slave addr 0x18 / 0x19)
 * -------------------------------------------------------------------------- */
#define BMI088_ACC_CHIP_ID_REG			0x00
#define BMI088_ACC_CHIP_ID_VAL			0x1E

#define BMI088_ACC_STATUS_REG			0x03
#define BMI088_ACC_STATUS_DRDY_MSK		BIT(7)

/*
 * Accelerometer data registers: X_LSB(0x12), X_MSB(0x13),
 *                               Y_LSB(0x14), Y_MSB(0x15),
 *                               Z_LSB(0x16), Z_MSB(0x17)
 * Six consecutive bytes — suitable for a single bulk read.
 */
#define BMI088_ACC_X_LSB_REG			0x12

/*
 * Temperature registers (accelerometer slave).
 * temp_raw11 = (TEMP_MSB[7:0] << 3) | (TEMP_LSB[7:5])
 * temp_celsius = temp_raw11_signed * 0.125 + 23
 */
#define BMI088_TEMP_MSB_REG			0x22
#define BMI088_TEMP_LSB_REG			0x23
#define BMI088_TEMP_MSB_SHIFT			3
#define BMI088_TEMP_LSB_SHIFT			5
#define BMI088_TEMP_SIGN_BIT			10	/* 11-bit 2's complement */
#define BMI088_TEMP_OFFSET			184	/* 23 / 0.125 */
#define BMI088_TEMP_SCALE_MICRO			125000	/* 0.125 °C in µ°C */

/* ACC_CONF (0x40): output data rate + bandwidth */
#define BMI088_ACC_CONF_REG			0x40
#define BMI088_ACC_CONF_ODR_MSK			GENMASK(3, 0)
#define BMI088_ACC_CONF_ODR_100HZ		0x08
#define BMI088_ACC_CONF_BWP_MSK			GENMASK(6, 4)
#define BMI088_ACC_CONF_BWP_NORMAL		0x02

/* ACC_RANGE (0x41): full-scale range */
#define BMI088_ACC_RANGE_REG			0x41
#define BMI088_ACC_RANGE_MSK			GENMASK(1, 0)

/* Interrupt pin control */
#define BMI088_INT1_IO_CTRL_REG			0x53
#define BMI088_INT2_IO_CTRL_REG			0x54
#define BMI088_INT_IO_CTRL_LVL_MSK		BIT(2)
#define BMI088_INT_IO_CTRL_OD_MSK		BIT(3)
#define BMI088_INT_IO_CTRL_OUT_EN_MSK		BIT(4)

/* INT_MAP_DATA (0x58): route data-ready to INT1 / INT2 */
#define BMI088_INT_MAP_DATA_REG			0x58
#define BMI088_INT_MAP_DATA_INT1_DRDY_MSK	BIT(2)
#define BMI088_INT_MAP_DATA_INT2_DRDY_MSK	BIT(6)

/* Power control */
#define BMI088_ACC_PWR_CONF_REG			0x7C
#define BMI088_ACC_PWR_CONF_ACTIVE		0x00
#define BMI088_ACC_PWR_CONF_SUSPEND		0x03

#define BMI088_ACC_PWR_CTRL_REG			0x7D
#define BMI088_ACC_PWR_CTRL_ENABLE		0x04
#define BMI088_ACC_PWR_CTRL_DISABLE		0x00

/* Soft-reset: write 0xB6 */
#define BMI088_ACC_SOFTRESET_REG		0x7E
#define BMI088_SOFTRESET_VAL			0xB6

/* --------------------------------------------------------------------------
 * Gyroscope register map  (slave addr 0x68 / 0x69)
 * -------------------------------------------------------------------------- */
#define BMI088_GYR_CHIP_ID_REG			0x00
#define BMI088_GYR_CHIP_ID_VAL			0x0F

/*
 * Gyroscope rate data: X_LSB(0x02), X_MSB(0x03),
 *                      Y_LSB(0x04), Y_MSB(0x05),
 *                      Z_LSB(0x06), Z_MSB(0x07)
 * Six consecutive bytes — suitable for a single bulk read.
 */
#define BMI088_GYR_X_LSB_REG			0x02

/* INT_STAT_1 (0x0A): bit 7 = gyro data ready */
#define BMI088_GYR_INT_STAT_1_REG		0x0A
#define BMI088_GYR_INT_STAT_1_DRDY_MSK		BIT(7)

/* GYRO_RANGE (0x0F): full-scale range selection */
#define BMI088_GYR_RANGE_REG			0x0F
#define BMI088_GYR_RANGE_MSK			GENMASK(2, 0)

/*
 * GYRO_BANDWIDTH (0x10): combined ODR + filter-bandwidth selection.
 * Encoding: 0x00=2000Hz/532Hz-BW, 0x01=2000Hz/230Hz-BW, 0x02=1000Hz/116Hz-BW,
 *           0x03=400Hz/47Hz-BW,   0x04=200Hz/23Hz-BW,   0x05=100Hz/12Hz-BW,
 *           0x06=200Hz/64Hz-BW,   0x07=100Hz/32Hz-BW.
 */
#define BMI088_GYR_BANDWIDTH_REG		0x10
#define BMI088_GYR_BANDWIDTH_MSK		GENMASK(3, 0)
#define BMI088_GYR_BANDWIDTH_100HZ		0x05	/* 100 Hz ODR, 12 Hz BW */

/* GYRO_LPM1 (0x11): power mode */
#define BMI088_GYR_LPM1_REG			0x11
#define BMI088_GYR_LPM1_NORMAL			0x00
#define BMI088_GYR_LPM1_SUSPEND		0x80
#define BMI088_GYR_LPM1_DEEP_SUSPEND		0x20

/* Soft-reset: write 0xB6 (same magic as accelerometer) */
#define BMI088_GYR_SOFTRESET_REG		0x14

/* Interrupt enable / pin configuration */
#define BMI088_GYR_INT_CTRL_REG		0x15
#define BMI088_GYR_INT_CTRL_EN_MSK		BIT(7)

#define BMI088_INT3_INT4_IO_CONF_REG		0x16
#define BMI088_INT3_INT4_IO_MAP_REG		0x18
#define BMI088_INT3_IO_MAP_DRDY_MSK		BIT(0)
#define BMI088_INT4_IO_MAP_DRDY_MSK		BIT(7)

/* --------------------------------------------------------------------------
 * Timing constants  (from datasheet)
 * -------------------------------------------------------------------------- */
#define BMI088_ACC_RESET_DELAY_US		1000	/* ≥ 1 ms after acc soft-reset  */
#define BMI088_GYR_RESET_DELAY_US		30000	/* ≥ 30 ms after gyr soft-reset */
#define BMI088_ACC_PWR_SWITCH_DELAY_US		450	/* ≥ 450 µs: suspend→active      */
#define BMI088_ACC_ENABLE_DELAY_US		50000	/* ≥ 50 ms: acc enable→data ready */

/* --------------------------------------------------------------------------
 * Internal sensor-type enum (array index into scale / ODR tables)
 * -------------------------------------------------------------------------- */
enum bmi088_sensor_type {
	BMI088_ACCEL = 0,
	BMI088_GYRO,
	BMI088_TEMP,
};

/* --------------------------------------------------------------------------
 * Scale tables  (IIO_VAL_INT_PLUS_MICRO pairs: val, val2)
 * -------------------------------------------------------------------------- */

/*
 * Accelerometer full-scale ranges.
 * Scale = g_range * 9.81 m/s²/ 32768 LSB ≈ 9.81 / (10920 >> (idx)) m/s²/LSB
 * Register encoding:  0=±3 g, 1=±6 g, 2=±12 g, 3=±24 g
 */
struct bmi088_scale { int val; int val2; };

static const struct bmi088_scale bmi088_accel_scale[] = {
	{ 0,  898 },	/* ±3 g:  9.81 / 10920 ≈ 898 µm/s²  */
	{ 0, 1797 },	/* ±6 g:  9.81 /  5460 ≈ 1797 µm/s² */
	{ 0, 3593 },	/* ±12 g: 9.81 /  2730 ≈ 3593 µm/s² */
	{ 0, 7187 },	/* ±24 g: 9.81 /  1365 ≈ 7187 µm/s² */
};

/*
 * Gyroscope full-scale ranges.
 * Scale = dps_range * π / (180 * 32768) rad/s/LSB
 * Register encoding: 0=±2000 °/s, 1=±1000, 2=±500, 3=±250, 4=±125
 */
static const struct bmi088_scale bmi088_gyro_scale[] = {
	{ 0, 1065 },	/* ±2000 °/s */
	{ 0,  533 },	/* ±1000 °/s */
	{ 0,  266 },	/* ± 500 °/s */
	{ 0,  133 },	/* ± 250 °/s */
	{ 0,   66 },	/* ± 125 °/s */
};

static const struct bmi088_scale bmi088_temp_scale[] = {
	{ 0, BMI088_TEMP_SCALE_MICRO },
};

struct bmi088_scale_item {
	const struct bmi088_scale *tbl;
	int num;
};

static const struct bmi088_scale_item bmi088_scale_table[] = {
	[BMI088_ACCEL] = { .tbl = bmi088_accel_scale,
			   .num = ARRAY_SIZE(bmi088_accel_scale) },
	[BMI088_GYRO]  = { .tbl = bmi088_gyro_scale,
			   .num = ARRAY_SIZE(bmi088_gyro_scale) },
	[BMI088_TEMP]  = { .tbl = bmi088_temp_scale,
			   .num = ARRAY_SIZE(bmi088_temp_scale) },
};

/* --------------------------------------------------------------------------
 * ODR tables
 * -------------------------------------------------------------------------- */
struct bmi088_odr { int odr; int uodr; };

/*
 * Accelerometer ODR.  ACC_CONF register bits[3:0].
 * The register also controls bandwidth via bits[6:4] (BWP); we fix BWP=normal
 * and only expose the ODR here.
 */
static const struct bmi088_odr bmi088_accel_odr[] = {
	{ 12,  500000 },	/* 12.5 Hz */
	{ 25,       0 },
	{ 50,       0 },
	{ 100,      0 },
	{ 200,      0 },
	{ 400,      0 },
	{ 800,      0 },
	{ 1600,     0 },
};

static const u8 bmi088_accel_odr_vals[] = {
	0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
};

/*
 * Gyroscope ODR.  GYRO_BANDWIDTH register bits[3:0].
 * Each entry also implicitly selects the filter bandwidth.
 */
static const struct bmi088_odr bmi088_gyro_odr[] = {
	{  100, 0 },	/* reg=0x05: 100 Hz ODR,  12 Hz filter */
	{  200, 0 },	/* reg=0x04: 200 Hz ODR,  23 Hz filter */
	{  400, 0 },	/* reg=0x03: 400 Hz ODR,  47 Hz filter */
	{ 1000, 0 },	/* reg=0x02: 1000 Hz ODR, 116 Hz filter */
	{ 2000, 0 },	/* reg=0x01: 2000 Hz ODR, 230 Hz filter */
};

static const u8 bmi088_gyro_odr_vals[] = {
	0x05, 0x04, 0x03, 0x02, 0x01,
};

struct bmi088_odr_item {
	const struct bmi088_odr *tbl;
	const u8 *vals;
	int num;
};

static const struct bmi088_odr_item bmi088_odr_table[] = {
	[BMI088_ACCEL] = { .tbl  = bmi088_accel_odr,
			   .vals = bmi088_accel_odr_vals,
			   .num  = ARRAY_SIZE(bmi088_accel_odr) },
	[BMI088_GYRO]  = { .tbl  = bmi088_gyro_odr,
			   .vals = bmi088_gyro_odr_vals,
			   .num  = ARRAY_SIZE(bmi088_gyro_odr) },
};

/* --------------------------------------------------------------------------
 * Private driver data
 * -------------------------------------------------------------------------- */
struct bmi088_data {
	struct device      *dev;
	struct regmap      *acc_regmap;
	struct regmap      *gyr_regmap;
	struct mutex        mutex;
	struct iio_trigger *trig;

	/*
	 * DMA-safe capture buffer.  Align to 8 bytes for IIO timestamp.
	 * acc_channels[0..2]: X, Y, Z from the accelerometer slave.
	 * gyr_channels[0..2]: X, Y, Z from the gyroscope slave.
	 * Both sub-groups are read in bmi088_trigger_handler() and stored
	 * contiguously so that iio_push_to_buffers_with_timestamp() works.
	 *
	 * IIO_DMA_MINALIGN may exceed 8 bytes; pad accordingly.
	 */
	struct {
		__le16      acc_channels[3];
		__le16      gyr_channels[3];
		s64 __aligned(8) timestamp;
	} buffer __aligned(8);
};

/* Scan channel indices (must match bmi088_channels[] order below) */
enum bmi088_scan {
	BMI088_SCAN_ACCEL_X,
	BMI088_SCAN_ACCEL_Y,
	BMI088_SCAN_ACCEL_Z,
	BMI088_SCAN_GYRO_X,
	BMI088_SCAN_GYRO_Y,
	BMI088_SCAN_GYRO_Z,
	BMI088_SCAN_TIMESTAMP,
};

/* Only a single mask: all six motion channels at once */
static const unsigned long bmi088_avail_scan_masks[] = {
	BIT(BMI088_SCAN_ACCEL_X) | BIT(BMI088_SCAN_ACCEL_Y) |
	BIT(BMI088_SCAN_ACCEL_Z) | BIT(BMI088_SCAN_GYRO_X)  |
	BIT(BMI088_SCAN_GYRO_Y)  | BIT(BMI088_SCAN_GYRO_Z),
	0
};

/* --------------------------------------------------------------------------
 * Scale helpers
 * -------------------------------------------------------------------------- */
static int bmi088_get_scale(struct bmi088_data *data, int chan_type,
			    int *val, int *val2)
{
	struct bmi088_scale_item item;
	unsigned int regval;
	int ret;

	switch (chan_type) {
	case IIO_ACCEL:
		ret = regmap_read(data->acc_regmap, BMI088_ACC_RANGE_REG, &regval);
		if (ret)
			return ret;
		regval = FIELD_GET(BMI088_ACC_RANGE_MSK, regval);
		item = bmi088_scale_table[BMI088_ACCEL];
		break;
	case IIO_ANGL_VEL:
		ret = regmap_read(data->gyr_regmap, BMI088_GYR_RANGE_REG, &regval);
		if (ret)
			return ret;
		regval = FIELD_GET(BMI088_GYR_RANGE_MSK, regval);
		item = bmi088_scale_table[BMI088_GYRO];
		break;
	case IIO_TEMP:
		*val  = bmi088_temp_scale[0].val;
		*val2 = bmi088_temp_scale[0].val2;
		return 0;
	default:
		return -EINVAL;
	}

	if (regval >= (unsigned int)item.num)
		return -EINVAL;

	*val  = item.tbl[regval].val;
	*val2 = item.tbl[regval].val2;
	return 0;
}

static int bmi088_set_scale(struct bmi088_data *data, int chan_type, int val2)
{
	struct bmi088_scale_item item;
	int reg, mask, i;

	switch (chan_type) {
	case IIO_ACCEL:
		reg  = BMI088_ACC_RANGE_REG;
		mask = BMI088_ACC_RANGE_MSK;
		item = bmi088_scale_table[BMI088_ACCEL];
		for (i = 0; i < item.num; i++) {
			if (item.tbl[i].val2 != val2)
				continue;
			return regmap_update_bits(data->acc_regmap, reg, mask, i);
		}
		break;
	case IIO_ANGL_VEL:
		reg  = BMI088_GYR_RANGE_REG;
		mask = BMI088_GYR_RANGE_MSK;
		item = bmi088_scale_table[BMI088_GYRO];
		for (i = 0; i < item.num; i++) {
			if (item.tbl[i].val2 != val2)
				continue;
			return regmap_update_bits(data->gyr_regmap, reg, mask, i);
		}
		break;
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

/* --------------------------------------------------------------------------
 * ODR helpers
 * -------------------------------------------------------------------------- */
static int bmi088_get_odr(struct bmi088_data *data, int chan_type,
			  int *odr, int *uodr)
{
	struct bmi088_odr_item item;
	int val, ret, i;

	switch (chan_type) {
	case IIO_ACCEL:
		ret = regmap_read(data->acc_regmap, BMI088_ACC_CONF_REG, &val);
		if (ret)
			return ret;
		val  = FIELD_GET(BMI088_ACC_CONF_ODR_MSK, val);
		item = bmi088_odr_table[BMI088_ACCEL];
		break;
	case IIO_ANGL_VEL:
		ret = regmap_read(data->gyr_regmap, BMI088_GYR_BANDWIDTH_REG, &val);
		if (ret)
			return ret;
		val  = FIELD_GET(BMI088_GYR_BANDWIDTH_MSK, val);
		item = bmi088_odr_table[BMI088_GYRO];
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < item.num; i++) {
		if (val != item.vals[i])
			continue;
		*odr  = item.tbl[i].odr;
		*uodr = item.tbl[i].uodr;
		return 0;
	}
	return -EINVAL;
}

static int bmi088_set_odr(struct bmi088_data *data, int chan_type,
			  int odr, int uodr)
{
	struct bmi088_odr_item item;
	int i;

	switch (chan_type) {
	case IIO_ACCEL:
		item = bmi088_odr_table[BMI088_ACCEL];
		for (i = 0; i < item.num; i++) {
			if (item.tbl[i].odr != odr || item.tbl[i].uodr != uodr)
				continue;
			return regmap_update_bits(data->acc_regmap,
						  BMI088_ACC_CONF_REG,
						  BMI088_ACC_CONF_ODR_MSK,
						  item.vals[i]);
		}
		break;
	case IIO_ANGL_VEL:
		item = bmi088_odr_table[BMI088_GYRO];
		for (i = 0; i < item.num; i++) {
			if (item.tbl[i].odr != odr || item.tbl[i].uodr != uodr)
				continue;
			return regmap_update_bits(data->gyr_regmap,
						  BMI088_GYR_BANDWIDTH_REG,
						  BMI088_GYR_BANDWIDTH_MSK,
						  item.vals[i]);
		}
		break;
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

/* --------------------------------------------------------------------------
 * Data readout
 * -------------------------------------------------------------------------- */

/*
 * bmi088_read_temperature() - Read the on-chip temperature from the
 * accelerometer slave (registers 0x22–0x23).
 *
 * The raw value is an 11-bit two's complement integer:
 *   temp_raw11 = (TEMP_MSB[7:0] << 3) | (TEMP_LSB[7:5])
 *   temp_celsius = temp_raw11 * 0.125 + 23
 *
 * This function returns the signed 11-bit raw value. IIO applies the
 * OFFSET (184) and SCALE (0.125 °C) automatically.
 */
static int bmi088_read_temperature(struct bmi088_data *data, int *val)
{
	u8 buf[2];
	int ret;
	s16 raw;

	ret = regmap_bulk_read(data->acc_regmap, BMI088_TEMP_MSB_REG,
			       buf, sizeof(buf));
	if (ret)
		return ret;

	/* Assemble 11-bit value; buf[0]=MSB, buf[1]=LSB (bits 7:5 only) */
	raw = (s16)((buf[0] << BMI088_TEMP_MSB_SHIFT) |
		    (buf[1] >> BMI088_TEMP_LSB_SHIFT));
	*val = sign_extend32((u32)raw, BMI088_TEMP_SIGN_BIT);
	return IIO_VAL_INT;
}

static int bmi088_get_data(struct bmi088_data *data, int chan_type,
			   int axis, int *val)
{
	__le16 sample;
	int reg, ret;

	switch (chan_type) {
	case IIO_ACCEL:
		/*
		 * ACC_X_LSB=0x12, ACC_Y_LSB=0x14, ACC_Z_LSB=0x16
		 * Each axis occupies two consecutive bytes (LSB, MSB).
		 */
		reg = BMI088_ACC_X_LSB_REG + (axis - IIO_MOD_X) * 2;
		ret = regmap_bulk_read(data->acc_regmap, reg,
				       &sample, sizeof(sample));
		break;
	case IIO_ANGL_VEL:
		/*
		 * RATE_X_LSB=0x02, RATE_Y_LSB=0x04, RATE_Z_LSB=0x06
		 */
		reg = BMI088_GYR_X_LSB_REG + (axis - IIO_MOD_X) * 2;
		ret = regmap_bulk_read(data->gyr_regmap, reg,
				       &sample, sizeof(sample));
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	*val = sign_extend32(le16_to_cpu(sample), 15);
	return IIO_VAL_INT;
}

/* --------------------------------------------------------------------------
 * IIO ops
 * -------------------------------------------------------------------------- */
static int bmi088_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bmi088_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;
		if (chan->type == IIO_TEMP)
			ret = bmi088_read_temperature(data, val);
		else
			ret = bmi088_get_data(data, chan->type, chan->channel2, val);
		iio_device_release_direct_mode(indio_dev);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		mutex_lock(&data->mutex);
		ret = bmi088_get_scale(data, chan->type, val, val2);
		mutex_unlock(&data->mutex);
		return ret ? ret : IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_OFFSET:
		/* Only the temperature channel has a non-zero offset */
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = BMI088_TEMP_OFFSET;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&data->mutex);
		ret = bmi088_get_odr(data, chan->type, val, val2);
		mutex_unlock(&data->mutex);
		return ret ? ret : IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int bmi088_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct bmi088_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;
		mutex_lock(&data->mutex);
		ret = bmi088_set_scale(data, chan->type, val2);
		mutex_unlock(&data->mutex);
		iio_device_release_direct_mode(indio_dev);
		return ret;

	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;
		mutex_lock(&data->mutex);
		ret = bmi088_set_odr(data, chan->type, val, val2);
		mutex_unlock(&data->mutex);
		iio_device_release_direct_mode(indio_dev);
		return ret;

	default:
		return -EINVAL;
	}
}

static int bmi088_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_INT_PLUS_MICRO;
		switch (chan->type) {
		case IIO_ACCEL:
			*vals   = (const int *)bmi088_accel_scale;
			*length = ARRAY_SIZE(bmi088_accel_scale) * 2;
			return IIO_AVAIL_LIST;
		case IIO_ANGL_VEL:
			*vals   = (const int *)bmi088_gyro_scale;
			*length = ARRAY_SIZE(bmi088_gyro_scale) * 2;
			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT_PLUS_MICRO;
		switch (chan->type) {
		case IIO_ACCEL:
			*vals   = (const int *)bmi088_accel_odr;
			*length = ARRAY_SIZE(bmi088_accel_odr) * 2;
			return IIO_AVAIL_LIST;
		case IIO_ANGL_VEL:
			*vals   = (const int *)bmi088_gyro_odr;
			*length = ARRAY_SIZE(bmi088_gyro_odr) * 2;
			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static const struct iio_info bmi088_info = {
	.read_raw   = bmi088_read_raw,
	.write_raw  = bmi088_write_raw,
	.read_avail = bmi088_read_avail,
};

/* --------------------------------------------------------------------------
 * Triggered buffer
 * -------------------------------------------------------------------------- */
static irqreturn_t bmi088_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf       = p;
	struct iio_dev       *indio_dev = pf->indio_dev;
	struct bmi088_data   *data      = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->mutex);

	/* Accelerometer: read X/Y/Z in a single 6-byte burst */
	ret = regmap_bulk_read(data->acc_regmap,
			       BMI088_ACC_X_LSB_REG,
			       data->buffer.acc_channels,
			       sizeof(data->buffer.acc_channels));
	if (ret)
		goto done;

	/* Gyroscope: read X/Y/Z in a single 6-byte burst */
	ret = regmap_bulk_read(data->gyr_regmap,
			       BMI088_GYR_X_LSB_REG,
			       data->buffer.gyr_channels,
			       sizeof(data->buffer.gyr_channels));
done:
	mutex_unlock(&data->mutex);
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev, &data->buffer,
					   pf->timestamp);
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* --------------------------------------------------------------------------
 * IIO channel specifications
 * -------------------------------------------------------------------------- */
#define BMI088_ACCEL_CHANNEL(_axis)					\
{									\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type =					\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.info_mask_shared_by_type_available =				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.scan_index = BMI088_SCAN_ACCEL_##_axis,			\
	.scan_type = {							\
		.sign        = 's',					\
		.realbits    = 16,					\
		.storagebits = 16,					\
		.endianness  = IIO_LE,					\
	},								\
}

#define BMI088_GYRO_CHANNEL(_axis)					\
{									\
	.type = IIO_ANGL_VEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type =					\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.info_mask_shared_by_type_available =				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.scan_index = BMI088_SCAN_GYRO_##_axis,				\
	.scan_type = {							\
		.sign        = 's',					\
		.realbits    = 16,					\
		.storagebits = 16,					\
		.endianness  = IIO_LE,					\
	},								\
}

static const struct iio_chan_spec bmi088_channels[] = {
	BMI088_ACCEL_CHANNEL(X),
	BMI088_ACCEL_CHANNEL(Y),
	BMI088_ACCEL_CHANNEL(Z),
	BMI088_GYRO_CHANNEL(X),
	BMI088_GYRO_CHANNEL(Y),
	BMI088_GYRO_CHANNEL(Z),
	{
		/* Temperature sensor (accelerometer slave, no buffer) */
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)    |
				      BIT(IIO_CHAN_INFO_SCALE)  |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = -1,
	},
	IIO_CHAN_SOFT_TIMESTAMP(BMI088_SCAN_TIMESTAMP),
};

/* --------------------------------------------------------------------------
 * Chip initialisation
 * -------------------------------------------------------------------------- */
static int bmi088_init_acc(struct bmi088_data *data)
{
	unsigned int dummy;
	int ret;

	/*
	 * The BMI088 datasheet (section 4.2) states that after power-on the
	 * accelerometer I2C interface is disabled until the first read of the
	 * CHIP_ID register.  Perform a dummy read here to activate the I2C
	 * interface before issuing any write (including the soft-reset below).
	 */
	regmap_read(data->acc_regmap, BMI088_ACC_CHIP_ID_REG, &dummy);

	/*
	 * Soft-reset the accelerometer.  After reset the chip is in suspend
	 * mode and requires at least 1 ms before accepting further commands.
	 */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_SOFTRESET_REG,
			   BMI088_SOFTRESET_VAL);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to soft-reset accelerometer\n");
	usleep_range(BMI088_ACC_RESET_DELAY_US,
		     BMI088_ACC_RESET_DELAY_US * 2);

	/*
	 * After soft-reset, the I2C interface is disabled again — perform
	 * another dummy read of CHIP_ID to re-enable it before the real read.
	 */
	regmap_read(data->acc_regmap, BMI088_ACC_CHIP_ID_REG, &dummy);

	{
		unsigned int chip_id;

		ret = regmap_read(data->acc_regmap, BMI088_ACC_CHIP_ID_REG,
				  &chip_id);
		if (ret)
			return dev_err_probe(data->dev, ret,
					     "Failed to read accelerometer chip ID\n");
		if (chip_id != BMI088_ACC_CHIP_ID_VAL)
			return dev_err_probe(data->dev, -ENODEV,
					     "Unexpected acc chip ID 0x%02x (expected 0x%02x)\n",
					     chip_id, BMI088_ACC_CHIP_ID_VAL);
	}

	/* Exit suspend mode (ACC_PWR_CONF = 0x00 → active) */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_PWR_CONF_REG,
			   BMI088_ACC_PWR_CONF_ACTIVE);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to activate accelerometer\n");
	usleep_range(BMI088_ACC_PWR_SWITCH_DELAY_US,
		     BMI088_ACC_PWR_SWITCH_DELAY_US * 2);

	/* Switch on the accelerometer (ACC_PWR_CTRL bit[2] = 1) */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_PWR_CTRL_REG,
			   BMI088_ACC_PWR_CTRL_ENABLE);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to enable accelerometer\n");
	/* ≥ 50 ms for first valid sample after enable */
	usleep_range(BMI088_ACC_ENABLE_DELAY_US,
		     BMI088_ACC_ENABLE_DELAY_US + 5000);

	/* Default: 100 Hz ODR, normal bandwidth */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_CONF_REG,
			   FIELD_PREP(BMI088_ACC_CONF_ODR_MSK,
				      BMI088_ACC_CONF_ODR_100HZ) |
			   FIELD_PREP(BMI088_ACC_CONF_BWP_MSK,
				      BMI088_ACC_CONF_BWP_NORMAL));
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to configure accelerometer ODR\n");

	/* Default range: ±6 g (register value 0x01) */
	return regmap_write(data->acc_regmap, BMI088_ACC_RANGE_REG, 0x01);
}

static int bmi088_init_gyr(struct bmi088_data *data)
{
	unsigned int chip_id;
	int ret;

	/*
	 * The Bosch SensorAPI (bmi08g_init) does NOT issue a soft-reset during
	 * gyroscope initialisation — it only reads the chip ID.  The gyroscope
	 * powers on in NORMAL mode with known default register values, so a
	 * soft-reset is unnecessary.  Issuing one causes a NACK on some
	 * hardware configurations.
	 */
	ret = regmap_read(data->gyr_regmap, BMI088_GYR_CHIP_ID_REG, &chip_id);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to read gyroscope chip ID\n");
	if (chip_id != BMI088_GYR_CHIP_ID_VAL)
		return dev_err_probe(data->dev, -ENODEV,
				     "Unexpected gyr chip ID 0x%02x (expected 0x%02x)\n",
				     chip_id, BMI088_GYR_CHIP_ID_VAL);

	/* Ensure normal power mode (gyro defaults to normal after reset) */
	ret = regmap_write(data->gyr_regmap, BMI088_GYR_LPM1_REG,
			   BMI088_GYR_LPM1_NORMAL);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to set gyroscope power mode\n");

	/* Default: 100 Hz ODR, 12 Hz filter bandwidth */
	ret = regmap_write(data->gyr_regmap, BMI088_GYR_BANDWIDTH_REG,
			   BMI088_GYR_BANDWIDTH_100HZ);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Failed to configure gyroscope ODR\n");

	/* Default range: ±2000 °/s (register value 0x00) */
	return regmap_write(data->gyr_regmap, BMI088_GYR_RANGE_REG, 0x00);
}

/* --------------------------------------------------------------------------
 * Runtime PM
 * -------------------------------------------------------------------------- */
#ifdef CONFIG_PM
static int bmi088_core_runtime_suspend(struct device *dev)
{
	struct iio_dev     *indio_dev = dev_get_drvdata(dev);
	struct bmi088_data *data      = iio_priv(indio_dev);
	int ret;

	/* Accelerometer → suspend */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_PWR_CONF_REG,
			   BMI088_ACC_PWR_CONF_SUSPEND);
	if (ret)
		return ret;

	/* Gyroscope → suspend */
	return regmap_write(data->gyr_regmap, BMI088_GYR_LPM1_REG,
			    BMI088_GYR_LPM1_SUSPEND);
}

static int bmi088_core_runtime_resume(struct device *dev)
{
	struct iio_dev     *indio_dev = dev_get_drvdata(dev);
	struct bmi088_data *data      = iio_priv(indio_dev);
	int ret;

	/* Accelerometer → active */
	ret = regmap_write(data->acc_regmap, BMI088_ACC_PWR_CONF_REG,
			   BMI088_ACC_PWR_CONF_ACTIVE);
	if (ret)
		return ret;
	usleep_range(BMI088_ACC_PWR_SWITCH_DELAY_US,
		     BMI088_ACC_PWR_SWITCH_DELAY_US * 2);

	/* Gyroscope → normal */
	return regmap_write(data->gyr_regmap, BMI088_GYR_LPM1_REG,
			   BMI088_GYR_LPM1_NORMAL);
}
#endif /* CONFIG_PM */

const struct dev_pm_ops bmi088_core_pm_ops = {
	SET_RUNTIME_PM_OPS(bmi088_core_runtime_suspend,
			   bmi088_core_runtime_resume, NULL)
};
EXPORT_SYMBOL_NS_GPL(bmi088_core_pm_ops, IIO_BMI088);

/* --------------------------------------------------------------------------
 * Core probe  (called by bus-specific drivers after creating regmaps)
 * -------------------------------------------------------------------------- */
int bmi088_core_probe(struct device *dev,
		      struct regmap *acc_regmap,
		      struct regmap *gyr_regmap)
{
	struct bmi088_data *data;
	struct iio_dev     *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data             = iio_priv(indio_dev);
	data->dev        = dev;
	data->acc_regmap = acc_regmap;
	data->gyr_regmap = gyr_regmap;
	mutex_init(&data->mutex);

	ret = bmi088_init_acc(data);
	if (ret)
		return ret;

	ret = bmi088_init_gyr(data);
	if (ret)
		return ret;

	indio_dev->channels             = bmi088_channels;
	indio_dev->num_channels         = ARRAY_SIZE(bmi088_channels);
	indio_dev->name                 = "bmi088";
	indio_dev->available_scan_masks = bmi088_avail_scan_masks;
	indio_dev->modes                = INDIO_DIRECT_MODE;
	indio_dev->info                 = &bmi088_info;
	dev_set_drvdata(dev, indio_dev);

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      bmi088_trigger_handler, NULL);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS_GPL(bmi088_core_probe, IIO_BMI088);

MODULE_AUTHOR("Your Name <your@email.com>");
MODULE_DESCRIPTION("Bosch BMI088 6-Axis IMU core driver");
MODULE_LICENSE("GPL");
