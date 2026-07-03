#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef B1000000
#define B1000000 0010010
#endif

#define UART_SOF1 0xA5
#define UART_SOF2 0x5A
#define UART_VERSION 0x01
#define UART_HEADER_LEN 10
#define UART_CRC_LEN 2
#define UART_MAX_PAYLOAD 128
#define UART_MAX_FRAME (UART_HEADER_LEN + UART_MAX_PAYLOAD + UART_CRC_LEN)

#define UART_CMD_PING 0x0001
#define UART_CMD_GET_VERSION 0x0002
#define UART_RSP_BIT 0x8000

#define CAN_DLC 8

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint16_t cmd;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[UART_MAX_PAYLOAD];
} uart_frame_t;

typedef struct {
    int state;
    uint8_t buf[UART_MAX_FRAME];
    size_t len;
    size_t expected_len;
} uart_stream_parser_t;

typedef struct {
    const char *port;
    unsigned int baud;
    unsigned int timeout_ms;
    uint16_t seq;
    bool verbose;
} serial_config_t;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i32_le(const uint8_t *p)
{
    return (int32_t)get_le32(p);
}

static int16_t get_i16_le(const uint8_t *p)
{
    return (int16_t)get_le16(p);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t crc16_ccitt(uint16_t init, const uint8_t *data, size_t len)
{
    uint16_t crc = init;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc <<= 1;
        }
    }

    return crc;
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i)
            putchar(' ');
        printf("%02X", data[i]);
    }
}

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(int argc, char **argv, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t len = 0;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        int hi = -1;

        if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
            s += 2;

        for (; *s; s++) {
            int v;

            if (*s == ':' || *s == ',' || *s == '-' || *s == '_' || *s == ' ')
                continue;
            v = parse_hex_nibble(*s);
            if (v < 0)
                return -1;
            if (hi < 0) {
                hi = v;
            } else {
                if (len >= out_cap)
                    return -1;
                out[len++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }

        if (hi >= 0)
            return -1;
    }

    *out_len = len;
    return 0;
}

static int parse_u32_arg(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno || end == s || *end || v < min || v > max)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_i32_arg(const char *s, int32_t min, int32_t max, int32_t *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 0);
    if (errno || end == s || *end || v < min || v > max)
        return -1;
    *out = (int32_t)v;
    return 0;
}

static size_t uart_build_frame(uint16_t cmd, uint16_t seq, uint8_t flags,
                               const uint8_t *payload, uint16_t payload_len,
                               uint8_t *out, size_t out_cap)
{
    uint16_t crc;

    if (payload_len > UART_MAX_PAYLOAD ||
        out_cap < (size_t)UART_HEADER_LEN + payload_len + UART_CRC_LEN)
        return 0;

    out[0] = UART_SOF1;
    out[1] = UART_SOF2;
    out[2] = UART_VERSION;
    out[3] = flags;
    put_le16(&out[4], cmd);
    put_le16(&out[6], seq);
    put_le16(&out[8], payload_len);
    if (payload_len && payload)
        memcpy(&out[10], payload, payload_len);

    crc = crc16_ccitt(0xffff, &out[2], 8 + payload_len);
    put_le16(&out[10 + payload_len], crc);
    return UART_HEADER_LEN + payload_len + UART_CRC_LEN;
}

static int uart_decode_frame_bytes(const uint8_t *data, size_t len, uart_frame_t *frame)
{
    uint16_t payload_len;
    uint16_t rx_crc;
    uint16_t calc_crc;

    if (len < UART_HEADER_LEN + UART_CRC_LEN) {
        errno = EMSGSIZE;
        return -1;
    }
    if (data[0] != UART_SOF1 || data[1] != UART_SOF2) {
        errno = EBADMSG;
        return -1;
    }

    payload_len = get_le16(&data[8]);
    if (payload_len > UART_MAX_PAYLOAD ||
        len != (size_t)UART_HEADER_LEN + payload_len + UART_CRC_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    rx_crc = get_le16(&data[10 + payload_len]);
    calc_crc = crc16_ccitt(0xffff, &data[2], 8 + payload_len);
    if (rx_crc != calc_crc) {
        errno = EBADMSG;
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = data[2];
    frame->flags = data[3];
    frame->cmd = get_le16(&data[4]);
    frame->seq = get_le16(&data[6]);
    frame->payload_len = payload_len;
    if (payload_len)
        memcpy(frame->payload, &data[10], payload_len);

    return 0;
}

static void uart_parser_init(uart_stream_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->expected_len = 0;
}

static int uart_parser_feed(uart_stream_parser_t *parser, uint8_t b, uart_frame_t *frame)
{
    switch (parser->state) {
    case 0:
        if (b == UART_SOF1) {
            parser->buf[0] = b;
            parser->len = 1;
            parser->state = 1;
        }
        return 0;
    case 1:
        if (b == UART_SOF2) {
            parser->buf[parser->len++] = b;
            parser->state = 2;
        } else if (b == UART_SOF1) {
            parser->buf[0] = b;
            parser->len = 1;
        } else {
            parser->state = 0;
            parser->len = 0;
        }
        return 0;
    default:
        parser->buf[parser->len++] = b;
        if (parser->len == UART_HEADER_LEN) {
            uint16_t payload_len = get_le16(&parser->buf[8]);

            if (payload_len > UART_MAX_PAYLOAD) {
                uart_parser_init(parser);
                return -1;
            }
            parser->expected_len = UART_HEADER_LEN + payload_len + UART_CRC_LEN;
        }

        if (parser->expected_len && parser->len == parser->expected_len) {
            int ret = uart_decode_frame_bytes(parser->buf, parser->len, frame);
            uart_parser_init(parser);
            return ret == 0 ? 1 : -1;
        }

        if (parser->len >= sizeof(parser->buf)) {
            uart_parser_init(parser);
            return -1;
        }
        return 0;
    }
}

static void print_uart_payload_detail(const uart_frame_t *frame)
{
    int16_t status;
    bool is_rsp = (frame->cmd & UART_RSP_BIT) != 0;
    uint16_t base_cmd = (uint16_t)(frame->cmd & ~UART_RSP_BIT);

    if (!is_rsp || frame->payload_len < 2)
        return;

    status = get_i16_le(frame->payload);
    printf("status: %d\n", status);

    if (base_cmd == UART_CMD_PING && frame->payload_len > 2) {
        printf("ping_echo: ");
        print_hex(&frame->payload[2], frame->payload_len - 2);
        putchar('\n');
    } else if (base_cmd == UART_CMD_GET_VERSION && frame->payload_len > 2) {
        printf("version_string: ");
        for (uint16_t i = 2; i < frame->payload_len; i++) {
            uint8_t c = frame->payload[i];
            putchar((c >= 32 && c <= 126) ? (char)c : '.');
        }
        putchar('\n');
    }
}

static void print_uart_frame(const uart_frame_t *frame)
{
    printf("uart_frame:\n");
    printf("  version: 0x%02X\n", frame->version);
    printf("  flags: 0x%02X\n", frame->flags);
    printf("  cmd: 0x%04X%s\n", frame->cmd, (frame->cmd & UART_RSP_BIT) ? " response" : "");
    printf("  seq: %u\n", frame->seq);
    printf("  payload_len: %u\n", frame->payload_len);
    printf("  payload: ");
    print_hex(frame->payload, frame->payload_len);
    putchar('\n');
    print_uart_payload_detail(frame);
}

static speed_t baud_to_speed(unsigned int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    default: return 0;
    }
}

static int open_serial(const serial_config_t *cfg)
{
    struct termios tio;
    speed_t speed = baud_to_speed(cfg->baud);
    int fd;

    if (!speed) {
        errno = EINVAL;
        return -1;
    }

    fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (tcgetattr(fd, &tio) < 0) {
        close(fd);
        return -1;
    }

    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0 ||
        tcsetattr(fd, TCSANOW, &tio) < 0) {
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void sleep_1ms(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_1ms();
            continue;
        }
        return -1;
    }

    return 0;
}

static int serial_request(const serial_config_t *cfg, uint16_t cmd, const uint8_t *payload,
                          uint16_t payload_len, uart_frame_t *rsp)
{
    int fd;
    uint8_t tx[UART_MAX_FRAME];
    size_t tx_len;
    long long deadline;
    uart_stream_parser_t parser;

    tx_len = uart_build_frame(cmd, cfg->seq, 0, payload, payload_len, tx, sizeof(tx));
    if (!tx_len) {
        errno = EINVAL;
        return -1;
    }

    fd = open_serial(cfg);
    if (fd < 0)
        return -1;

    if (cfg->verbose) {
        printf("tx: ");
        print_hex(tx, tx_len);
        putchar('\n');
    }

    if (write_all(fd, tx, tx_len) < 0) {
        close(fd);
        return -1;
    }
    tcdrain(fd);

    uart_parser_init(&parser);
    deadline = monotonic_ms() + cfg->timeout_ms;

    while (monotonic_ms() < deadline) {
        fd_set rfds;
        struct timeval tv;
        uint8_t rx[64];
        ssize_t n;
        int ret;
        long long now = monotonic_ms();
        long long remain = deadline - now;

        if (remain < 0)
            remain = 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = (time_t)(remain / 1000);
        tv.tv_usec = (suseconds_t)((remain % 1000) * 1000);
        ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (ret == 0)
            break;

        n = read(fd, rx, sizeof(rx));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            close(fd);
            return -1;
        }

        for (ssize_t i = 0; i < n; i++) {
            ret = uart_parser_feed(&parser, rx[i], rsp);
            if (ret == 1) {
                if (cfg->verbose) {
                    printf("rx: ");
                    print_uart_frame(rsp);
                }
                close(fd);
                if (rsp->seq != cfg->seq || rsp->cmd != (cmd | UART_RSP_BIT)) {
                    errno = EBADMSG;
                    return -1;
                }
                return 0;
            }
        }
    }

    close(fd);
    errno = ETIMEDOUT;
    return -1;
}

static const char *can_status_text(uint8_t status)
{
    switch (status) {
    case 0x00: return "ok";
    case 0x01: return "invalid command";
    case 0x02: return "invalid parameter";
    case 0x03: return "invalid mode";
    case 0x04: return "CAN error";
    default: return "unknown";
    }
}

static const char *can_cmd_text(uint8_t cmd)
{
    switch (cmd) {
    case 0x01: return "set_mode";
    case 0x02: return "set_current_target";
    case 0x03: return "set_velocity_target";
    case 0x04: return "set_position_target";
    case 0x10: return "set_velocity_pid";
    case 0x11: return "read_velocity_pid";
    case 0x12: return "set_position_pid";
    case 0x13: return "read_position_pid";
    case 0x20: return "set_node_id";
    case 0x21: return "read_node_id";
    case 0x22: return "read_app_version";
    case 0x23: return "set_report_config";
    case 0x24: return "read_report_config";
    case 0x25: return "reset_can_config";
    case 0x26: return "set_foc_config";
    case 0x27: return "read_foc_config";
    case 0x30: return "calibrate_zero";
    case 0x31: return "read_zero";
    case 0x40: return "enter_boot_ota";
    case 0x60: return "management_discovery";
    case 0x61: return "identify";
    case 0x62: return "configure_node_id";
    case 0x80: return "position_report";
    case 0x81: return "velocity_report";
    default: return "unknown";
    }
}

static const char *pid_param_text(uint8_t param)
{
    switch (param) {
    case 0x00: return "P";
    case 0x01: return "I";
    case 0x02: return "D";
    case 0x03: return "I_ACC_MAX";
    case 0x04: return "OUT_MAX";
    default: return "unknown";
    }
}

static const char *foc_param_text(uint8_t param)
{
    switch (param) {
    case 0x00: return "pole_pairs";
    case 0x01: return "master_voltage_mv";
    case 0x02: return "control_hz";
    case 0x03: return "sensor_hz";
    case 0x04: return "phase_map";
    default: return "unknown";
    }
}

static void print_can_id_class(uint16_t id)
{
    if (id == 0x17e)
        printf("frame_class: management command, node_id=0x7E\n");
    else if (id == 0x1fe)
        printf("frame_class: management response, node_id=0x7E\n");
    else if (id == 0x27e)
        printf("frame_class: management report, node_id=0x7E\n");
    else if (id >= 0x101 && id <= 0x17f)
        printf("frame_class: host command, node_id=0x%02X\n", id - 0x100);
    else if (id >= 0x181 && id <= 0x1ff)
        printf("frame_class: motor response, node_id=0x%02X\n", id - 0x180);
    else if (id >= 0x201 && id <= 0x27f)
        printf("frame_class: motor report, node_id=0x%02X\n", id - 0x200);
    else if (id >= 0x401 && id <= 0x47f)
        printf("frame_class: ota control, node_id=0x%02X\n", id - 0x400);
    else if (id >= 0x501 && id <= 0x57f)
        printf("frame_class: ota response, node_id=0x%02X\n", id - 0x500);
    else
        printf("frame_class: unknown\n");
}

static bool can_is_response_id(uint16_t id)
{
    return id == 0x1fe || (id >= 0x181 && id <= 0x1ff) || (id >= 0x501 && id <= 0x57f);
}

static bool can_is_report_id(uint16_t id)
{
    return id == 0x27e || (id >= 0x201 && id <= 0x27f);
}

static void decode_can_payload(uint16_t id, const uint8_t d[CAN_DLC])
{
    uint8_t cmd = d[0];
    bool is_rsp = can_is_response_id(id);
    bool is_report = can_is_report_id(id);

    printf("cmd: 0x%02X (%s)\n", cmd, can_cmd_text(cmd));
    if (is_rsp)
        printf("status: 0x%02X (%s)\n", d[1], can_status_text(d[1]));

    switch (cmd) {
    case 0x01:
        printf("enable: %u\n", d[1]);
        printf("control_mode: %u (%s)\n", d[2],
               d[2] == 0 ? "current" : d[2] == 1 ? "velocity" : d[2] == 2 ? "position" : "unknown");
        break;
    case 0x02:
        printf("current_target: %d\n", get_i32_le(&d[1]));
        break;
    case 0x03:
        printf("velocity_target_mrad_s: %d\n", get_i32_le(&d[1]));
        break;
    case 0x04:
        printf("position_target_mrad: %d\n", get_i32_le(&d[1]));
        break;
    case 0x10:
    case 0x12:
        printf("pid_param: 0x%02X (%s)\n", d[1], pid_param_text(d[1]));
        printf("pid_value_raw: %d\n", get_i32_le(&d[2]));
        if (d[1] <= 0x02)
            printf("pid_value_float: %.3f\n", get_i32_le(&d[2]) / 1000.0);
        break;
    case 0x11:
    case 0x13:
        printf("pid_param: 0x%02X (%s)\n", is_rsp ? d[2] : d[1],
               pid_param_text(is_rsp ? d[2] : d[1]));
        if (is_rsp) {
            printf("pid_value_raw: %d\n", get_i32_le(&d[3]));
            if (d[2] <= 0x02)
                printf("pid_value_float: %.3f\n", get_i32_le(&d[3]) / 1000.0);
        }
        break;
    case 0x21:
    case 0x20:
        if (is_rsp)
            printf("node_id: 0x%02X\n", d[2]);
        else
            printf("node_id: 0x%02X\n", d[1]);
        break;
    case 0x22:
        if (is_rsp)
            printf("app_version: %u.%u.%u\n", d[2], d[3], d[4]);
        break;
    case 0x23:
        if (is_rsp) {
            printf("report_enable: %u\n", d[2]);
            printf("report_period_ms: %u\n", get_le16(&d[3]));
        } else {
            printf("report_enable: %u\n", d[1]);
            printf("report_period_ms: %u\n", get_le16(&d[2]));
        }
        break;
    case 0x24:
        if (is_rsp) {
            printf("report_enable: %u\n", d[2]);
            printf("report_period_ms: %u\n", get_le16(&d[3]));
        }
        break;
    case 0x25:
        if (!is_rsp)
            printf("confirm_code: 0x%02X\n", d[1]);
        break;
    case 0x26:
        printf("foc_param: 0x%02X (%s)\n", d[1], foc_param_text(d[1]));
        printf("foc_value: %u\n", get_le32(&d[2]));
        break;
    case 0x27:
        if (is_rsp) {
            printf("foc_param: 0x%02X (%s)\n", d[2], foc_param_text(d[2]));
            printf("foc_value: %u\n", get_le32(&d[3]));
        } else {
            printf("foc_param: 0x%02X (%s)\n", d[1], foc_param_text(d[1]));
        }
        break;
    case 0x31:
        if (is_rsp)
            printf("zero_mrad: %d\n", get_i32_le(&d[2]));
        break;
    case 0x60:
        printf("configured: %u\n", d[1]);
        printf("node_id: 0x%02X\n", d[2]);
        printf("uid32: 0x%08X\n", get_le32(&d[3]));
        break;
    case 0x61:
        if (is_rsp) {
            printf("actual_hz: %u\n", d[2]);
            printf("actual_duration_s: %u\n", d[3]);
            printf("uid32: 0x%08X\n", get_le32(&d[4]));
        } else {
            printf("blink_hz: %u\n", d[1]);
            printf("duration_s: %u\n", d[2]);
            printf("uid32: 0x%08X\n", get_le32(&d[3]));
        }
        break;
    case 0x62:
        if (is_rsp) {
            printf("new_node_id: 0x%02X\n", d[2]);
            printf("uid32: 0x%08X\n", get_le32(&d[3]));
        } else {
            printf("new_node_id: 0x%02X\n", d[1]);
            printf("uid32: 0x%08X\n", get_le32(&d[3]));
        }
        break;
    case 0x80:
        if (is_report || is_rsp)
            printf("position_mrad: %d\n", get_i32_le(&d[1]));
        break;
    case 0x81:
        if (is_report || is_rsp)
            printf("velocity_mrad_s: %d\n", get_i32_le(&d[1]));
        break;
    default:
        break;
    }
}

static int cmd_uart_decode(int argc, char **argv)
{
    uint8_t data[UART_MAX_FRAME];
    size_t len;
    uart_frame_t frame;

    if (argc < 1 || parse_hex_bytes(argc, argv, data, sizeof(data), &len) < 0) {
        fprintf(stderr, "invalid UART frame hex\n");
        return 2;
    }

    if (uart_decode_frame_bytes(data, len, &frame) < 0) {
        fprintf(stderr, "decode failed: %s\n", strerror(errno));
        return 1;
    }

    print_uart_frame(&frame);
    return 0;
}

static int cmd_uart_build(int argc, char **argv)
{
    uint32_t cmd;
    uint32_t seq;
    uint8_t payload[UART_MAX_PAYLOAD];
    uint8_t frame[UART_MAX_FRAME];
    size_t payload_len = 0;
    size_t frame_len;

    if (argc < 2 || parse_u32_arg(argv[0], 0, 0xffff, &cmd) < 0 ||
        parse_u32_arg(argv[1], 0, 0xffff, &seq) < 0) {
        fprintf(stderr, "usage: protocol_tool uart build <cmd> <seq> [payload-hex...]\n");
        return 2;
    }

    if (argc > 2 && parse_hex_bytes(argc - 2, &argv[2], payload, sizeof(payload), &payload_len) < 0) {
        fprintf(stderr, "invalid payload hex\n");
        return 2;
    }

    frame_len = uart_build_frame((uint16_t)cmd, (uint16_t)seq, 0, payload, (uint16_t)payload_len,
                                 frame, sizeof(frame));
    if (!frame_len)
        return 1;

    print_hex(frame, frame_len);
    putchar('\n');
    return 0;
}

static int cmd_uart_request(int argc, char **argv, uint16_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    serial_config_t cfg = {
        .port = "/dev/ttyS0",
        .baud = 1000000,
        .timeout_ms = 1000,
        .seq = 1,
        .verbose = false,
    };
    uart_frame_t rsp;

    for (int i = 0; i < argc; i++) {
        uint32_t uval;

        if (strcmp(argv[i], "--port") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--port requires value\n");
                return 2;
            }
            cfg.port = argv[i];
        } else if (strcmp(argv[i], "--baud") == 0) {
            if (++i >= argc || parse_u32_arg(argv[i], 1, 4000000, &uval) < 0) {
                fprintf(stderr, "--baud requires integer\n");
                return 2;
            }
            cfg.baud = uval;
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (++i >= argc || parse_u32_arg(argv[i], 1, 60000, &uval) < 0) {
                fprintf(stderr, "--timeout-ms requires integer\n");
                return 2;
            }
            cfg.timeout_ms = uval;
        } else if (strcmp(argv[i], "--seq") == 0) {
            if (++i >= argc || parse_u32_arg(argv[i], 0, 0xffff, &uval) < 0) {
                fprintf(stderr, "--seq requires 0..65535\n");
                return 2;
            }
            cfg.seq = (uint16_t)uval;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (serial_request(&cfg, cmd, payload, payload_len, &rsp) < 0) {
        fprintf(stderr, "UART request failed on %s: %s\n", cfg.port, strerror(errno));
        return 1;
    }

    print_uart_frame(&rsp);
    return 0;
}

static int cmd_uart_ping(int argc, char **argv)
{
    uint8_t payload[UART_MAX_PAYLOAD];
    size_t payload_len = 0;
    int opt_argc = argc;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--payload") == 0) {
            opt_argc = i;
            if (++i >= argc || parse_hex_bytes(argc - i, &argv[i], payload, sizeof(payload), &payload_len) < 0) {
                fprintf(stderr, "--payload requires hex bytes\n");
                return 2;
            }
            break;
        }
    }

    return cmd_uart_request(opt_argc, argv, UART_CMD_PING, payload, (uint16_t)payload_len);
}

static int cmd_uart_version(int argc, char **argv)
{
    return cmd_uart_request(argc, argv, UART_CMD_GET_VERSION, NULL, 0);
}

static int cmd_can_decode(int argc, char **argv)
{
    uint32_t id;
    uint8_t data[CAN_DLC];
    size_t len;

    if (argc < 2 || parse_u32_arg(argv[0], 0, 0x7ff, &id) < 0 ||
        parse_hex_bytes(argc - 1, &argv[1], data, sizeof(data), &len) < 0 || len != CAN_DLC) {
        fprintf(stderr, "usage: protocol_tool can decode <std-id> <8-byte-data-hex...>\n");
        return 2;
    }

    printf("can_frame:\n");
    printf("id: 0x%03X\n", id);
    print_can_id_class((uint16_t)id);
    printf("data: ");
    print_hex(data, sizeof(data));
    putchar('\n');
    decode_can_payload((uint16_t)id, data);
    return 0;
}

static int cmd_can_build(int argc, char **argv)
{
    uint32_t id;
    uint32_t cmd;
    uint8_t data[CAN_DLC] = {0};

    if (argc < 2 || parse_u32_arg(argv[0], 0, 0x7ff, &id) < 0 ||
        parse_u32_arg(argv[1], 0, 0xff, &cmd) < 0) {
        fprintf(stderr, "usage: protocol_tool can build <std-id> <cmd> [args...]\n");
        return 2;
    }

    data[0] = (uint8_t)cmd;

    if (cmd == 0x01) {
        uint32_t enable;
        uint32_t mode;
        if (argc != 4 || parse_u32_arg(argv[2], 0, 1, &enable) < 0 ||
            parse_u32_arg(argv[3], 0, 2, &mode) < 0) {
            fprintf(stderr, "set_mode args: <enable 0|1> <mode 0=current 1=velocity 2=position>\n");
            return 2;
        }
        data[1] = (uint8_t)enable;
        data[2] = (uint8_t)mode;
    } else if (cmd == 0x02 || cmd == 0x03 || cmd == 0x04) {
        int32_t value;
        if (argc != 3 || parse_i32_arg(argv[2], INT32_MIN, INT32_MAX, &value) < 0) {
            fprintf(stderr, "target args: <int32-value>\n");
            return 2;
        }
        put_le32(&data[1], (uint32_t)value);
    } else if (cmd == 0x10 || cmd == 0x12 || cmd == 0x26) {
        uint32_t param;
        uint32_t value;
        if (argc != 4 || parse_u32_arg(argv[2], 0, 0xff, &param) < 0 ||
            parse_u32_arg(argv[3], 0, UINT32_MAX, &value) < 0) {
            fprintf(stderr, "set-param args: <param> <uint32-value>\n");
            return 2;
        }
        data[1] = (uint8_t)param;
        put_le32(&data[2], value);
    } else if (cmd == 0x11 || cmd == 0x13 || cmd == 0x27) {
        uint32_t param;
        if (argc != 3 || parse_u32_arg(argv[2], 0, 0xff, &param) < 0) {
            fprintf(stderr, "read-param args: <param>\n");
            return 2;
        }
        data[1] = (uint8_t)param;
    } else if (cmd == 0x20) {
        uint32_t node;
        if (argc != 3 || parse_u32_arg(argv[2], 0, 0xff, &node) < 0) {
            fprintf(stderr, "set_node_id args: <node-id>\n");
            return 2;
        }
        data[1] = (uint8_t)node;
    } else if (cmd == 0x23) {
        uint32_t enable;
        uint32_t period;
        if (argc != 4 || parse_u32_arg(argv[2], 0, 1, &enable) < 0 ||
            parse_u32_arg(argv[3], 0, 0xffff, &period) < 0) {
            fprintf(stderr, "report config args: <enable> <period-ms>\n");
            return 2;
        }
        data[1] = (uint8_t)enable;
        put_le16(&data[2], (uint16_t)period);
    } else if (cmd == 0x25) {
        data[1] = 0xa5;
    } else if (cmd == 0x61) {
        uint32_t hz;
        uint32_t sec;
        uint32_t uid;
        if (argc != 5 || parse_u32_arg(argv[2], 0, 255, &hz) < 0 ||
            parse_u32_arg(argv[3], 0, 255, &sec) < 0 ||
            parse_u32_arg(argv[4], 0, UINT32_MAX, &uid) < 0) {
            fprintf(stderr, "identify args: <hz> <duration-s> <uid32>\n");
            return 2;
        }
        data[1] = (uint8_t)hz;
        data[2] = (uint8_t)sec;
        put_le32(&data[3], uid);
    } else if (cmd == 0x62) {
        uint32_t node;
        uint32_t uid;
        if (argc != 4 || parse_u32_arg(argv[2], 0, 255, &node) < 0 ||
            parse_u32_arg(argv[3], 0, UINT32_MAX, &uid) < 0) {
            fprintf(stderr, "configure_node_id args: <new-node-id> <uid32>\n");
            return 2;
        }
        data[1] = (uint8_t)node;
        put_le32(&data[3], uid);
    } else if (argc > 2) {
        size_t len;
        if (parse_hex_bytes(argc - 2, &argv[2], &data[1], CAN_DLC - 1, &len) < 0) {
            fprintf(stderr, "invalid raw CAN args\n");
            return 2;
        }
    }

    printf("id: 0x%03X\n", id);
    printf("data: ");
    print_hex(data, sizeof(data));
    putchar('\n');
    decode_can_payload((uint16_t)id, data);
    return 0;
}

static int cmd_selftest(void)
{
    uint8_t payload[] = {0x11, 0x22, 0x33};
    uint8_t frame[UART_MAX_FRAME];
    size_t frame_len;
    uart_frame_t decoded;
    uint8_t rsp_payload[2 + sizeof(payload)];
    uint8_t rsp[UART_MAX_FRAME];
    size_t rsp_len;
    uint8_t can_data[CAN_DLC] = {0x60, 0x00, 0x7e, 0x78, 0x56, 0x34, 0x12, 0x00};

    frame_len = uart_build_frame(UART_CMD_PING, 1, 0, payload, sizeof(payload), frame, sizeof(frame));
    if (!frame_len || uart_decode_frame_bytes(frame, frame_len, &decoded) < 0)
        return 1;
    if (decoded.cmd != UART_CMD_PING || decoded.seq != 1 || decoded.payload_len != sizeof(payload) ||
        memcmp(decoded.payload, payload, sizeof(payload)) != 0)
        return 1;

    put_le16(rsp_payload, 0);
    memcpy(&rsp_payload[2], payload, sizeof(payload));
    rsp_len = uart_build_frame(UART_CMD_PING | UART_RSP_BIT, 1, 0, rsp_payload,
                               sizeof(rsp_payload), rsp, sizeof(rsp));
    if (!rsp_len || uart_decode_frame_bytes(rsp, rsp_len, &decoded) < 0 ||
        decoded.cmd != (UART_CMD_PING | UART_RSP_BIT) || get_i16_le(decoded.payload) != 0)
        return 1;

    printf("selftest: ok\n");
    printf("sample_uart_ping_req: ");
    print_hex(frame, frame_len);
    putchar('\n');
    printf("sample_uart_ping_rsp: ");
    print_hex(rsp, rsp_len);
    putchar('\n');
    printf("sample_can_decode:\n");
    print_can_id_class(0x27e);
    decode_can_payload(0x27e, can_data);
    return 0;
}

static void print_usage(const char *argv0)
{
    printf(
        "Usage:\n"
        "  %s selftest\n"
        "  %s uart build <cmd> <seq> [payload-hex...]\n"
        "  %s uart decode <frame-hex...>\n"
        "  %s uart ping [--port /dev/ttyS0] [--baud 1000000] [--seq n] [--timeout-ms n] [--verbose] [--payload hex...]\n"
        "  %s uart version [--port /dev/ttyS0] [--baud 1000000] [--seq n] [--timeout-ms n] [--verbose]\n"
        "  %s can build <std-id> <cmd> [args...]\n"
        "  %s can decode <std-id> <8-byte-data-hex...>\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "selftest") == 0)
        return cmd_selftest();

    if (strcmp(argv[1], "uart") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[2], "build") == 0)
            return cmd_uart_build(argc - 3, &argv[3]);
        if (strcmp(argv[2], "decode") == 0)
            return cmd_uart_decode(argc - 3, &argv[3]);
        if (strcmp(argv[2], "ping") == 0)
            return cmd_uart_ping(argc - 3, &argv[3]);
        if (strcmp(argv[2], "version") == 0)
            return cmd_uart_version(argc - 3, &argv[3]);
    } else if (strcmp(argv[1], "can") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[2], "build") == 0)
            return cmd_can_build(argc - 3, &argv[3]);
        if (strcmp(argv[2], "decode") == 0)
            return cmd_can_decode(argc - 3, &argv[3]);
    }

    print_usage(argv[0]);
    return 2;
}
