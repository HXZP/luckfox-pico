#define _GNU_SOURCE

#include "imu_pose.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int choose_accel_odr(unsigned int rate_hz)
{
    static const unsigned int odrs[] = {25, 50, 100, 200, 400, 800, 1600};

    for (size_t i = 0; i < sizeof(odrs) / sizeof(odrs[0]); i++) {
        if (rate_hz <= odrs[i])
            return odrs[i];
    }
    return odrs[sizeof(odrs) / sizeof(odrs[0]) - 1];
}

static unsigned int choose_gyro_odr(unsigned int rate_hz)
{
    static const unsigned int odrs[] = {100, 200, 400, 1000, 2000};

    for (size_t i = 0; i < sizeof(odrs) / sizeof(odrs[0]); i++) {
        if (rate_hz <= odrs[i])
            return odrs[i];
    }
    return odrs[sizeof(odrs) / sizeof(odrs[0]) - 1];
}

static int iio_find_trigger_by_name(const char *name, char *out_dir, size_t out_size)
{
    DIR *dir;
    struct dirent *entry;
    int found = -1;

    dir = opendir("/sys/bus/iio/devices");
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char trig_dir[320];
        char trig_name[96];

        if (strncmp(entry->d_name, "trigger", 7) != 0)
            continue;

        snprintf(trig_dir, sizeof(trig_dir), "/sys/bus/iio/devices/%s", entry->d_name);
        if (read_sysfs_path3(trig_dir, "name", trig_name, sizeof(trig_name)) < 0)
            continue;
        if (strcmp(trig_name, name) != 0)
            continue;

        snprintf(out_dir, out_size, "%s", trig_dir);
        found = 0;
        break;
    }

    closedir(dir);
    if (found < 0)
        errno = ENOENT;
    return found;
}

static int iio_find_bmi088_device(const char *selector, char *dev_dir, size_t dev_dir_size,
                                  char *dev_node, size_t dev_node_size)
{
    DIR *dir;
    struct dirent *entry;
    const char *base;

    if (selector && selector[0]) {
        struct stat st;
        char path[320];

        if (strncmp(selector, "/sys/", 5) == 0)
            snprintf(path, sizeof(path), "%s", selector);
        else if (strncmp(selector, "iio:device", 10) == 0)
            snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s", selector);
        else
            goto scan_by_name;

        if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            errno = ENOENT;
            return -1;
        }
        base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (strlen(base) + sizeof("/dev/") > dev_node_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        snprintf(dev_dir, dev_dir_size, "%s", path);
        strcpy(dev_node, "/dev/");
        strcat(dev_node, base);
        return 0;
    }

scan_by_name:
    dir = opendir("/sys/bus/iio/devices");
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char path[320];
        char name[96];

        if (strncmp(entry->d_name, "iio:device", 10) != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s", entry->d_name);
        if (read_sysfs_path3(path, "name", name, sizeof(name)) < 0)
            continue;
        if (selector && selector[0]) {
            if (strcmp(name, selector) != 0 && strstr(name, selector) == NULL)
                continue;
        } else if (strstr(name, "bmi088") == NULL) {
            continue;
        }

        snprintf(dev_dir, dev_dir_size, "%s", path);
        snprintf(dev_node, dev_node_size, "/dev/%s", entry->d_name);
        closedir(dir);
        return 0;
    }

    closedir(dir);
    errno = ENOENT;
    return -1;
}

static int iio_prepare_hrtimer_trigger(const char *trigger_name, unsigned int rate_hz,
                                       char *trigger_dir, size_t trigger_dir_size)
{
    char config_root[] = "/sys/kernel/config";
    char hrtimer_root[] = "/sys/kernel/config/iio/triggers/hrtimer";
    char trigger_cfg_dir[256];
    char value[32];
    int ret = -1;

    if (!path_exists(config_root) && mkdir(config_root, 0755) < 0 && errno != EEXIST)
        return -1;

    if (!path_exists(hrtimer_root)) {
        if (mount("configfs", config_root, "configfs", 0, NULL) < 0 && errno != EBUSY)
            return -1;
    }

    if (!path_exists(hrtimer_root)) {
        errno = ENOENT;
        return -1;
    }

    snprintf(trigger_cfg_dir, sizeof(trigger_cfg_dir), "%s/%s", hrtimer_root, trigger_name);
    if (mkdir(trigger_cfg_dir, 0755) < 0 && errno != EEXIST)
        return -1;

    for (unsigned int i = 0; i < 50; i++) {
        ret = iio_find_trigger_by_name(trigger_name, trigger_dir, trigger_dir_size);
        if (ret == 0)
            break;
        sleep_ms(10);
    }
    if (ret < 0)
        return -1;

    snprintf(value, sizeof(value), "%u", rate_hz);
    return write_sysfs_path3(trigger_dir, "sampling_frequency", value);
}

static int iio_enable_scan_channels(iio_backend_t *iio, bool *timestamp_enabled)
{
    static const char *motion_channels[] = {
        "scan_elements/in_accel_x_en",
        "scan_elements/in_accel_y_en",
        "scan_elements/in_accel_z_en",
        "scan_elements/in_anglvel_x_en",
        "scan_elements/in_anglvel_y_en",
        "scan_elements/in_anglvel_z_en",
    };

    *timestamp_enabled = false;

    for (size_t i = 0; i < sizeof(motion_channels) / sizeof(motion_channels[0]); i++)
        (void)write_sysfs_path3(iio->dev_dir, motion_channels[i], "0");
    (void)write_sysfs_path3(iio->dev_dir, "scan_elements/in_timestamp_en", "0");

    for (size_t i = 0; i < sizeof(motion_channels) / sizeof(motion_channels[0]); i++) {
        if (write_sysfs_path3(iio->dev_dir, motion_channels[i], "1") < 0)
            return -1;
    }

    if (write_sysfs_path3(iio->dev_dir, "scan_elements/in_timestamp_en", "1") == 0)
        *timestamp_enabled = true;
    return 0;
}

static int iio_configure_sensor(iio_backend_t *iio, const app_config_t *cfg)
{
    char value[32];
    unsigned int accel_odr = choose_accel_odr(cfg->rate_hz);
    unsigned int gyro_odr = choose_gyro_odr(cfg->rate_hz);

    (void)write_sysfs_path3(iio->dev_dir, "buffer/enable", "0");

    snprintf(value, sizeof(value), "%u", accel_odr);
    if (write_sysfs_path3(iio->dev_dir, "in_accel_sampling_frequency", value) < 0 && cfg->verbose)
        fprintf(stderr, "imu_pose: warning: cannot set accel ODR %uHz: %s\n",
                accel_odr, strerror(errno));

    snprintf(value, sizeof(value), "%u", gyro_odr);
    if (write_sysfs_path3(iio->dev_dir, "in_anglvel_sampling_frequency", value) < 0 && cfg->verbose)
        fprintf(stderr, "imu_pose: warning: cannot set gyro ODR %uHz: %s\n",
                gyro_odr, strerror(errno));

    (void)write_sysfs_path3(iio->dev_dir, "in_accel_scale", "0.001797");
    (void)write_sysfs_path3(iio->dev_dir, "in_anglvel_scale", "0.000133");

    if (cfg->verbose)
        fprintf(stderr, "imu_pose: IIO sensor ODR target accel=%uHz gyro=%uHz trigger=%uHz\n",
                accel_odr, gyro_odr, cfg->rate_hz);
    return 0;
}

int iio_init(iio_backend_t *iio, const app_config_t *cfg)
{
    char trigger_dir[320];
    char path[512];
    char value[32];
    bool timestamp_enabled;

    memset(iio, 0, sizeof(*iio));
    iio->fd = -1;
    iio->accel_lsb_to_mps2 = GRAVITY_MPS2 / 5460.0f;
    iio->gyro_lsb_to_rad_s = (250.0f / 32768.0f) * DEG_TO_RAD;

    if (iio_find_bmi088_device(cfg->iio_device, iio->dev_dir, sizeof(iio->dev_dir),
                               iio->dev_node, sizeof(iio->dev_node)) < 0)
        return -1;

    snprintf(iio->trigger_name, sizeof(iio->trigger_name), "%s",
             cfg->iio_trigger[0] ? cfg->iio_trigger : DEFAULT_IIO_TRIGGER_NAME);

    if (iio_prepare_hrtimer_trigger(iio->trigger_name, cfg->rate_hz,
                                    trigger_dir, sizeof(trigger_dir)) < 0)
        return -1;

    if (iio_configure_sensor(iio, cfg) < 0)
        return -1;

    if (iio_enable_scan_channels(iio, &timestamp_enabled) < 0)
        return -1;

    if (write_sysfs_path3(iio->dev_dir, "trigger/current_trigger", iio->trigger_name) < 0)
        return -1;

    snprintf(value, sizeof(value), "%u", cfg->iio_buffer_length);
    if (write_sysfs_path3(iio->dev_dir, "buffer/length", value) < 0)
        return -1;

    if (write_sysfs_path3(iio->dev_dir, "buffer/enable", "1") < 0)
        return -1;
    iio->buffer_enabled = true;

    iio->scan_bytes = timestamp_enabled ? 24U : 12U;
    iio->fd = open(iio->dev_node, O_RDONLY | O_CLOEXEC);
    if (iio->fd < 0)
        return -1;

    snprintf(path, sizeof(path), "%s/in_accel_scale", iio->dev_dir);
    if (read_float_file(path, &iio->accel_lsb_to_mps2) < 0 && cfg->verbose)
        fprintf(stderr, "imu_pose: warning: using fallback accel scale %.9f\n",
                iio->accel_lsb_to_mps2);
    snprintf(path, sizeof(path), "%s/in_anglvel_scale", iio->dev_dir);
    if (read_float_file(path, &iio->gyro_lsb_to_rad_s) < 0 && cfg->verbose)
        fprintf(stderr, "imu_pose: warning: using fallback gyro scale %.9f\n",
                iio->gyro_lsb_to_rad_s);

    if (cfg->verbose)
        fprintf(stderr, "imu_pose: IIO device=%s node=%s trigger=%s scan=%zuB scale acc=%.9f gyro=%.9f\n",
                iio->dev_dir, iio->dev_node, iio->trigger_name, iio->scan_bytes,
                iio->accel_lsb_to_mps2, iio->gyro_lsb_to_rad_s);
    return 0;
}

void iio_close(iio_backend_t *iio)
{
    if (iio->buffer_enabled) {
        (void)write_sysfs_path3(iio->dev_dir, "buffer/enable", "0");
        iio->buffer_enabled = false;
    }
    if (iio->fd >= 0)
        close(iio->fd);
    iio->fd = -1;
}

int iio_read_sample(iio_backend_t *iio, imu_sample_t *sample)
{
    uint8_t buf[32];
    size_t got = 0;

    if (iio->scan_bytes > sizeof(buf)) {
        errno = EINVAL;
        return -1;
    }

    while (got < iio->scan_bytes) {
        ssize_t n = read(iio->fd, buf + got, iio->scan_bytes - got);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        got += (size_t)n;
    }

    sample->accel_raw[0] = le16_to_i16(&buf[0]);
    sample->accel_raw[1] = le16_to_i16(&buf[2]);
    sample->accel_raw[2] = le16_to_i16(&buf[4]);
    sample->gyro_raw[0] = le16_to_i16(&buf[6]);
    sample->gyro_raw[1] = le16_to_i16(&buf[8]);
    sample->gyro_raw[2] = le16_to_i16(&buf[10]);

    for (int i = 0; i < 3; i++) {
        sample->accel_mps2[i] = (float)sample->accel_raw[i] * iio->accel_lsb_to_mps2;
        sample->gyro_rad_s[i] = (float)sample->gyro_raw[i] * iio->gyro_lsb_to_rad_s;
    }

    return 0;
}
