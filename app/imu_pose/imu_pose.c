#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "imu_pose.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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

static void set_default_config(app_config_t *cfg, pose_config_t *pose_cfg)
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
}

static int parse_args(int argc, char **argv, app_config_t *cfg, pose_config_t *pose_cfg)
{
    set_default_config(cfg, pose_cfg);

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

static int init_input(const app_config_t *cfg, bool use_iio, bmi088_t *imu, iio_backend_t *iio)
{
    memset(imu, 0, sizeof(*imu));
    imu->accel_fd = -1;
    imu->gyro_fd = -1;
    memset(iio, 0, sizeof(*iio));
    iio->fd = -1;

    if (use_iio) {
        if (iio_init(iio, cfg) < 0) {
            fprintf(stderr, "imu_pose: failed to init BMI088 IIO hrtimer buffer: %s\n",
                    strerror(errno));
            fprintf(stderr, "imu_pose: check CONFIG_IIO_CONFIGFS, CONFIG_IIO_SW_TRIGGER, "
                    "CONFIG_IIO_HRTIMER_TRIGGER and /sys/kernel/config/iio/triggers/hrtimer.\n");
            iio_close(iio);
            return -1;
        }
    } else if (!cfg->simulate) {
        if (bmi088_init(imu, cfg) < 0) {
            fprintf(stderr, "imu_pose: failed to init BMI088 on /dev/i2c-%u accel=0x%02X gyro=0x%02X: %s\n",
                    cfg->i2c_bus, cfg->accel_addr, cfg->gyro_addr, strerror(errno));
            if (errno == EBUSY && !cfg->i2c_force)
                fprintf(stderr, "imu_pose: BMI088 address is owned by kernel driver; use --i2c-force only for lab validation.\n");
            return -1;
        }
    }
    return 0;
}

static int read_input_sample(const app_config_t *cfg, bool use_iio, iio_backend_t *iio,
                             bmi088_t *imu, unsigned long long sample_index,
                             imu_sample_t *sample)
{
    if (use_iio)
        return iio_read_sample(iio, sample);
    if (cfg->simulate) {
        simulate_sample(sample, sample_index, (float)cfg->rate_hz);
        return 0;
    }
    return bmi088_read_sample(imu, sample);
}

static void print_start_banner(const app_config_t *cfg, const pose_config_t *pose_cfg, bool use_iio)
{
    printf("imu_pose: source=%s i2c_rdwr=%u rate=%uHz report=%ums duration=%us "
           "wake_margin_us=%u cpu=%d fifo=%u dt=%s fusion=Mahony6 kp=%.3f ki=%.3f\n",
           cfg->simulate ? "simulate" :
           (use_iio ? "bmi088-iio-hrtimer-buffer" :
            (cfg->i2c_force ? "bmi088-i2c-force" : "bmi088-i2c")),
           cfg->i2c_rdwr ? 1u : 0u,
           cfg->rate_hz,
           cfg->report_ms,
           cfg->duration_s,
           cfg->wake_margin_us,
           cfg->cpu_affinity,
           cfg->fifo_priority,
           cfg->fixed_dt ? "fixed" : "measured",
           pose_cfg->kp,
           pose_cfg->ki);
    fflush(stdout);
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
    bool use_iio;

    if (parse_args(argc, argv, &cfg, &pose_cfg) < 0) {
        print_usage(argv[0]);
        return 2;
    }

    use_iio = cfg.iio && !cfg.simulate;

    if (cfg.realtime)
        request_realtime(&cfg);

    if (init_input(&cfg, use_iio, &imu, &iio) < 0)
        return 1;

    pose_init(&pose, &pose_cfg);
    stats_init(&stats);
    period_ns = 1000000000LL / (int64_t)cfg.rate_hz;
    start_ns = now_ns();
    next_ns = start_ns + period_ns;
    last_loop_ns = start_ns;
    last_report_ns = start_ns;

    print_start_banner(&cfg, &pose_cfg, use_iio);

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
            if (read_input_sample(&cfg, use_iio, &iio, &imu, sample_index, &sample) < 0)
                read_ok = false;
            read_end_ns = now_ns();
            loop_ns = read_end_ns;
            late = (loop_ns - last_loop_ns) > period_ns + period_ns / 2;
        } else {
            wait_until_ns(next_ns, (int64_t)cfg.wake_margin_us * 1000LL);
            loop_ns = now_ns();
            late = loop_ns > next_ns + period_ns / 2;

            read_start_ns = now_ns();
            if (read_input_sample(&cfg, use_iio, &iio, &imu, sample_index, &sample) < 0)
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
    return 0;
}
