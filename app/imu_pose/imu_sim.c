#include "imu_pose.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

void simulate_sample(imu_sample_t *sample, unsigned long long index, float rate_hz)
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
