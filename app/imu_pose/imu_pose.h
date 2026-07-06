#ifndef IMU_POSE_H
#define IMU_POSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
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

int64_t now_ns(void);
void wait_until_ns(int64_t deadline_ns, int64_t busy_margin_ns);
int parse_u32(const char *text, unsigned int min, unsigned int max, unsigned int *out);
int parse_i32(const char *text, int min, int max, int *out);
int parse_float_arg(const char *text, float *out);
float clamp_f32(float value, float min, float max);
float half_cycle_f32(float value, float cycle);
void sleep_ms(unsigned int ms);
int read_text_file(const char *path, char *buf, size_t size);
int write_text_file(const char *path, const char *value);
bool path_exists(const char *path);
int read_float_file(const char *path, float *out);
int write_sysfs_path3(const char *dir, const char *leaf, const char *value);
int read_sysfs_path3(const char *dir, const char *leaf, char *buf, size_t size);
int16_t le16_to_i16(const uint8_t *p);

int bmi088_init(bmi088_t *imu, const app_config_t *cfg);
void bmi088_close(bmi088_t *imu);
int bmi088_read_sample(bmi088_t *imu, imu_sample_t *sample);

int iio_init(iio_backend_t *iio, const app_config_t *cfg);
void iio_close(iio_backend_t *iio);
int iio_read_sample(iio_backend_t *iio, imu_sample_t *sample);

void simulate_sample(imu_sample_t *sample, unsigned long long index, float rate_hz);

void pose_init(pose_filter_t *pose, const pose_config_t *cfg);
int pose_update(pose_filter_t *pose, const imu_sample_t *sample, float dt_s);

void stats_init(loop_stats_t *stats);
void stats_update(loop_stats_t *stats, int64_t period_ns, int64_t read_ns,
                  int64_t update_ns, bool late, bool read_ok, bool update_ok);
void print_stats(const loop_stats_t *stats, const pose_filter_t *pose, int64_t window_ns);

#endif
