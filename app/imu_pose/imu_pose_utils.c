#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "imu_pose.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int64_t now_ns(void)
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

void wait_until_ns(int64_t deadline_ns, int64_t busy_margin_ns)
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

int parse_u32(const char *text, unsigned int min, unsigned int max, unsigned int *out)
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

int parse_i32(const char *text, int min, int max, int *out)
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

int parse_float_arg(const char *text, float *out)
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

float clamp_f32(float value, float min, float max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

float half_cycle_f32(float value, float cycle)
{
    float half = cycle / 2.0f;

    while (value >= half)
        value -= cycle;
    while (value < -half)
        value += cycle;
    return value;
}

void sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

int read_text_file(const char *path, char *buf, size_t size)
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

int write_text_file(const char *path, const char *value)
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

bool path_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

int read_float_file(const char *path, float *out)
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

int write_sysfs_path3(const char *dir, const char *leaf, const char *value)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    return write_text_file(path, value);
}

int read_sysfs_path3(const char *dir, const char *leaf, char *buf, size_t size)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    return read_text_file(path, buf, size);
}

int16_t le16_to_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
