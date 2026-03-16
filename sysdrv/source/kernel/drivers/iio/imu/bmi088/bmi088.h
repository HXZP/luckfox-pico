/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Bosch BMI088 6-Axis IMU — shared header
 *
 * The BMI088 integrates a 3-axis accelerometer and a 3-axis gyroscope in a
 * single package, but exposes them as two independent I2C/SPI slaves:
 *
 *   Accelerometer  I2C: 0x18 (SDO=0) / 0x19 (SDO=1)    Chip-ID: 0x1E
 *   Gyroscope      I2C: 0x68 (SDO=0) / 0x69 (SDO=1)    Chip-ID: 0x0F
 *
 * Datasheet: https://www.bosch-sensortec.com/media/boschsensortec/downloads/
 *            datasheets/bst-bmi088-ds001.pdf
 */

#ifndef BMI088_H_
#define BMI088_H_

#include <linux/iio/iio.h>
#include <linux/regmap.h>

struct device;

/**
 * bmi088_core_probe() - Initialise and register the BMI088 IIO device.
 * @dev:        Parent device (I2C or SPI).
 * @acc_regmap: Regmap bound to the accelerometer slave.
 * @gyr_regmap: Regmap bound to the gyroscope slave.
 *
 * Returns 0 on success, negative errno on failure.
 */
int bmi088_core_probe(struct device *dev,
		      struct regmap *acc_regmap,
		      struct regmap *gyr_regmap);

extern const struct dev_pm_ops bmi088_core_pm_ops;

#endif  /* BMI088_H_ */
