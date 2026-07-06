#include "imu_pose.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void stats_init(loop_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->min_period_ns = INT64_MAX;
}

void stats_update(loop_stats_t *stats, int64_t period_ns, int64_t read_ns,
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

void print_stats(const loop_stats_t *stats, const pose_filter_t *pose, int64_t window_ns)
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
