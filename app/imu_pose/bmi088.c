#define _GNU_SOURCE

#include "imu_pose.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif

#define BMI088_ACCEL_CHIP_ID_REG 0x00
#define BMI088_ACCEL_CHIP_ID 0x1E
#define BMI088_ACCEL_DATA_REG 0x12
#define BMI088_ACCEL_CONF_REG 0x40
#define BMI088_ACCEL_RANGE_REG 0x41
#define BMI088_ACCEL_PWR_CONF_REG 0x7C
#define BMI088_ACCEL_PWR_CTRL_REG 0x7D
#define BMI088_ACCEL_SOFTRESET_REG 0x7E

#define BMI088_GYRO_CHIP_ID_REG 0x00
#define BMI088_GYRO_CHIP_ID 0x0F
#define BMI088_GYRO_DATA_REG 0x02
#define BMI088_GYRO_RANGE_REG 0x0F
#define BMI088_GYRO_BANDWIDTH_REG 0x10
#define BMI088_GYRO_LPM1_REG 0x11
#define BMI088_GYRO_SOFTRESET_REG 0x14

static int i2c_set_addr(int fd, unsigned int addr, bool force)
{
    if (ioctl(fd, force ? I2C_SLAVE_FORCE : I2C_SLAVE, addr) < 0)
        return -1;
    return 0;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *value)
{
    uint8_t buf;

    if (write(fd, &reg, 1) != 1)
        return -1;
    if (read(fd, &buf, 1) != 1)
        return -1;
    *value = buf;
    return 0;
}

static int i2c_write_reg(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};

    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
        return -1;
    return 0;
}

static int i2c_read_regs(int fd, uint8_t reg, uint8_t *data, size_t len)
{
    if (write(fd, &reg, 1) != 1)
        return -1;
    if (read(fd, data, len) != (ssize_t)len)
        return -1;
    return 0;
}

static int i2c_rdwr_read_regs(int fd, unsigned int addr, uint8_t reg, uint8_t *data, size_t len)
{
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data ioctl_data;

    memset(msgs, 0, sizeof(msgs));
    msgs[0].addr = (uint16_t)addr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = (uint16_t)addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = (uint16_t)len;
    msgs[1].buf = data;

    ioctl_data.msgs = msgs;
    ioctl_data.nmsgs = 2;
    if (ioctl(fd, I2C_RDWR, &ioctl_data) < 0)
        return -1;
    return 0;
}

static int open_i2c_device(unsigned int bus, unsigned int addr, bool force)
{
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/dev/i2c-%u", bus);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (i2c_set_addr(fd, addr, force) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int bmi088_init(bmi088_t *imu, const app_config_t *cfg)
{
    uint8_t id;

    memset(imu, 0, sizeof(*imu));
    imu->accel_fd = -1;
    imu->gyro_fd = -1;
    imu->accel_addr = cfg->accel_addr;
    imu->gyro_addr = cfg->gyro_addr;
    imu->accel_lsb_to_mps2 = GRAVITY_MPS2 / 5460.0f;
    imu->gyro_lsb_to_rad_s = (250.0f / 32768.0f) * DEG_TO_RAD;
    imu->use_i2c_rdwr = cfg->i2c_rdwr;

    imu->accel_fd = open_i2c_device(cfg->i2c_bus, cfg->accel_addr, cfg->i2c_force);
    if (imu->accel_fd < 0)
        return -1;
    imu->gyro_fd = open_i2c_device(cfg->i2c_bus, cfg->gyro_addr, cfg->i2c_force);
    if (imu->gyro_fd < 0)
        return -1;

    if (i2c_read_reg(imu->accel_fd, BMI088_ACCEL_CHIP_ID_REG, &id) < 0 ||
        id != BMI088_ACCEL_CHIP_ID) {
        errno = ENODEV;
        return -1;
    }
    if (i2c_read_reg(imu->gyro_fd, BMI088_GYRO_CHIP_ID_REG, &id) < 0 ||
        id != BMI088_GYRO_CHIP_ID) {
        errno = ENODEV;
        return -1;
    }

    i2c_write_reg(imu->accel_fd, BMI088_ACCEL_SOFTRESET_REG, 0xB6);
    sleep_ms(50);
    i2c_write_reg(imu->gyro_fd, BMI088_GYRO_SOFTRESET_REG, 0xB6);
    sleep_ms(50);

    if (i2c_write_reg(imu->accel_fd, BMI088_ACCEL_PWR_CTRL_REG, 0x04) < 0)
        return -1;
    sleep_ms(5);
    if (i2c_write_reg(imu->accel_fd, BMI088_ACCEL_PWR_CONF_REG, 0x00) < 0)
        return -1;
    sleep_ms(5);
    if (i2c_write_reg(imu->accel_fd, BMI088_ACCEL_CONF_REG, 0xAC) < 0)
        return -1;
    if (i2c_write_reg(imu->accel_fd, BMI088_ACCEL_RANGE_REG, 0x01) < 0)
        return -1;

    if (i2c_write_reg(imu->gyro_fd, BMI088_GYRO_LPM1_REG, 0x00) < 0)
        return -1;
    sleep_ms(30);
    if (i2c_write_reg(imu->gyro_fd, BMI088_GYRO_RANGE_REG, 0x03) < 0)
        return -1;
    if (i2c_write_reg(imu->gyro_fd, BMI088_GYRO_BANDWIDTH_REG, 0x02) < 0)
        return -1;

    return 0;
}

void bmi088_close(bmi088_t *imu)
{
    if (imu->accel_fd >= 0)
        close(imu->accel_fd);
    if (imu->gyro_fd >= 0)
        close(imu->gyro_fd);
    imu->accel_fd = -1;
    imu->gyro_fd = -1;
}

int bmi088_read_sample(bmi088_t *imu, imu_sample_t *sample)
{
    uint8_t acc[6];
    uint8_t gyro[6];

    if (imu->use_i2c_rdwr) {
        if (i2c_rdwr_read_regs(imu->accel_fd, imu->accel_addr, BMI088_ACCEL_DATA_REG,
                               acc, sizeof(acc)) == 0 &&
            i2c_rdwr_read_regs(imu->gyro_fd, imu->gyro_addr, BMI088_GYRO_DATA_REG,
                               gyro, sizeof(gyro)) == 0)
            goto decode_sample;

        imu->use_i2c_rdwr = false;
    }

    if (i2c_read_regs(imu->accel_fd, BMI088_ACCEL_DATA_REG, acc, sizeof(acc)) < 0)
        return -1;
    if (i2c_read_regs(imu->gyro_fd, BMI088_GYRO_DATA_REG, gyro, sizeof(gyro)) < 0)
        return -1;

decode_sample:
    sample->accel_raw[0] = le16_to_i16(&acc[0]);
    sample->accel_raw[1] = le16_to_i16(&acc[2]);
    sample->accel_raw[2] = le16_to_i16(&acc[4]);
    sample->gyro_raw[0] = le16_to_i16(&gyro[0]);
    sample->gyro_raw[1] = le16_to_i16(&gyro[2]);
    sample->gyro_raw[2] = le16_to_i16(&gyro[4]);

    for (int i = 0; i < 3; i++) {
        sample->accel_mps2[i] = (float)sample->accel_raw[i] * imu->accel_lsb_to_mps2;
        sample->gyro_rad_s[i] = (float)sample->gyro_raw[i] * imu->gyro_lsb_to_rad_s;
    }

    return 0;
}
