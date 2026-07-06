#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif

#define DEFAULT_I2C_BUS 2
#define DEFAULT_ACCEL_ADDR 0x18
#define DEFAULT_GYRO_ADDR 0x68
#define DEFAULT_RATE_HZ 1000U
#define DEFAULT_REPORT_MS 1000U
#define DEFAULT_DURATION_S 10U
#define DEFAULT_WAKE_MARGIN_US 50U
#define DEFAULT_FIFO_PRIORITY 60U
#define DEFAULT_IIO_TRIGGER_NAME "imu_pose_hrtimer"
#define DEFAULT_IIO_BUFFER_LENGTH 32U

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

#define GRAVITY_MPS2 9.80665f
#define RAD_TO_DEG 57.29577951308232f
#define DEG_TO_RAD 0.017453292519943295f

#define POSE_UPDATE_ERROR -1
#define POSE_UPDATE_OK 0
#define POSE_UPDATE_CALIBRATING 1

typedef struct {
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    float accel_mps2[3];
    float gyro_rad_s[3];
} imu_sample_t;

typedef struct {
    unsigned int sample_rate_hz;
    float kp;
    float ki;
    float gyro_offset_rad_s[3];
    bool gyro_auto_offset_enable;
    unsigned int gyro_auto_offset_sample_count;
    float gyro_auto_offset_limit_rad_s;
    float accel_norm_ref_mps2;
    float accel_trust_min_mps2;
    float accel_trust_max_mps2;
    float accel_static_min_mps2;
    float accel_static_max_mps2;
    float gyro_static_limit_rad_s;
} pose_config_t;

typedef struct {
    const pose_config_t *cfg;
    float q[4];
    float accel_mps2[3];
    float gyro_rad_s[3];
    float gyro_auto_offset_rad_s[3];
    float accel_norm_mps2;
    float accel_confidence;
    float effective_kp;
    float effective_ki;
    float ex_int;
    float ey_int;
    float ez_int;
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
    float sample_period_s;
    unsigned int gyro_auto_offset_count;
    bool gyro_auto_offset_done;
    bool static_detected;
    bool ready;
} pose_filter_t;

typedef struct {
    unsigned int i2c_bus;
    unsigned int accel_addr;
    unsigned int gyro_addr;
    unsigned int rate_hz;
    unsigned int report_ms;
    unsigned int duration_s;
    unsigned int wake_margin_us;
    unsigned int fifo_priority;
    unsigned int iio_buffer_length;
    int cpu_affinity;
    bool simulate;
    bool iio;
    bool i2c_force;
    bool i2c_rdwr;
    bool fixed_dt;
    bool verbose;
    bool realtime;
    char iio_device[64];
    char iio_trigger[64];
} app_config_t;

typedef struct {
    int accel_fd;
    int gyro_fd;
    unsigned int accel_addr;
    unsigned int gyro_addr;
    float accel_lsb_to_mps2;
    float gyro_lsb_to_rad_s;
    bool use_i2c_rdwr;
} bmi088_t;

typedef struct {
    int fd;
    char dev_dir[320];
    char dev_node[320];
    char trigger_name[64];
    float accel_lsb_to_mps2;
    float gyro_lsb_to_rad_s;
    size_t scan_bytes;
    bool buffer_enabled;
} iio_backend_t;

typedef struct {
    unsigned long long count;
    unsigned long long late_count;
    unsigned long long read_error_count;
    unsigned long long update_error_count;
    int64_t min_period_ns;
    int64_t max_period_ns;
    long double sum_period_ns;
    long double sum_read_ns;
    long double sum_update_ns;
    int64_t max_read_ns;
    int64_t max_update_ns;
} loop_stats_t;

static int64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until_ns(int64_t deadline_ns)
{
    struct timespec ts;

    if (deadline_ns < 0)
        deadline_ns = 0;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000LL);
    ts.tv_nsec = (long)(deadline_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) < 0 && errno == EINTR)
        ;
}

static void wait_until_ns(int64_t deadline_ns, int64_t busy_margin_ns)
{
    int64_t sleep_deadline_ns;

    if (busy_margin_ns <= 0) {
        sleep_until_ns(deadline_ns);
        return;
    }

    sleep_deadline_ns = deadline_ns - busy_margin_ns;
    if (sleep_deadline_ns > now_ns())
        sleep_until_ns(sleep_deadline_ns);
    while (now_ns() < deadline_ns)
        ;
}

static int parse_u32(const char *text, unsigned int min, unsigned int max, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno || end == text || *end || value < min || value > max)
        return -1;
    *out = (unsigned int)value;
    return 0;
}

static int parse_i32(const char *text, int min, int max, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 0);
    if (errno || end == text || *end || value < min || value > max)
        return -1;
    *out = (int)value;
    return 0;
}

static int parse_float_arg(const char *text, float *out)
{
    char *end = NULL;
    float value;

    errno = 0;
    value = strtof(text, &end);
    if (errno || end == text || *end)
        return -1;
    *out = value;
    return 0;
}

static float clamp_f32(float value, float min, float max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

static float half_cycle_f32(float value, float cycle)
{
    float half = cycle / 2.0f;

    while (value >= half)
        value -= cycle;
    while (value < -half)
        value += cycle;
    return value;
}

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

static void sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int read_text_file(const char *path, char *buf, size_t size)
{
    int fd;
    ssize_t n;
    int saved_errno;

    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1);
    saved_errno = errno;
    close(fd);
    if (n < 0) {
        errno = saved_errno;
        return -1;
    }

    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        n--;
    }
    return 0;
}

static int write_text_file(const char *path, const char *value)
{
    int fd;
    ssize_t n;
    size_t len = strlen(value);
    int saved_errno = 0;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    n = write(fd, value, len);
    if (n < 0)
        saved_errno = errno;
    else if ((size_t)n != len)
        saved_errno = EIO;
    close(fd);

    if (saved_errno) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static bool path_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int read_float_file(const char *path, float *out)
{
    char buf[64];
    char *end = NULL;
    float value;

    if (read_text_file(path, buf, sizeof(buf)) < 0)
        return -1;

    errno = 0;
    value = strtof(buf, &end);
    if (errno || end == buf)
        return -1;
    *out = value;
    return 0;
}

static int write_sysfs_path3(const char *dir, const char *leaf, const char *value)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    return write_text_file(path, value);
}

static int read_sysfs_path3(const char *dir, const char *leaf, char *buf, size_t size)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    return read_text_file(path, buf, size);
}

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
    int ret;

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

static int iio_init(iio_backend_t *iio, const app_config_t *cfg)
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

static void iio_close(iio_backend_t *iio)
{
    if (iio->buffer_enabled) {
        (void)write_sysfs_path3(iio->dev_dir, "buffer/enable", "0");
        iio->buffer_enabled = false;
    }
    if (iio->fd >= 0)
        close(iio->fd);
    iio->fd = -1;
}

static int bmi088_init(bmi088_t *imu, const app_config_t *cfg)
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

    if (i2c_read_reg(imu->accel_fd, BMI088_ACCEL_CHIP_ID_REG, &id) < 0 || id != BMI088_ACCEL_CHIP_ID) {
        errno = ENODEV;
        return -1;
    }
    if (i2c_read_reg(imu->gyro_fd, BMI088_GYRO_CHIP_ID_REG, &id) < 0 || id != BMI088_GYRO_CHIP_ID) {
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

static void bmi088_close(bmi088_t *imu)
{
    if (imu->accel_fd >= 0)
        close(imu->accel_fd);
    if (imu->gyro_fd >= 0)
        close(imu->gyro_fd);
    imu->accel_fd = -1;
    imu->gyro_fd = -1;
}

static int16_t le16_to_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int iio_read_sample(iio_backend_t *iio, imu_sample_t *sample)
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

static int bmi088_read_sample(bmi088_t *imu, imu_sample_t *sample)
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

static void simulate_sample(imu_sample_t *sample, unsigned long long index, float rate_hz)
{
    float t = (float)index / rate_hz;
    float yaw_rate_rad_s = 5.0f * DEG_TO_RAD;

    memset(sample, 0, sizeof(*sample));
    sample->accel_mps2[0] = 0.04f * sinf(t * 2.0f);
    sample->accel_mps2[1] = 0.04f * cosf(t * 1.7f);
    sample->accel_mps2[2] = GRAVITY_MPS2;
    sample->gyro_rad_s[2] = yaw_rate_rad_s;
    sample->accel_raw[0] = (int16_t)(sample->accel_mps2[0] / (GRAVITY_MPS2 / 5460.0f));
    sample->accel_raw[1] = (int16_t)(sample->accel_mps2[1] / (GRAVITY_MPS2 / 5460.0f));
    sample->accel_raw[2] = (int16_t)(sample->accel_mps2[2] / (GRAVITY_MPS2 / 5460.0f));
    sample->gyro_raw[2] = (int16_t)(sample->gyro_rad_s[2] / ((250.0f / 32768.0f) * DEG_TO_RAD));
}

static float vector_norm3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static float accel_confidence(const pose_config_t *cfg, float accel_norm)
{
    float confidence;

    if (accel_norm <= cfg->accel_trust_min_mps2 || accel_norm >= cfg->accel_trust_max_mps2)
        return 0.0f;
    if (accel_norm <= cfg->accel_norm_ref_mps2)
        confidence = (accel_norm - cfg->accel_trust_min_mps2) /
                     (cfg->accel_norm_ref_mps2 - cfg->accel_trust_min_mps2);
    else
        confidence = (cfg->accel_trust_max_mps2 - accel_norm) /
                     (cfg->accel_trust_max_mps2 - cfg->accel_norm_ref_mps2);
    return clamp_f32(confidence, 0.0f, 1.0f);
}

static void pose_init(pose_filter_t *pose, const pose_config_t *cfg)
{
    memset(pose, 0, sizeof(*pose));
    pose->cfg = cfg;
    pose->q[0] = 1.0f;
    pose->q[1] = 0.0f;
    pose->q[2] = 0.0f;
    pose->q[3] = 0.0f;
    pose->sample_period_s = 1.0f / (float)cfg->sample_rate_hz;
    if (!cfg->gyro_auto_offset_enable) {
        pose->gyro_auto_offset_done = true;
        pose->ready = true;
    }
}

static int pose_update_motion_state(pose_filter_t *pose)
{
    float gyro_norm;

    pose->accel_norm_mps2 = vector_norm3(pose->accel_mps2);
    pose->accel_confidence = accel_confidence(pose->cfg, pose->accel_norm_mps2);
    gyro_norm = vector_norm3(pose->gyro_rad_s);
    pose->static_detected =
        pose->accel_norm_mps2 >= pose->cfg->accel_static_min_mps2 &&
        pose->accel_norm_mps2 <= pose->cfg->accel_static_max_mps2 &&
        gyro_norm <= pose->cfg->gyro_static_limit_rad_s;
    return 0;
}

static int pose_update_gyro_offset(pose_filter_t *pose)
{
    const pose_config_t *cfg = pose->cfg;

    for (int i = 0; i < 3; i++)
        pose->gyro_rad_s[i] -= cfg->gyro_offset_rad_s[i];

    if (!cfg->gyro_auto_offset_enable) {
        pose_update_motion_state(pose);
        pose->gyro_auto_offset_done = true;
        return 0;
    }

    if (pose->gyro_auto_offset_done) {
        for (int i = 0; i < 3; i++)
            pose->gyro_rad_s[i] -= pose->gyro_auto_offset_rad_s[i];
        pose_update_motion_state(pose);
        return 0;
    }

    pose_update_motion_state(pose);
    if (!pose->static_detected)
        return POSE_UPDATE_CALIBRATING;

    if (pose->gyro_auto_offset_count < cfg->gyro_auto_offset_sample_count) {
        for (int i = 0; i < 3; i++)
            pose->gyro_auto_offset_rad_s[i] += pose->gyro_rad_s[i] /
                                               (float)cfg->gyro_auto_offset_sample_count;
        pose->gyro_auto_offset_count++;
        if (pose->gyro_auto_offset_count < cfg->gyro_auto_offset_sample_count)
            return POSE_UPDATE_CALIBRATING;
    }

    for (int i = 0; i < 3; i++) {
        if (fabsf(pose->gyro_auto_offset_rad_s[i]) > cfg->gyro_auto_offset_limit_rad_s)
            pose->gyro_auto_offset_rad_s[i] = 0.0f;
        pose->gyro_rad_s[i] -= pose->gyro_auto_offset_rad_s[i];
    }
    pose->gyro_auto_offset_done = true;
    pose_update_motion_state(pose);
    return POSE_UPDATE_OK;
}

static int pose_update(pose_filter_t *pose, const imu_sample_t *sample, float dt_s)
{
    float q0;
    float q1;
    float q2;
    float q3;
    float q0_temp;
    float q1_temp;
    float q2_temp;
    float q3_temp;
    float norm;
    float vx;
    float vy;
    float vz;
    float gx;
    float gy;
    float gz;
    float ax;
    float ay;
    float az;
    float ex = 0.0f;
    float ey = 0.0f;
    float ez = 0.0f;
    float half_t;
    float sin_temp;
    float cos_temp;

    memcpy(pose->accel_mps2, sample->accel_mps2, sizeof(pose->accel_mps2));
    memcpy(pose->gyro_rad_s, sample->gyro_rad_s, sizeof(pose->gyro_rad_s));

    int offset_status = pose_update_gyro_offset(pose);

    if (offset_status != POSE_UPDATE_OK)
        return offset_status;

    ax = pose->accel_mps2[0];
    ay = pose->accel_mps2[1];
    az = pose->accel_mps2[2];
    gx = pose->gyro_rad_s[0];
    gy = pose->gyro_rad_s[1];
    gz = pose->gyro_rad_s[2];
    q0 = pose->q[0];
    q1 = pose->q[1];
    q2 = pose->q[2];
    q3 = pose->q[3];

    norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    if (norm <= 0.0f)
        return -1;
    q0 /= norm;
    q1 /= norm;
    q2 /= norm;
    q3 /= norm;

    norm = sqrtf(ax * ax + ay * ay + az * az);
    if (norm > 0.0f) {
        ax /= norm;
        ay /= norm;
        az /= norm;

        vx = 2.0f * (q1 * q3 - q0 * q2);
        vy = 2.0f * (q0 * q1 + q2 * q3);
        vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

        ex = ay * vz - az * vy;
        ey = az * vx - ax * vz;
        ez = ax * vy - ay * vx;
    } else {
        pose->accel_confidence = 0.0f;
    }

    pose->effective_kp = pose->cfg->kp * pose->accel_confidence;
    pose->effective_ki = pose->cfg->ki * pose->accel_confidence;
    pose->ex_int += ex * pose->effective_ki;
    pose->ey_int += ey * pose->effective_ki;
    pose->ez_int += ez * pose->effective_ki;

    if (pose->accel_confidence == 0.0f) {
        pose->ex_int = 0.0f;
        pose->ey_int = 0.0f;
        pose->ez_int = 0.0f;
    }

    gx += pose->effective_kp * ex + pose->ex_int;
    gy += pose->effective_kp * ey + pose->ey_int;
    gz += pose->effective_kp * ez + pose->ez_int;

    q0_temp = q0;
    q1_temp = q1;
    q2_temp = q2;
    q3_temp = q3;
    if (dt_s <= 0.0f)
        dt_s = pose->sample_period_s;
    dt_s = clamp_f32(dt_s, pose->sample_period_s * 0.25f, pose->sample_period_s * 4.0f);
    half_t = dt_s / 2.0f;

    q0 += (-q1_temp * gx - q2_temp * gy - q3_temp * gz) * half_t;
    q1 += (q0_temp * gx + q2_temp * gz - q3_temp * gy) * half_t;
    q2 += (q0_temp * gy - q1_temp * gz + q3_temp * gx) * half_t;
    q3 += (q0_temp * gz + q1_temp * gy - q2_temp * gx) * half_t;

    norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    if (norm <= 0.0f)
        return -1;
    q0 /= norm;
    q1 /= norm;
    q2 /= norm;
    q3 /= norm;

    pose->q[0] = q0;
    pose->q[1] = q1;
    pose->q[2] = q2;
    pose->q[3] = q3;

    sin_temp = clamp_f32(2.0f * q1 * q3 - 2.0f * q0 * q2, -1.0f, 1.0f);
    cos_temp = sqrtf(fmaxf(0.0f, 1.0f - sin_temp * sin_temp));
    pose->roll_deg = atan2f(2.0f * q2 * q3 + 2.0f * q0 * q1,
                            q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3) * RAD_TO_DEG;
    pose->pitch_deg = -atan2f(sin_temp, cos_temp) * RAD_TO_DEG;
    pose->yaw_deg = atan2f(2.0f * (q1 * q2 + q0 * q3),
                           q0 * q0 + q1 * q1 - q2 * q2 - q3 * q3) * RAD_TO_DEG;
    pose->yaw_deg = half_cycle_f32(pose->yaw_deg, 360.0f);
    pose->ready = true;
    return 0;
}

static void stats_init(loop_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->min_period_ns = INT64_MAX;
}

static void stats_update(loop_stats_t *stats, int64_t period_ns, int64_t read_ns,
                         int64_t update_ns, bool late, bool read_ok, bool update_ok)
{
    stats->count++;
    if (period_ns < stats->min_period_ns)
        stats->min_period_ns = period_ns;
    if (period_ns > stats->max_period_ns)
        stats->max_period_ns = period_ns;
    stats->sum_period_ns += period_ns;
    stats->sum_read_ns += read_ns;
    stats->sum_update_ns += update_ns;
    if (read_ns > stats->max_read_ns)
        stats->max_read_ns = read_ns;
    if (update_ns > stats->max_update_ns)
        stats->max_update_ns = update_ns;
    if (late)
        stats->late_count++;
    if (!read_ok)
        stats->read_error_count++;
    if (!update_ok)
        stats->update_error_count++;
}

static void print_stats(const loop_stats_t *stats, const pose_filter_t *pose, int64_t window_ns)
{
    double elapsed_s = (double)window_ns / 1000000000.0;
    double hz = elapsed_s > 0.0 ? (double)stats->count / elapsed_s : 0.0;
    double avg_period_us = stats->count ? (double)(stats->sum_period_ns / stats->count) / 1000.0 : 0.0;
    double min_period_us = stats->min_period_ns == INT64_MAX ? 0.0 : (double)stats->min_period_ns / 1000.0;
    double max_period_us = (double)stats->max_period_ns / 1000.0;
    double avg_read_us = stats->count ? (double)(stats->sum_read_ns / stats->count) / 1000.0 : 0.0;
    double avg_update_us = stats->count ? (double)(stats->sum_update_ns / stats->count) / 1000.0 : 0.0;

    printf("rate=%.1fHz count=%llu period_us avg=%.1f min=%.1f max=%.1f late=%llu "
           "read_us avg=%.1f max=%.1f update_us avg=%.2f max=%.2f errors read=%llu update=%llu "
           "rpy_deg=%.2f %.2f %.2f acc_conf=%.2f ready=%u\n",
           hz,
           stats->count,
           avg_period_us,
           min_period_us,
           max_period_us,
           stats->late_count,
           avg_read_us,
           (double)stats->max_read_ns / 1000.0,
           avg_update_us,
           (double)stats->max_update_ns / 1000.0,
           stats->read_error_count,
           stats->update_error_count,
           pose->roll_deg,
           pose->pitch_deg,
           pose->yaw_deg,
           pose->accel_confidence,
           pose->ready ? 1u : 0u);
}

static void print_usage(const char *argv0)
{
    printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --bus <n>              I2C bus number. Default: %u\n"
        "  --accel-addr <addr>    BMI088 accel I2C address. Default: 0x%02X\n"
        "  --gyro-addr <addr>     BMI088 gyro I2C address. Default: 0x%02X\n"
        "  --rate <hz>            Update rate. Default: %u\n"
        "  --duration <sec>       Run duration, 0 means forever. Default: %u\n"
        "  --report-ms <ms>       Stats print interval. Default: %u\n"
        "  --wake-margin-us <us>  Busy-wait window before deadline. Default: %u\n"
        "  --cpu <n>              Pin the process to one CPU, -1 disables. Default: -1\n"
        "  --fifo-priority <n>    SCHED_FIFO priority. Default: %u\n"
        "  --kp <value>           Mahony proportional gain. Default: 0.1\n"
        "  --ki <value>           Mahony integral gain. Default: 0.0\n"
        "  --simulate             Use generated IMU samples instead of BMI088 input\n"
        "  --iio                  Use BMI088 IIO hrtimer trigger + buffer. Default on\n"
        "  --i2c                  Use direct BMI088 I2C register reads\n"
        "  --iio-device <name>    IIO device path/name. Default: first bmi088 device\n"
        "  --iio-trigger <name>   hrtimer trigger name. Default: %s\n"
        "  --iio-buffer <n>       IIO buffer length. Default: %u\n"
        "  --i2c-rdwr             Use combined I2C_RDWR register reads. Default on\n"
        "  --no-i2c-rdwr          Use write/read register access fallback\n"
        "  --i2c-force            Use I2C_SLAVE_FORCE when a kernel driver owns BMI088\n"
        "  --fixed-dt             Integrate fusion with nominal period instead of measured dt\n"
        "  --no-auto-offset       Skip startup gyro static calibration\n"
        "  --no-realtime          Do not request SCHED_FIFO/mlockall\n"
        "  --verbose              Print init details\n"
        "  -h, --help             Show this help\n",
        argv0, DEFAULT_I2C_BUS, DEFAULT_ACCEL_ADDR, DEFAULT_GYRO_ADDR,
        DEFAULT_RATE_HZ, DEFAULT_DURATION_S, DEFAULT_REPORT_MS,
        DEFAULT_WAKE_MARGIN_US, DEFAULT_FIFO_PRIORITY,
        DEFAULT_IIO_TRIGGER_NAME, DEFAULT_IIO_BUFFER_LENGTH);
}

static int parse_args(int argc, char **argv, app_config_t *cfg, pose_config_t *pose_cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    memset(pose_cfg, 0, sizeof(*pose_cfg));

    cfg->i2c_bus = DEFAULT_I2C_BUS;
    cfg->accel_addr = DEFAULT_ACCEL_ADDR;
    cfg->gyro_addr = DEFAULT_GYRO_ADDR;
    cfg->rate_hz = DEFAULT_RATE_HZ;
    cfg->report_ms = DEFAULT_REPORT_MS;
    cfg->duration_s = DEFAULT_DURATION_S;
    cfg->wake_margin_us = DEFAULT_WAKE_MARGIN_US;
    cfg->fifo_priority = DEFAULT_FIFO_PRIORITY;
    cfg->iio_buffer_length = DEFAULT_IIO_BUFFER_LENGTH;
    cfg->cpu_affinity = -1;
    cfg->iio = true;
    cfg->i2c_rdwr = true;
    cfg->realtime = true;

    pose_cfg->sample_rate_hz = cfg->rate_hz;
    pose_cfg->kp = 0.1f;
    pose_cfg->ki = 0.0f;
    pose_cfg->gyro_auto_offset_enable = true;
    pose_cfg->gyro_auto_offset_sample_count = 1000;
    pose_cfg->gyro_auto_offset_limit_rad_s = 0.05f;
    pose_cfg->accel_norm_ref_mps2 = GRAVITY_MPS2;
    pose_cfg->accel_trust_min_mps2 = 8.33565f;
    pose_cfg->accel_trust_max_mps2 = 11.27765f;
    pose_cfg->accel_static_min_mps2 = 9.61052f;
    pose_cfg->accel_static_max_mps2 = 10.00278f;
    pose_cfg->gyro_static_limit_rad_s = 0.02f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--bus") == 0) {
            if (++i >= argc || parse_u32(argv[i], 0, 64, &cfg->i2c_bus) < 0)
                return -1;
        } else if (strcmp(argv[i], "--accel-addr") == 0) {
            if (++i >= argc || parse_u32(argv[i], 0x03, 0x77, &cfg->accel_addr) < 0)
                return -1;
        } else if (strcmp(argv[i], "--gyro-addr") == 0) {
            if (++i >= argc || parse_u32(argv[i], 0x03, 0x77, &cfg->gyro_addr) < 0)
                return -1;
        } else if (strcmp(argv[i], "--rate") == 0) {
            if (++i >= argc || parse_u32(argv[i], 1, 5000, &cfg->rate_hz) < 0)
                return -1;
        } else if (strcmp(argv[i], "--duration") == 0) {
            if (++i >= argc || parse_u32(argv[i], 0, 86400, &cfg->duration_s) < 0)
                return -1;
        } else if (strcmp(argv[i], "--report-ms") == 0) {
            if (++i >= argc || parse_u32(argv[i], 100, 60000, &cfg->report_ms) < 0)
                return -1;
        } else if (strcmp(argv[i], "--wake-margin-us") == 0) {
            if (++i >= argc || parse_u32(argv[i], 0, 1000, &cfg->wake_margin_us) < 0)
                return -1;
        } else if (strcmp(argv[i], "--cpu") == 0) {
            if (++i >= argc || parse_i32(argv[i], -1, 1024, &cfg->cpu_affinity) < 0)
                return -1;
        } else if (strcmp(argv[i], "--fifo-priority") == 0) {
            if (++i >= argc || parse_u32(argv[i], 1, 99, &cfg->fifo_priority) < 0)
                return -1;
        } else if (strcmp(argv[i], "--kp") == 0) {
            if (++i >= argc || parse_float_arg(argv[i], &pose_cfg->kp) < 0)
                return -1;
        } else if (strcmp(argv[i], "--ki") == 0) {
            if (++i >= argc || parse_float_arg(argv[i], &pose_cfg->ki) < 0)
                return -1;
        } else if (strcmp(argv[i], "--simulate") == 0) {
            cfg->simulate = true;
        } else if (strcmp(argv[i], "--iio") == 0) {
            cfg->iio = true;
        } else if (strcmp(argv[i], "--i2c") == 0) {
            cfg->iio = false;
        } else if (strcmp(argv[i], "--iio-device") == 0) {
            if (++i >= argc || strlen(argv[i]) >= sizeof(cfg->iio_device))
                return -1;
            snprintf(cfg->iio_device, sizeof(cfg->iio_device), "%s", argv[i]);
        } else if (strcmp(argv[i], "--iio-trigger") == 0) {
            if (++i >= argc || strlen(argv[i]) >= sizeof(cfg->iio_trigger))
                return -1;
            snprintf(cfg->iio_trigger, sizeof(cfg->iio_trigger), "%s", argv[i]);
        } else if (strcmp(argv[i], "--iio-buffer") == 0) {
            if (++i >= argc || parse_u32(argv[i], 2, 4096, &cfg->iio_buffer_length) < 0)
                return -1;
        } else if (strcmp(argv[i], "--i2c-rdwr") == 0) {
            cfg->i2c_rdwr = true;
        } else if (strcmp(argv[i], "--no-i2c-rdwr") == 0) {
            cfg->i2c_rdwr = false;
        } else if (strcmp(argv[i], "--i2c-force") == 0) {
            cfg->i2c_force = true;
            cfg->iio = false;
        } else if (strcmp(argv[i], "--fixed-dt") == 0) {
            cfg->fixed_dt = true;
        } else if (strcmp(argv[i], "--no-auto-offset") == 0) {
            pose_cfg->gyro_auto_offset_enable = false;
        } else if (strcmp(argv[i], "--no-realtime") == 0) {
            cfg->realtime = false;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else {
            return -1;
        }
    }

    pose_cfg->sample_rate_hz = cfg->rate_hz;
    return 0;
}

static void request_realtime(const app_config_t *cfg)
{
    struct sched_param sp;

    if (cfg->cpu_affinity >= 0) {
        cpu_set_t set;

        CPU_ZERO(&set);
        CPU_SET((unsigned int)cfg->cpu_affinity, &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0 && cfg->verbose)
            fprintf(stderr, "sched_setaffinity cpu=%d warning: %s\n",
                    cfg->cpu_affinity, strerror(errno));
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0 && cfg->verbose)
        fprintf(stderr, "mlockall warning: %s\n", strerror(errno));

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = (int)cfg->fifo_priority;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0 && cfg->verbose)
        fprintf(stderr, "SCHED_FIFO warning: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
    app_config_t cfg;
    pose_config_t pose_cfg;
    bmi088_t imu;
    iio_backend_t iio;
    pose_filter_t pose;
    loop_stats_t stats;
    int64_t period_ns;
    int64_t next_ns;
    int64_t last_loop_ns;
    int64_t start_ns;
    int64_t last_report_ns;
    unsigned long long sample_index = 0;
    int rc = 0;
    bool use_iio;

    if (parse_args(argc, argv, &cfg, &pose_cfg) < 0) {
        print_usage(argv[0]);
        return 2;
    }

    use_iio = cfg.iio && !cfg.simulate;
    memset(&imu, 0, sizeof(imu));
    imu.accel_fd = -1;
    imu.gyro_fd = -1;
    memset(&iio, 0, sizeof(iio));
    iio.fd = -1;

    if (cfg.realtime)
        request_realtime(&cfg);

    if (use_iio) {
        if (iio_init(&iio, &cfg) < 0) {
            fprintf(stderr, "imu_pose: failed to init BMI088 IIO hrtimer buffer: %s\n",
                    strerror(errno));
            fprintf(stderr, "imu_pose: check CONFIG_IIO_CONFIGFS, CONFIG_IIO_SW_TRIGGER, "
                    "CONFIG_IIO_HRTIMER_TRIGGER and /sys/kernel/config/iio/triggers/hrtimer.\n");
            iio_close(&iio);
            return 1;
        }
    } else if (!cfg.simulate) {
        if (bmi088_init(&imu, &cfg) < 0) {
            fprintf(stderr, "imu_pose: failed to init BMI088 on /dev/i2c-%u accel=0x%02X gyro=0x%02X: %s\n",
                    cfg.i2c_bus, cfg.accel_addr, cfg.gyro_addr, strerror(errno));
            if (errno == EBUSY && !cfg.i2c_force)
                fprintf(stderr, "imu_pose: BMI088 address is owned by kernel driver; use --i2c-force only for lab validation.\n");
            return 1;
        }
    }

    pose_init(&pose, &pose_cfg);
    stats_init(&stats);
    period_ns = 1000000000LL / (int64_t)cfg.rate_hz;
    start_ns = now_ns();
    next_ns = start_ns + period_ns;
    last_loop_ns = start_ns;
    last_report_ns = start_ns;

    printf("imu_pose: source=%s i2c_rdwr=%u rate=%uHz report=%ums duration=%us "
           "wake_margin_us=%u cpu=%d fifo=%u dt=%s fusion=Mahony6 kp=%.3f ki=%.3f\n",
           cfg.simulate ? "simulate" :
           (use_iio ? "bmi088-iio-hrtimer-buffer" :
            (cfg.i2c_force ? "bmi088-i2c-force" : "bmi088-i2c")),
           cfg.i2c_rdwr ? 1u : 0u,
           cfg.rate_hz,
           cfg.report_ms,
           cfg.duration_s,
           cfg.wake_margin_us,
           cfg.cpu_affinity,
           cfg.fifo_priority,
           cfg.fixed_dt ? "fixed" : "measured",
           pose_cfg.kp,
           pose_cfg.ki);
    fflush(stdout);

    while (cfg.duration_s == 0 || (unsigned int)((now_ns() - start_ns) / 1000000000LL) < cfg.duration_s) {
        imu_sample_t sample;
        int64_t loop_ns;
        int64_t read_start_ns;
        int64_t read_end_ns;
        int64_t update_end_ns;
        bool read_ok = true;
        bool update_ok = true;
        bool late;

        if (use_iio) {
            read_start_ns = now_ns();
            if (iio_read_sample(&iio, &sample) < 0)
                read_ok = false;
            read_end_ns = now_ns();
            loop_ns = read_end_ns;
            late = (loop_ns - last_loop_ns) > period_ns + period_ns / 2;
        } else {
            wait_until_ns(next_ns, (int64_t)cfg.wake_margin_us * 1000LL);
            loop_ns = now_ns();
            late = loop_ns > next_ns + period_ns / 2;

            read_start_ns = now_ns();
            if (cfg.simulate)
                simulate_sample(&sample, sample_index, (float)cfg.rate_hz);
            else if (bmi088_read_sample(&imu, &sample) < 0)
                read_ok = false;
            read_end_ns = now_ns();
        }

        if (read_ok) {
            float dt_s = cfg.fixed_dt ? pose.sample_period_s :
                (float)(loop_ns - last_loop_ns) / 1000000000.0f;
            int update_status = pose_update(&pose, &sample, dt_s);

            if (update_status < 0)
                update_ok = false;
        }
        update_end_ns = now_ns();

        stats_update(&stats,
                     loop_ns - last_loop_ns,
                     read_end_ns - read_start_ns,
                     update_end_ns - read_end_ns,
                     late,
                     read_ok,
                     update_ok);

        last_loop_ns = loop_ns;
        sample_index++;
        next_ns += period_ns;
        if (next_ns < loop_ns)
            next_ns = loop_ns + period_ns;

        if ((uint64_t)(loop_ns - last_report_ns) >= (uint64_t)cfg.report_ms * 1000000ULL) {
            print_stats(&stats, &pose, loop_ns - last_report_ns);
            stats_init(&stats);
            last_report_ns = loop_ns;
            fflush(stdout);
        }
    }

    if (stats.count > 0)
        print_stats(&stats, &pose, now_ns() - last_report_ns);

    if (use_iio)
        iio_close(&iio);
    else
        bmi088_close(&imu);
    return rc;
}
