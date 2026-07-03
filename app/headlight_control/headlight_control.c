#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_PWM_NAME "ff490020"
#define DEFAULT_PWM_CHANNEL 0
#define DEFAULT_FREQUENCY_HZ 1000
#define DEFAULT_BRIGHTNESS 100
#define MAX_BRIGHTNESS 100
#define MIN_PERIOD_NS 1000U
#define DEFAULT_ACTIVE_LOW true

typedef struct {
    char chip_path[PATH_MAX];
    int channel;
    unsigned int frequency_hz;
    unsigned int brightness;
    bool keep_on_exit;
    bool verbose;
} light_config_t;

typedef struct {
    char pwm_path[PATH_MAX];
    light_config_t cfg;
    uint32_t period_ns;
    bool exported_by_us;
    bool configured;
} pwm_light_t;

typedef enum {
    MODE_NONE = 0,
    MODE_ON,
    MODE_OFF,
    MODE_SET,
    MODE_BLINK,
    MODE_BREATHE,
    MODE_SOS,
    MODE_STROBE,
    MODE_HEARTBEAT,
    MODE_STATUS,
} light_mode_t;

typedef struct {
    light_mode_t mode;
    int repeat;
    unsigned int on_ms;
    unsigned int off_ms;
    unsigned int step_ms;
    unsigned int hold_ms;
} command_t;

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int sleep_ms(unsigned int ms)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = ms / 1000U;
    req.tv_nsec = (long)(ms % 1000U) * 1000000L;

    while (!g_stop && nanosleep(&req, &rem) < 0) {
        if (errno != EINTR)
            return -1;
        req = rem;
    }

    return g_stop ? -1 : 0;
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

    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;

    buf[n] = '\0';
    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[len - 1] = '\0';
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

static int parse_uint(const char *text, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0')
        return -1;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX)
        return -1;

    *out = (unsigned int)value;
    return 0;
}

static int parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0')
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT_MIN || value > INT_MAX)
        return -1;

    *out = (int)value;
    return 0;
}

static int join_path(char *out, size_t out_size, const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool need_slash = left_len > 0 && left[left_len - 1] != '/';
    size_t total = left_len + (need_slash ? 1U : 0U) + right_len;

    if (out_size == 0 || total >= out_size) {
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
        char name_path[PATH_MAX];
        char read_name[128];
        char link_path[PATH_MAX];
        char link_target[PATH_MAX];
        ssize_t link_len;

        if (strncmp(entry->d_name, "pwmchip", 7) != 0)
            continue;

        snprintf(link_path, sizeof(link_path), "/sys/class/pwm/%s", entry->d_name);
        link_len = readlink(link_path, link_target, sizeof(link_target) - 1);
        if (link_len > 0) {
            link_target[link_len] = '\0';
            if (strstr(link_target, name) != NULL) {
                snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
                closedir(dir);
                return 0;
            }
        }

        snprintf(link_path, sizeof(link_path), "/sys/class/pwm/%s/device", entry->d_name);
        link_len = readlink(link_path, link_target, sizeof(link_target) - 1);
        if (link_len > 0) {
            link_target[link_len] = '\0';
            if (strstr(link_target, name) != NULL) {
                snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
                closedir(dir);
                return 0;
            }
        }

        snprintf(name_path, sizeof(name_path), "/sys/class/pwm/%s/device/name", entry->d_name);
        if (read_file_trim(name_path, read_name, sizeof(read_name)) == 0 && strcmp(read_name, name) == 0) {
            snprintf(out, out_size, "/sys/class/pwm/%s", entry->d_name);
            closedir(dir);
            return 0;
        }

        snprintf(name_path, sizeof(name_path), "/sys/class/pwm/%s/device/uevent", entry->d_name);
        if (read_file_trim(name_path, read_name, sizeof(read_name)) == 0 && strstr(read_name, name) != NULL) {
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
    unsigned int chip_num;

    if (strncmp(arg, "/sys/class/pwm/", 15) == 0) {
        snprintf(out, out_size, "%s", arg);
        return 0;
    }

    if (strncmp(arg, "pwmchip", 7) == 0) {
        snprintf(out, out_size, "/sys/class/pwm/%s", arg);
        return 0;
    }

    if (parse_uint(arg, &chip_num) == 0) {
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

    if (frequency_hz == 0)
        frequency_hz = DEFAULT_FREQUENCY_HZ;

    period = 1000000000ULL / frequency_hz;
    if (period < MIN_PERIOD_NS)
        period = MIN_PERIOD_NS;
    if (period > UINT32_MAX)
        period = UINT32_MAX;

    return (uint32_t)period;
}

static uint32_t duty_from_percent(uint32_t period_ns, unsigned int percent)
{
    unsigned int duty_percent;

    percent = clamp_u32(percent, 0, MAX_BRIGHTNESS);
    duty_percent = DEFAULT_ACTIVE_LOW ? (MAX_BRIGHTNESS - percent) : percent;
    return (uint32_t)(((uint64_t)period_ns * duty_percent) / 100U);
}

static int pwm_export_if_needed(pwm_light_t *light)
{
    char export_path[PATH_MAX];
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

    for (int i = 0; i < 50; ++i) {
        if (path_exists(light->pwm_path)) {
            light->exported_by_us = true;
            return 0;
        }
        sleep_ms(10);
    }

    errno = ETIMEDOUT;
    return -1;
}

static int pwm_enable(pwm_light_t *light, bool enable)
{
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), light->pwm_path, "enable") < 0)
        return -1;
    return write_uint_file(path, enable ? 1U : 0U);
}

static int pwm_set_period(pwm_light_t *light, uint32_t period_ns)
{
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), light->pwm_path, "period") < 0)
        return -1;
    return write_uint_file(path, period_ns);
}

static int pwm_set_duty(pwm_light_t *light, uint32_t duty_ns)
{
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), light->pwm_path, "duty_cycle") < 0)
        return -1;
    return write_uint_file(path, duty_ns);
}

static int pwm_set_brightness(pwm_light_t *light, unsigned int percent)
{
    uint32_t duty = duty_from_percent(light->period_ns, percent);
    return pwm_set_duty(light, duty);
}

static int pwm_init(pwm_light_t *light)
{
    light->period_ns = period_from_frequency(light->cfg.frequency_hz);

    if (pwm_export_if_needed(light) < 0)
        return -1;

    if (pwm_enable(light, false) < 0 && errno != EINVAL)
        return -1;

    if (pwm_set_period(light, light->period_ns) < 0)
        return -1;

    if (pwm_set_brightness(light, 0) < 0)
        return -1;

    if (pwm_enable(light, true) < 0)
        return -1;

    light->configured = true;
    return 0;
}

static int pwm_cleanup(pwm_light_t *light)
{
    char unexport_path[PATH_MAX];
    int rc = 0;

    if (!light)
        return 0;

    if (light->configured && !light->cfg.keep_on_exit) {
        if (pwm_set_brightness(light, 0) < 0)
            rc = -1;
        if (pwm_enable(light, true) < 0)
            rc = -1;
    }

    if (light->exported_by_us && !light->cfg.keep_on_exit && !DEFAULT_ACTIVE_LOW) {
        if (join_path(unexport_path, sizeof(unexport_path), light->cfg.chip_path, "unexport") < 0)
            return -1;
        if (write_uint_file(unexport_path, (unsigned int)light->cfg.channel) < 0)
            rc = -1;
    }

    return rc;
}

static int effect_solid(pwm_light_t *light, unsigned int brightness, bool keep_on)
{
    if (pwm_set_brightness(light, brightness) < 0)
        return -1;
    if (pwm_enable(light, true) < 0)
        return -1;
    if (keep_on)
        light->cfg.keep_on_exit = true;
    return 0;
}

static int effect_blink(pwm_light_t *light, int repeat, unsigned int on_ms, unsigned int off_ms)
{
    int count = 0;

    while (!g_stop && (repeat < 0 || count < repeat)) {
        if (pwm_set_brightness(light, light->cfg.brightness) < 0)
            return -1;
        if (sleep_ms(on_ms) < 0)
            break;

        if (pwm_set_brightness(light, 0) < 0)
            return -1;
        if (sleep_ms(off_ms) < 0)
            break;

        count++;
    }

    return 0;
}

static int effect_strobe(pwm_light_t *light, int repeat)
{
    int count = 0;

    while (!g_stop && (repeat < 0 || count < repeat)) {
        if (pwm_set_brightness(light, light->cfg.brightness) < 0)
            return -1;
        if (sleep_ms(45) < 0)
            break;
        if (pwm_set_brightness(light, 0) < 0)
            return -1;
        if (sleep_ms(80) < 0)
            break;
        count++;
    }

    return 0;
}

static int effect_breathe(pwm_light_t *light, int repeat, unsigned int step_ms, unsigned int hold_ms)
{
    int count = 0;
    unsigned int max = light->cfg.brightness;

    if (max == 0)
        max = DEFAULT_BRIGHTNESS;

    while (!g_stop && (repeat < 0 || count < repeat)) {
        for (unsigned int b = 0; b <= max && !g_stop; b++) {
            if (pwm_set_brightness(light, b) < 0)
                return -1;
            if (sleep_ms(step_ms) < 0)
                break;
        }
        if (!g_stop && hold_ms > 0 && sleep_ms(hold_ms) < 0)
            break;
        for (unsigned int b = max; b > 0 && !g_stop; b--) {
            if (pwm_set_brightness(light, b - 1) < 0)
                return -1;
            if (sleep_ms(step_ms) < 0)
                break;
        }
        if (!g_stop && hold_ms > 0 && sleep_ms(hold_ms) < 0)
            break;
        count++;
    }

    return 0;
}

static int pulse(pwm_light_t *light, unsigned int brightness, unsigned int on_ms, unsigned int off_ms)
{
    if (pwm_set_brightness(light, brightness) < 0)
        return -1;
    if (sleep_ms(on_ms) < 0)
        return -1;
    if (pwm_set_brightness(light, 0) < 0)
        return -1;
    if (sleep_ms(off_ms) < 0)
        return -1;
    return 0;
}

static int effect_sos(pwm_light_t *light, int repeat)
{
    int count = 0;
    unsigned int b = light->cfg.brightness;

    while (!g_stop && (repeat < 0 || count < repeat)) {
        for (int i = 0; i < 3 && !g_stop; i++)
            if (pulse(light, b, 180, 180) < 0)
                return g_stop ? 0 : -1;
        if (sleep_ms(240) < 0)
            break;

        for (int i = 0; i < 3 && !g_stop; i++)
            if (pulse(light, b, 520, 220) < 0)
                return g_stop ? 0 : -1;
        if (sleep_ms(240) < 0)
            break;

        for (int i = 0; i < 3 && !g_stop; i++)
            if (pulse(light, b, 180, 180) < 0)
                return g_stop ? 0 : -1;
        if (sleep_ms(1200) < 0)
            break;

        count++;
    }

    return 0;
}

static int effect_heartbeat(pwm_light_t *light, int repeat)
{
    int count = 0;
    unsigned int b = light->cfg.brightness;

    while (!g_stop && (repeat < 0 || count < repeat)) {
        if (pulse(light, b, 90, 80) < 0)
            return g_stop ? 0 : -1;
        if (pulse(light, b, 140, 650) < 0)
            return g_stop ? 0 : -1;
        count++;
    }

    return 0;
}

static int print_pwm_status(pwm_light_t *light)
{
    char path[PATH_MAX];
    char value[128];

    printf("chip: %s\n", light->cfg.chip_path);
    printf("channel: %d\n", light->cfg.channel);
    printf("pwm: %s\n", light->pwm_path);
    printf("frequency_hz: %u\n", light->cfg.frequency_hz);

    if (join_path(path, sizeof(path), light->pwm_path, "period") < 0)
        return -1;
    if (read_file_trim(path, value, sizeof(value)) == 0)
        printf("period_ns: %s\n", value);

    if (join_path(path, sizeof(path), light->pwm_path, "duty_cycle") < 0)
        return -1;
    if (read_file_trim(path, value, sizeof(value)) == 0)
        printf("duty_cycle_ns: %s\n", value);

    if (join_path(path, sizeof(path), light->pwm_path, "enable") < 0)
        return -1;
    if (read_file_trim(path, value, sizeof(value)) == 0)
        printf("enable: %s\n", value);

    return 0;
}

static const char *mode_to_text(light_mode_t mode)
{
    switch (mode) {
    case MODE_ON: return "on";
    case MODE_OFF: return "off";
    case MODE_SET: return "set";
    case MODE_BLINK: return "blink";
    case MODE_BREATHE: return "breathe";
    case MODE_SOS: return "sos";
    case MODE_STROBE: return "strobe";
    case MODE_HEARTBEAT: return "heartbeat";
    case MODE_STATUS: return "status";
    default: return "none";
    }
}

static void print_usage(const char *argv0)
{
    printf(
        "Usage: %s [options] <mode>\n"
        "\n"
        "Modes:\n"
        "  on                 Turn headlight on and keep it on after exit\n"
        "  off                Turn headlight off\n"
        "  set <0-100>        Set brightness percent and keep it after exit; 0=off, 100=max\n"
        "  blink              Blink on/off\n"
        "  breathe            Smooth breathing effect\n"
        "  sos                Morse SOS effect\n"
        "  strobe             Fast flash effect\n"
        "  heartbeat          Double-pulse heartbeat effect\n"
        "  status             Print current PWM state\n"
        "\n"
        "Options:\n"
        "  --chip <chip>      pwmchipN, N, sysfs path, or device name. Default auto: %s\n"
        "  --channel <n>      PWM channel under chip. Default: %d\n"
        "  --freq <hz>        PWM frequency. Default: %d\n"
        "  --brightness <%%>  Effect brightness, 0=off and 100=max. Default: %d\n"
        "  --repeat <n>       Effect cycles. Default: 1, use -1 for forever\n"
        "  --on-ms <ms>       Blink on time. Default: 500\n"
        "  --off-ms <ms>      Blink off time. Default: 500\n"
        "  --step-ms <ms>     Breathe step time. Default: 12\n"
        "  --hold-ms <ms>     Breathe high/low hold time. Default: 120\n"
        "  --keep             Keep current PWM output after effect exits\n"
        "  --verbose          Print selected chip and mode\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Examples:\n"
        "  %s on\n"
        "  %s set 35\n"
        "  %s blink --repeat 10 --brightness 80\n"
        "  %s breathe --repeat -1 --brightness 70\n"
        "  %s sos --repeat 3\n",
        argv0, DEFAULT_PWM_NAME, DEFAULT_PWM_CHANNEL, DEFAULT_FREQUENCY_HZ,
        DEFAULT_BRIGHTNESS, argv0, argv0, argv0, argv0, argv0);
}

static int parse_args(int argc, char **argv, light_config_t *cfg, command_t *cmd)
{
    int i = 1;

    memset(cfg, 0, sizeof(*cfg));
    memset(cmd, 0, sizeof(*cmd));

    cfg->channel = DEFAULT_PWM_CHANNEL;
    cfg->frequency_hz = DEFAULT_FREQUENCY_HZ;
    cfg->brightness = DEFAULT_BRIGHTNESS;
    cmd->repeat = 1;
    cmd->on_ms = 500;
    cmd->off_ms = 500;
    cmd->step_ms = 12;
    cmd->hold_ms = 120;

    if (find_pwmchip_by_name(DEFAULT_PWM_NAME, cfg->chip_path, sizeof(cfg->chip_path)) < 0)
        snprintf(cfg->chip_path, sizeof(cfg->chip_path), "/sys/class/pwm/pwmchip0");

    while (i < argc) {
        unsigned int uval;
        int ival;

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--chip") == 0) {
            if (++i >= argc)
                return -1;
            if (set_pwm_path_from_chip_arg(argv[i], cfg->chip_path, sizeof(cfg->chip_path)) < 0)
                return -1;
        } else if (strcmp(argv[i], "--channel") == 0) {
            if (++i >= argc || parse_int(argv[i], &ival) < 0 || ival < 0)
                return -1;
            cfg->channel = ival;
        } else if (strcmp(argv[i], "--freq") == 0) {
            if (++i >= argc || parse_uint(argv[i], &uval) < 0 || uval == 0)
                return -1;
            cfg->frequency_hz = uval;
        } else if (strcmp(argv[i], "--brightness") == 0) {
            if (++i >= argc || parse_uint(argv[i], &uval) < 0)
                return -1;
            cfg->brightness = clamp_u32(uval, 0, MAX_BRIGHTNESS);
        } else if (strcmp(argv[i], "--repeat") == 0) {
            if (++i >= argc || parse_int(argv[i], &ival) < 0)
                return -1;
            cmd->repeat = ival;
        } else if (strcmp(argv[i], "--on-ms") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cmd->on_ms) < 0)
                return -1;
        } else if (strcmp(argv[i], "--off-ms") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cmd->off_ms) < 0)
                return -1;
        } else if (strcmp(argv[i], "--step-ms") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cmd->step_ms) < 0 || cmd->step_ms == 0)
                return -1;
        } else if (strcmp(argv[i], "--hold-ms") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cmd->hold_ms) < 0)
                return -1;
        } else if (strcmp(argv[i], "--keep") == 0) {
            cfg->keep_on_exit = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (argv[i][0] == '-') {
            return -1;
        } else {
            if (cmd->mode != MODE_NONE)
                return -1;

            if (strcmp(argv[i], "on") == 0) {
                cmd->mode = MODE_ON;
                cfg->keep_on_exit = true;
            } else if (strcmp(argv[i], "off") == 0) {
                cmd->mode = MODE_OFF;
            } else if (strcmp(argv[i], "set") == 0) {
                cmd->mode = MODE_SET;
                cfg->keep_on_exit = true;
                if (++i >= argc || parse_uint(argv[i], &uval) < 0)
                    return -1;
                cfg->brightness = clamp_u32(uval, 0, MAX_BRIGHTNESS);
            } else if (strcmp(argv[i], "blink") == 0) {
                cmd->mode = MODE_BLINK;
            } else if (strcmp(argv[i], "breathe") == 0 || strcmp(argv[i], "breath") == 0) {
                cmd->mode = MODE_BREATHE;
            } else if (strcmp(argv[i], "sos") == 0) {
                cmd->mode = MODE_SOS;
            } else if (strcmp(argv[i], "strobe") == 0) {
                cmd->mode = MODE_STROBE;
            } else if (strcmp(argv[i], "heartbeat") == 0) {
                cmd->mode = MODE_HEARTBEAT;
            } else if (strcmp(argv[i], "status") == 0) {
                cmd->mode = MODE_STATUS;
            } else {
                return -1;
            }
        }
        i++;
    }

    if (cmd->mode == MODE_NONE)
        return -1;

    return 0;
}

static int run_command(pwm_light_t *light, const command_t *cmd)
{
    switch (cmd->mode) {
    case MODE_ON:
        return effect_solid(light, light->cfg.brightness, true);
    case MODE_OFF:
        return effect_solid(light, 0, false);
    case MODE_SET:
        return effect_solid(light, light->cfg.brightness, true);
    case MODE_BLINK:
        return effect_blink(light, cmd->repeat, cmd->on_ms, cmd->off_ms);
    case MODE_BREATHE:
        return effect_breathe(light, cmd->repeat, cmd->step_ms, cmd->hold_ms);
    case MODE_SOS:
        return effect_sos(light, cmd->repeat);
    case MODE_STROBE:
        return effect_strobe(light, cmd->repeat);
    case MODE_HEARTBEAT:
        return effect_heartbeat(light, cmd->repeat);
    case MODE_STATUS:
        return print_pwm_status(light);
    default:
        errno = EINVAL;
        return -1;
    }
}

int main(int argc, char **argv)
{
    light_config_t cfg;
    command_t cmd;
    pwm_light_t light;
    int rc;

    if (parse_args(argc, argv, &cfg, &cmd) < 0) {
        print_usage(argv[0]);
        return 2;
    }

    memset(&light, 0, sizeof(light));
    light.cfg = cfg;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (cmd.mode == MODE_STATUS) {
        if (pwm_export_if_needed(&light) < 0) {
            fprintf(stderr, "headlight: failed to find/export PWM %s channel %d: %s\n",
                    cfg.chip_path, cfg.channel, strerror(errno));
            return 1;
        }
        rc = print_pwm_status(&light);
        if (light.exported_by_us) {
            char unexport_path[PATH_MAX];
            if (join_path(unexport_path, sizeof(unexport_path), light.cfg.chip_path, "unexport") < 0)
                rc = -1;
            if (write_uint_file(unexport_path, (unsigned int)light.cfg.channel) < 0 && rc == 0)
                rc = -1;
        }
        return rc == 0 ? 0 : 1;
    }

    if (pwm_init(&light) < 0) {
        fprintf(stderr, "headlight: failed to init PWM %s channel %d: %s\n",
                cfg.chip_path, cfg.channel, strerror(errno));
        return 1;
    }

    if (cfg.verbose) {
        printf("headlight: mode=%s chip=%s channel=%d freq=%u brightness=%u repeat=%d\n",
               mode_to_text(cmd.mode), cfg.chip_path, cfg.channel, cfg.frequency_hz,
               cfg.brightness, cmd.repeat);
    }

    rc = run_command(&light, &cmd);
    if (rc < 0) {
        fprintf(stderr, "headlight: mode %s failed: %s\n", mode_to_text(cmd.mode), strerror(errno));
    }

    if (pwm_cleanup(&light) < 0 && rc == 0) {
        fprintf(stderr, "headlight: cleanup warning: %s\n", strerror(errno));
        rc = -1;
    }

    return rc == 0 ? 0 : 1;
}
