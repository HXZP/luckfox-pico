#define _POSIX_C_SOURCE 200809L

#include "led_tool.h"

#include "tool_common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LED_TOOL_MIN_PERIOD_NS 1000U
#define LED_TOOL_MAX_BRIGHTNESS 100U
#define LED_TOOL_ACTIVE_LOW true

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int led_tool_sleep_ms(unsigned int ms)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = ms / 1000U;
    req.tv_nsec = (long)(ms % 1000U) * 1000000L;

    while (nanosleep(&req, &rem) < 0) {
        if (errno != EINTR)
            return -1;
        req = rem;
    }

    return 0;
}

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    ssize_t len;
    ssize_t written;

    if (fd < 0)
        return -1;

    len = (ssize_t)strlen(value);
    written = write(fd, value, (size_t)len);
    if (written < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    close(fd);
    if (written != len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int write_uint_file(const char *path, unsigned int value)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%u", value);
    return write_file(path, buf);
}

static int read_file_trim(const char *path, char *buf, size_t size)
{
    int fd;
    ssize_t n;
    size_t len;

    if (size == 0U) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1U);
    close(fd);
    if (n < 0)
        return -1;

    buf[n] = '\0';
    len = strlen(buf);
    while (len > 0U && isspace((unsigned char)buf[len - 1U])) {
        buf[len - 1U] = '\0';
        len--;
    }

    return 0;
}

static unsigned int clamp_u32(unsigned int value, unsigned int min, unsigned int max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

static int join_path(char *out, size_t out_size, const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool need_slash = left_len > 0U && left[left_len - 1U] != '/';
    size_t total = left_len + (need_slash ? 1U : 0U) + right_len;

    if (out_size == 0U || total >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(out, left, left_len);
    if (need_slash) {
        out[left_len] = '/';
        memcpy(out + left_len + 1U, right, right_len);
    } else {
        memcpy(out + left_len, right, right_len);
    }
    out[total] = '\0';
    return 0;
}

static int find_pwmchip_by_name(const char *name, char *out, size_t out_size)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/sys/class/pwm");
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char name_path[LED_TOOL_PATH_MAX];
        char read_name[128];
        char link_path[LED_TOOL_PATH_MAX];
        char link_target[LED_TOOL_PATH_MAX];
        ssize_t link_len;

        if (strncmp(entry->d_name, "pwmchip", 7) != 0)
            continue;

        snprintf(link_path, sizeof(link_path), "/sys/class/pwm/%s", entry->d_name);
        link_len = readlink(link_path, link_target, sizeof(link_target) - 1U);
        if (link_len > 0) {
            link_target[link_len] = '\0';
            if (strstr(link_target, name) != NULL) {
                snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
                closedir(dir);
                return 0;
            }
        }

        snprintf(link_path, sizeof(link_path), "/sys/class/pwm/%s/device", entry->d_name);
        link_len = readlink(link_path, link_target, sizeof(link_target) - 1U);
        if (link_len > 0) {
            link_target[link_len] = '\0';
            if (strstr(link_target, name) != NULL) {
                snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
                closedir(dir);
                return 0;
            }
        }

        snprintf(name_path, sizeof(name_path), "/sys/class/pwm/%s/device/name", entry->d_name);
        if (read_file_trim(name_path, read_name, sizeof(read_name)) == 0 &&
            strcmp(read_name, name) == 0) {
            snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
            closedir(dir);
            return 0;
        }

        snprintf(name_path, sizeof(name_path), "/sys/class/pwm/%s/device/uevent", entry->d_name);
        if (read_file_trim(name_path, read_name, sizeof(read_name)) == 0 &&
            strstr(read_name, name) != NULL) {
            snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    errno = ENOENT;
    return -1;
}

static int set_pwm_path_from_chip_arg(const char *arg, char *out, size_t out_size)
{
    uint32_t chip_num;

    if (strncmp(arg, "/sys/class/pwm/", 15) == 0) {
        snprintf(out, out_size, "%s", arg);
        return 0;
    }

    if (strncmp(arg, "pwmchip", 7) == 0) {
        snprintf(out, out_size, "/sys/class/pwm/%s", arg);
        return 0;
    }

    if (tool_parse_u32_arg(arg, 0U, UINT32_MAX, &chip_num) == 0) {
        snprintf(out, out_size, "/sys/class/pwm/pwmchip%u", chip_num);
        return 0;
    }

    if (find_pwmchip_by_name(arg, out, out_size) == 0)
        return 0;

    return -1;
}

static uint32_t period_from_frequency(unsigned int frequency_hz)
{
    uint64_t period;

    if (frequency_hz == 0U)
        frequency_hz = LED_TOOL_DEFAULT_PWM_FREQUENCY_HZ;

    period = 1000000000ULL / frequency_hz;
    if (period < LED_TOOL_MIN_PERIOD_NS)
        period = LED_TOOL_MIN_PERIOD_NS;
    if (period > UINT32_MAX)
        period = UINT32_MAX;

    return (uint32_t)period;
}

static uint32_t duty_from_percent(uint32_t period_ns, unsigned int percent)
{
    unsigned int duty_percent;

    percent = clamp_u32(percent, 0U, LED_TOOL_MAX_BRIGHTNESS);
    duty_percent = LED_TOOL_ACTIVE_LOW ? (LED_TOOL_MAX_BRIGHTNESS - percent) : percent;
    return (uint32_t)(((uint64_t)period_ns * duty_percent) / 100U);
}

static int pwm_export_if_needed(led_tool_headlight_t *light)
{
    char export_path[LED_TOOL_PATH_MAX];
    char pwm_name[64];

    snprintf(pwm_name, sizeof(pwm_name), "pwm%d", light->cfg.channel);
    if (join_path(light->pwm_path, sizeof(light->pwm_path), light->cfg.chip_path, pwm_name) < 0)
        return -1;

    if (path_exists(light->pwm_path))
        return 0;

    if (join_path(export_path, sizeof(export_path), light->cfg.chip_path, "export") < 0)
        return -1;
    if (write_uint_file(export_path, (unsigned int)light->cfg.channel) < 0)
        return -1;

    for (int retry = 0; retry < 50; retry++) {
        if (path_exists(light->pwm_path)) {
            light->exported_by_us = true;
            return 0;
        }
        if (led_tool_sleep_ms(10U) < 0)
            return -1;
    }

    errno = ETIMEDOUT;
    return -1;
}

static int pwm_enable(led_tool_headlight_t *light, bool enable)
{
    char path[LED_TOOL_PATH_MAX];

    if (join_path(path, sizeof(path), light->pwm_path, "enable") < 0)
        return -1;
    return write_uint_file(path, enable ? 1U : 0U);
}

static int pwm_set_period(led_tool_headlight_t *light, uint32_t period_ns)
{
    char path[LED_TOOL_PATH_MAX];

    if (join_path(path, sizeof(path), light->pwm_path, "period") < 0)
        return -1;
    return write_uint_file(path, period_ns);
}

static int pwm_set_duty(led_tool_headlight_t *light, uint32_t duty_ns)
{
    char path[LED_TOOL_PATH_MAX];

    if (join_path(path, sizeof(path), light->pwm_path, "duty_cycle") < 0)
        return -1;
    return write_uint_file(path, duty_ns);
}

void led_tool_headlight_defaults(led_tool_headlight_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->channel = LED_TOOL_DEFAULT_PWM_CHANNEL;
    cfg->frequency_hz = LED_TOOL_DEFAULT_PWM_FREQUENCY_HZ;
    cfg->brightness = LED_TOOL_DEFAULT_HEADLIGHT_BRIGHTNESS;

    if (find_pwmchip_by_name(LED_TOOL_DEFAULT_PWM_NAME,
                             cfg->chip_path, sizeof(cfg->chip_path)) < 0) {
        snprintf(cfg->chip_path, sizeof(cfg->chip_path), "/sys/class/pwm/pwmchip0");
    }
}

int led_tool_headlight_set_chip(led_tool_headlight_config_t *cfg, const char *chip)
{
    return set_pwm_path_from_chip_arg(chip, cfg->chip_path, sizeof(cfg->chip_path));
}

int led_tool_headlight_init(led_tool_headlight_t *light,
                            const led_tool_headlight_config_t *cfg)
{
    memset(light, 0, sizeof(*light));
    light->cfg = *cfg;
    light->period_ns = period_from_frequency(light->cfg.frequency_hz);

    if (pwm_export_if_needed(light) < 0)
        return -1;

    if (pwm_enable(light, false) < 0 && errno != EINVAL)
        return -1;
    if (pwm_set_period(light, light->period_ns) < 0)
        return -1;
    if (led_tool_headlight_set_brightness(light, 0U) < 0)
        return -1;
    if (pwm_enable(light, true) < 0)
        return -1;

    light->configured = true;
    return 0;
}

int led_tool_headlight_cleanup(led_tool_headlight_t *light)
{
    if (light == NULL || !light->configured)
        return 0;

    if (led_tool_headlight_set_brightness(light, 0U) < 0)
        return -1;
    if (pwm_enable(light, true) < 0)
        return -1;

    return 0;
}

int led_tool_headlight_set_brightness(led_tool_headlight_t *light, unsigned int percent)
{
    uint32_t duty = duty_from_percent(light->period_ns, percent);
    return pwm_set_duty(light, duty);
}

int led_tool_headlight_fade(led_tool_headlight_t *light,
                            unsigned int start_percent,
                            unsigned int end_percent,
                            unsigned int duration_ms,
                            unsigned int step_ms)
{
    unsigned int steps;
    unsigned int previous_target_ms = 0U;

    if (step_ms == 0U)
        step_ms = 1U;

    if (duration_ms == 0U)
        return led_tool_headlight_set_brightness(light, end_percent);

    steps = duration_ms / step_ms;
    if (steps == 0U)
        steps = 1U;

    for (unsigned int step = 0U; step <= steps; step++) {
        unsigned int target_ms;
        int delta = (int)end_percent - (int)start_percent;
        unsigned int brightness = (unsigned int)((int)start_percent +
                                  (delta * (int)step) / (int)steps);

        if (led_tool_headlight_set_brightness(light, brightness) < 0)
            return -1;

        if (step == steps)
            break;

        target_ms = (unsigned int)(((uint64_t)duration_ms * (step + 1U)) / steps);
        if (target_ms > previous_target_ms &&
            led_tool_sleep_ms(target_ms - previous_target_ms) < 0)
            return -1;
        previous_target_ms = target_ms;
    }

    return 0;
}

int led_tool_headlight_blink(led_tool_headlight_t *light,
                             unsigned int brightness,
                             uint16_t repeat,
                             uint16_t on_ms,
                             uint16_t off_ms)
{
    for (uint16_t index = 0U; index < repeat; index++) {
        if (led_tool_headlight_set_brightness(light, brightness) < 0)
            return -1;
        if (led_tool_sleep_ms(on_ms) < 0)
            return -1;
        if (led_tool_headlight_set_brightness(light, 0U) < 0)
            return -1;
        if (led_tool_sleep_ms(off_ms) < 0)
            return -1;
    }

    return 0;
}

static int validate_targets(const led_tool_rgb_target_t *targets, uint8_t target_count)
{
    uint8_t seen = 0U;

    if (targets == NULL || target_count == 0U || target_count > LED_TOOL_MAX_TARGETS) {
        errno = EINVAL;
        return -1;
    }

    for (uint8_t index = 0U; index < target_count; index++) {
        uint8_t address = targets[index].address;

        if (address >= LED_TOOL_MAX_TARGETS || (seen & (uint8_t)(1U << address)) != 0U) {
            errno = EINVAL;
            return -1;
        }
        seen |= (uint8_t)(1U << address);
    }

    return 0;
}

static void write_targets(uint8_t *out, const led_tool_rgb_target_t *targets, uint8_t target_count)
{
    for (uint8_t index = 0U; index < target_count; index++) {
        out[index * 4U + 0U] = targets[index].address;
        out[index * 4U + 1U] = targets[index].red;
        out[index * 4U + 2U] = targets[index].green;
        out[index * 4U + 3U] = targets[index].blue;
    }
}

static size_t write_enqueue_header(uint16_t request_id,
                                   uint8_t queue_mode,
                                   uint8_t action_count,
                                   uint8_t *out,
                                   size_t out_cap)
{
    if (queue_mode > LED_TOOL_QUEUE_INTERRUPT_REPLACE ||
        action_count == 0U || out == NULL || out_cap < 5U) {
        errno = EINVAL;
        return 0U;
    }

    out[0] = LED_TOOL_ACTION_VERSION;
    tool_put_le16(&out[1], request_id);
    out[3] = queue_mode;
    out[4] = action_count;
    return 5U;
}

static size_t append_hold_record(uint16_t action_id,
                                 uint32_t duration_ms,
                                 const led_tool_rgb_target_t *targets,
                                 uint8_t target_count,
                                 uint8_t *out,
                                 size_t out_cap)
{
    uint8_t record_len;

    if (validate_targets(targets, target_count) < 0)
        return 0U;

    record_len = (uint8_t)(8U + target_count * 4U);
    if (out == NULL || out_cap < (size_t)record_len + 1U) {
        errno = EMSGSIZE;
        return 0U;
    }

    out[0] = record_len;
    tool_put_le16(&out[1], action_id);
    out[3] = LED_TOOL_ACTION_HOLD;
    tool_put_le32(&out[4], duration_ms);
    out[8] = target_count;
    write_targets(&out[9], targets, target_count);

    return (size_t)record_len + 1U;
}

static size_t append_blink_record(uint16_t action_id,
                                  uint16_t repeat_count,
                                  uint16_t on_time_ms,
                                  uint16_t off_time_ms,
                                  const led_tool_rgb_target_t *targets,
                                  uint8_t target_count,
                                  uint8_t *out,
                                  size_t out_cap)
{
    uint8_t record_len;

    if (repeat_count == 0U || on_time_ms < 10U || off_time_ms < 10U) {
        errno = EINVAL;
        return 0U;
    }
    if (validate_targets(targets, target_count) < 0)
        return 0U;

    record_len = (uint8_t)(10U + target_count * 4U);
    if (out == NULL || out_cap < (size_t)record_len + 1U) {
        errno = EMSGSIZE;
        return 0U;
    }

    out[0] = record_len;
    tool_put_le16(&out[1], action_id);
    out[3] = LED_TOOL_ACTION_BLINK;
    tool_put_le16(&out[4], repeat_count);
    tool_put_le16(&out[6], on_time_ms);
    tool_put_le16(&out[8], off_time_ms);
    out[10] = target_count;
    write_targets(&out[11], targets, target_count);

    return (size_t)record_len + 1U;
}

size_t led_tool_build_hold_enqueue(uint16_t request_id,
                                   uint16_t action_id,
                                   uint8_t queue_mode,
                                   uint32_t duration_ms,
                                   const led_tool_rgb_target_t *targets,
                                   uint8_t target_count,
                                   uint8_t *out,
                                   size_t out_cap)
{
    size_t offset = write_enqueue_header(request_id, queue_mode, 1U, out, out_cap);
    size_t record_len;

    if (offset == 0U)
        return 0U;

    record_len = append_hold_record(action_id, duration_ms, targets, target_count,
                                    &out[offset], out_cap - offset);
    if (record_len == 0U)
        return 0U;

    return offset + record_len;
}

size_t led_tool_build_blink_enqueue(uint16_t request_id,
                                    uint16_t action_id,
                                    uint8_t queue_mode,
                                    uint16_t repeat_count,
                                    uint16_t on_time_ms,
                                    uint16_t off_time_ms,
                                    const led_tool_rgb_target_t *targets,
                                    uint8_t target_count,
                                    uint8_t *out,
                                    size_t out_cap)
{
    size_t offset = write_enqueue_header(request_id, queue_mode, 1U, out, out_cap);
    size_t record_len;

    if (offset == 0U)
        return 0U;

    record_len = append_blink_record(action_id, repeat_count, on_time_ms, off_time_ms,
                                     targets, target_count, &out[offset], out_cap - offset);
    if (record_len == 0U)
        return 0U;

    return offset + record_len;
}

int led_tool_parse_enqueue_response(const uint8_t *payload,
                                    uint16_t payload_len,
                                    led_tool_enqueue_response_t *rsp)
{
    if (payload == NULL || rsp == NULL || payload_len < 2U) {
        errno = EMSGSIZE;
        return -1;
    }

    memset(rsp, 0, sizeof(*rsp));
    rsp->status = tool_get_i16_le(payload);

    if (payload_len == 2U)
        return 0;

    if (payload_len < 6U) {
        errno = EMSGSIZE;
        return -1;
    }

    rsp->has_detail = true;
    rsp->request_id = tool_get_le16(&payload[2]);
    rsp->accepted_count = payload[4];
    rsp->queue_used = payload[5];
    return 0;
}

int led_tool_parse_action_event(const uint8_t *payload,
                                uint16_t payload_len,
                                led_tool_action_event_t *event)
{
    if (payload == NULL || event == NULL || payload_len != 9U) {
        errno = EMSGSIZE;
        return -1;
    }

    event->version = payload[0];
    event->request_id = tool_get_le16(&payload[1]);
    event->action_id = tool_get_le16(&payload[3]);
    event->event = payload[5];
    event->result = tool_get_i16_le(&payload[6]);
    event->queue_used = payload[8];
    return 0;
}

const char *led_tool_queue_mode_name(uint8_t mode)
{
    switch (mode) {
    case LED_TOOL_QUEUE_APPEND:
        return "APPEND";
    case LED_TOOL_QUEUE_REPLACE_PENDING:
        return "REPLACE_PENDING";
    case LED_TOOL_QUEUE_INTERRUPT_REPLACE:
        return "INTERRUPT_REPLACE";
    default:
        return "UNKNOWN";
    }
}

const char *led_tool_action_event_name(uint8_t event)
{
    switch (event) {
    case 0U:
        return "COMPLETED";
    case 1U:
        return "CANCELLED";
    case 2U:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}
