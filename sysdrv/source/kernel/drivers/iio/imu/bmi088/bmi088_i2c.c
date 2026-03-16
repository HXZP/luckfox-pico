// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * Bosch BMI088 6-Axis IMU — I2C interface driver
 *
 * The BMI088 presents two independent I2C slaves that share the same SDO
 * pin for address selection:
 *
 *   Accelerometer:  0x18 (SDO=0) / 0x19 (SDO=1)
 *   Gyroscope:      0x68 (SDO=0) / 0x69 (SDO=1)
 *
 * The driver probes on the accelerometer address (listed in the device tree)
 * and internally creates a dummy I2C device for the gyroscope on the same
 * adapter, deriving its address from the accelerometer address.
 *
 * Example device-tree node:
 *
 *   &i2c1 {
 *       bmi088@18 {
 *           compatible = "bosch,bmi088";
 *           reg = <0x18>;                // accelerometer (SDO=0)
 *           // interrupt-parent = <&gpio>;
 *           // interrupts = <X IRQ_TYPE_EDGE_RISING>;
 *       };
 *   };
 *
 * (SDO=1 variant uses reg = <0x19>; gyroscope address is derived automatically.)
 */

#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pm.h>
#include <linux/regmap.h>

#include "bmi088.h"

#define BMI088_ACC_I2C_ADDR_SDO0	0x18
#define BMI088_ACC_I2C_ADDR_SDO1	0x19
#define BMI088_GYR_I2C_ADDR_SDO0	0x68
#define BMI088_GYR_I2C_ADDR_SDO1	0x69

/* Both slaves use a plain 8-bit address + 8-bit data regmap */
static const struct regmap_config bmi088_acc_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.name     = "bmi088-acc",
};

static const struct regmap_config bmi088_gyr_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.name     = "bmi088-gyr",
};

/* devm action: unregister the gyroscope dummy client on driver detach */
static void bmi088_i2c_release_gyro(void *gyro_client)
{
	i2c_unregister_device(gyro_client);
}

static int bmi088_i2c_probe(struct i2c_client *acc_client,
			    const struct i2c_device_id *id)
{
	struct device      *dev = &acc_client->dev;
	struct i2c_client  *gyr_client;
	struct regmap      *acc_regmap, *gyr_regmap;
	u16                 gyr_addr;
	int                 ret;

	/*
	 * Infer the gyroscope I2C address from the accelerometer address.
	 * Both slaves track the same SDO pin.
	 */
	gyr_addr = (acc_client->addr == BMI088_ACC_I2C_ADDR_SDO1)
		   ? BMI088_GYR_I2C_ADDR_SDO1
		   : BMI088_GYR_I2C_ADDR_SDO0;

	/*
	 * Create a dummy I2C client for the gyroscope slave so we can build
	 * a regmap against it.  i2c_new_dummy_device() was introduced in
	 * Linux 5.1 and is available in all supported kernel versions.
	 */
	gyr_client = i2c_new_dummy_device(acc_client->adapter, gyr_addr);
	if (IS_ERR(gyr_client))
		return dev_err_probe(dev, PTR_ERR(gyr_client),
				     "Failed to register gyroscope I2C client at 0x%02x\n",
				     gyr_addr);

	/*
	 * Tie the gyroscope client's lifetime to the accelerometer device so
	 * it is unregistered automatically when the driver is unbound.
	 */
	ret = devm_add_action_or_reset(dev, bmi088_i2c_release_gyro,
				       gyr_client);
	if (ret)
		return ret;

	acc_regmap = devm_regmap_init_i2c(acc_client,
					  &bmi088_acc_i2c_regmap_config);
	if (IS_ERR(acc_regmap))
		return dev_err_probe(dev, PTR_ERR(acc_regmap),
				     "Failed to initialise accelerometer regmap\n");

	gyr_regmap = devm_regmap_init_i2c(gyr_client,
					  &bmi088_gyr_i2c_regmap_config);
	if (IS_ERR(gyr_regmap))
		return dev_err_probe(dev, PTR_ERR(gyr_regmap),
				     "Failed to initialise gyroscope regmap\n");

	return bmi088_core_probe(dev, acc_regmap, gyr_regmap);
}

static const struct i2c_device_id bmi088_i2c_id[] = {
	{ "bmi088", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bmi088_i2c_id);

static const struct of_device_id bmi088_of_match[] = {
	{ .compatible = "bosch,bmi088" },
	{ }
};
MODULE_DEVICE_TABLE(of, bmi088_of_match);

static struct i2c_driver bmi088_i2c_driver = {
	.driver = {
		.name           = "bmi088_i2c",
		.pm             = pm_ptr(&bmi088_core_pm_ops),
		.of_match_table = bmi088_of_match,
	},
	.probe    = bmi088_i2c_probe,
	.id_table = bmi088_i2c_id,
};
module_i2c_driver(bmi088_i2c_driver);

MODULE_AUTHOR("Your Name <your@email.com>");
MODULE_DESCRIPTION("Bosch BMI088 IMU I2C driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_BMI088);
