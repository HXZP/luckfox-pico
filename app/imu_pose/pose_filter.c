#include "imu_pose.h"

#include <math.h>
#include <string.h>

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

void pose_init(pose_filter_t *pose, const pose_config_t *cfg)
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

int pose_update(pose_filter_t *pose, const imu_sample_t *sample, float dt_s)
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
