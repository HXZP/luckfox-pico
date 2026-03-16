// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * Bosch BMI088 6-Axis IMU — SPI interface driver (stub)
 *
 * The BMI088 exposes two independent SPI slaves — one for the accelerometer
 * (CS1) and one for the gyroscope (CS2) — each requiring its own chip-select.
 *
 * Key SPI details from the datasheet (section 6.3):
 *  - CPOL=0, CPHA=0 (mode 0) for both slaves.
 *  - Maximum SPI clock: 10 MHz (accelerometer), 10 MHz (gyroscope).
 *  - The accelerometer slave inserts one dummy byte after the address byte
 *    on READ operations (same behaviour as BMI270); the gyroscope slave
 *    does NOT require a dummy byte.
 *  - MSB of the address byte is the R/W bit (1=read, 0=write).
 *
 * Full SPI dual-slave support is pending; this stub allows the module to be
 * compiled and linked without breaking the build when SPI is selected.
 */

#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include "bmi088.h"

static int bmi088_spi_probe(struct spi_device *spi)
{
	return dev_err_probe(&spi->dev, -ENOSYS,
			     "BMI088 SPI interface is not yet implemented\n");
}

static const struct spi_device_id bmi088_spi_id[] = {
	{ "bmi088", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, bmi088_spi_id);

static const struct of_device_id bmi088_spi_of_match[] = {
	{ .compatible = "bosch,bmi088-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, bmi088_spi_of_match);

static struct spi_driver bmi088_spi_driver = {
	.driver = {
		.name           = "bmi088_spi",
		.pm             = pm_ptr(&bmi088_core_pm_ops),
		.of_match_table = bmi088_spi_of_match,
	},
	.probe    = bmi088_spi_probe,
	.id_table = bmi088_spi_id,
};
module_spi_driver(bmi088_spi_driver);

MODULE_AUTHOR("Your Name <your@email.com>");
MODULE_DESCRIPTION("Bosch BMI088 IMU SPI driver (stub)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_BMI088");
