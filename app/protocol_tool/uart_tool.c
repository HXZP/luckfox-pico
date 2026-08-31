#define _POSIX_C_SOURCE 200809L

#include "uart_tool.h"

#include "led_tool.h"
#include "tool_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef B1000000
#define B1000000 0010010
#endif
#ifndef B1500000
#define B1500000 0010012
#endif
#ifndef B2000000
#define B2000000 0010013
#endif
#ifndef B2500000
#define B2500000 0010014
#endif
#ifndef B3000000
#define B3000000 0010015
#endif
#ifndef B3500000
#define B3500000 0010016
#endif
#ifndef B4000000
#define B4000000 0010017
#endif

size_t uart_tool_build_frame(uint16_t cmd, uint16_t seq, uint8_t flags,
                             const uint8_t *payload, uint16_t payload_len,
                             uint8_t *out, size_t out_cap)
{
    uint16_t crc;

    if (payload_len > UART_TOOL_MAX_PAYLOAD ||
        out_cap < (size_t)UART_TOOL_HEADER_LEN + payload_len + UART_TOOL_CRC_LEN)
        return 0;

    out[0] = UART_TOOL_SOF1;
    out[1] = UART_TOOL_SOF2;
    out[2] = UART_TOOL_VERSION;
    out[3] = flags;
    tool_put_le16(&out[4], cmd);
    tool_put_le16(&out[6], seq);
    tool_put_le16(&out[8], payload_len);
    if (payload_len != 0U && payload != NULL)
        memcpy(&out[UART_TOOL_HEADER_LEN], payload, payload_len);

    crc = tool_crc16_ccitt(0xffffU, &out[2], 8U + payload_len);
    tool_put_le16(&out[UART_TOOL_HEADER_LEN + payload_len], crc);
    return UART_TOOL_HEADER_LEN + payload_len + UART_TOOL_CRC_LEN;
}

int uart_tool_decode_frame_bytes(const uint8_t *data, size_t len,
                                 uart_tool_frame_t *frame)
{
    uint16_t payload_len;
    uint16_t rx_crc;
    uint16_t calc_crc;

    if (len < UART_TOOL_HEADER_LEN + UART_TOOL_CRC_LEN) {
        errno = EMSGSIZE;
        return -1;
    }
    if (data[0] != UART_TOOL_SOF1 || data[1] != UART_TOOL_SOF2) {
        errno = EBADMSG;
        return -1;
    }

    payload_len = tool_get_le16(&data[8]);
    if (payload_len > UART_TOOL_MAX_PAYLOAD ||
        len != (size_t)UART_TOOL_HEADER_LEN + payload_len + UART_TOOL_CRC_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    rx_crc = tool_get_le16(&data[UART_TOOL_HEADER_LEN + payload_len]);
    calc_crc = tool_crc16_ccitt(0xffffU, &data[2], 8U + payload_len);
    if (rx_crc != calc_crc) {
        errno = EBADMSG;
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = data[2];
    frame->flags = data[3];
    frame->cmd = tool_get_le16(&data[4]);
    frame->seq = tool_get_le16(&data[6]);
    frame->payload_len = payload_len;
    if (payload_len != 0U)
        memcpy(frame->payload, &data[UART_TOOL_HEADER_LEN], payload_len);

    return 0;
}

void uart_tool_parser_init(uart_tool_stream_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

int uart_tool_parser_feed(uart_tool_stream_parser_t *parser, uint8_t byte,
                          uart_tool_frame_t *frame)
{
    switch (parser->state) {
    case 0:
        if (byte == UART_TOOL_SOF1) {
            parser->buf[0] = byte;
            parser->len = 1;
            parser->state = 1;
        }
        return 0;
    case 1:
        if (byte == UART_TOOL_SOF2) {
            parser->buf[parser->len++] = byte;
            parser->state = 2;
        } else if (byte == UART_TOOL_SOF1) {
            parser->buf[0] = byte;
            parser->len = 1;
        } else {
            uart_tool_parser_init(parser);
        }
        return 0;
    default:
        parser->buf[parser->len++] = byte;
        if (parser->len == UART_TOOL_HEADER_LEN) {
            uint16_t payload_len = tool_get_le16(&parser->buf[8]);

            if (payload_len > UART_TOOL_MAX_PAYLOAD) {
                uart_tool_parser_init(parser);
                return -1;
            }
            parser->expected_len = UART_TOOL_HEADER_LEN + payload_len + UART_TOOL_CRC_LEN;
        }

        if (parser->expected_len != 0U && parser->len == parser->expected_len) {
            int ret = uart_tool_decode_frame_bytes(parser->buf, parser->len, frame);

            uart_tool_parser_init(parser);
            return ret == 0 ? 1 : -1;
        }

        if (parser->len >= sizeof(parser->buf)) {
            uart_tool_parser_init(parser);
            return -1;
        }
        return 0;
    }
}

const char *uart_tool_cmd_name(uint16_t cmd)
{
    switch ((uint16_t)(cmd & ~UART_TOOL_RSP_BIT)) {
    case UART_TOOL_CMD_PING:
        return "PING";
    case UART_TOOL_CMD_GET_VERSION:
        return "GET_VERSION";
    case LED_TOOL_CMD_ACTION_ENQUEUE:
        return "LED_ACTION_ENQUEUE";
    case LED_TOOL_CMD_ACTION_EVENT:
        return "LED_ACTION_EVENT";
    default:
        return "UNKNOWN";
    }
}

const char *uart_tool_status_text(int16_t status)
{
    switch (status) {
    case 0:
        return "ok";
    case -EINVAL:
        return "-EINVAL";
    case -ENOENT:
        return "-ENOENT";
    case -ENOBUFS:
        return "-ENOBUFS";
    case -ENODEV:
        return "-ENODEV";
#ifdef ECANCELED
    case -ECANCELED:
        return "-ECANCELED";
#endif
    default:
        return "unknown";
    }
}

static void print_uart_payload_detail(const uart_tool_frame_t *frame)
{
    int16_t status;
    bool is_response = (frame->cmd & UART_TOOL_RSP_BIT) != 0U;
    uint16_t base_cmd = (uint16_t)(frame->cmd & ~UART_TOOL_RSP_BIT);

    if (!is_response && base_cmd == LED_TOOL_CMD_ACTION_EVENT) {
        led_tool_action_event_t event;

        if (led_tool_parse_action_event(frame->payload, frame->payload_len, &event) == 0) {
            printf("led_event:\n");
            printf("  action_version: %u\n", event.version);
            printf("  request_id: %u\n", event.request_id);
            printf("  action_id: %u\n", event.action_id);
            printf("  event: %u (%s)\n",
                   event.event, led_tool_action_event_name(event.event));
            printf("  result: %d (%s)\n",
                   event.result, uart_tool_status_text(event.result));
            printf("  queue_used: %u\n", event.queue_used);
        }
        return;
    }

    if (!is_response || frame->payload_len < 2U)
        return;

    status = tool_get_i16_le(frame->payload);
    printf("status: %d (%s)\n", status, uart_tool_status_text(status));

    if (base_cmd == UART_TOOL_CMD_PING && frame->payload_len > 2U) {
        printf("ping_echo: ");
        tool_print_hex(&frame->payload[2], frame->payload_len - 2U);
        putchar('\n');
    } else if (base_cmd == UART_TOOL_CMD_GET_VERSION && frame->payload_len > 2U) {
        printf("version_string: ");
        for (uint16_t index = 2; index < frame->payload_len; index++) {
            uint8_t c = frame->payload[index];

            putchar((c >= 32U && c <= 126U) ? (char)c : '.');
        }
        putchar('\n');
    } else if (base_cmd == LED_TOOL_CMD_ACTION_ENQUEUE) {
        led_tool_enqueue_response_t led_rsp;

        if (led_tool_parse_enqueue_response(frame->payload, frame->payload_len, &led_rsp) == 0 &&
            led_rsp.has_detail) {
            printf("led_enqueue:\n");
            printf("  request_id: %u\n", led_rsp.request_id);
            printf("  accepted_count: %u\n", led_rsp.accepted_count);
            printf("  queue_used: %u\n", led_rsp.queue_used);
        }
    }
}

void uart_tool_print_frame(const uart_tool_frame_t *frame)
{
    printf("uart_frame:\n");
    printf("  version: 0x%02X\n", frame->version);
    printf("  flags: 0x%02X\n", frame->flags);
    printf("  cmd: 0x%04X%s (%s)\n",
           frame->cmd,
           (frame->cmd & UART_TOOL_RSP_BIT) != 0U ? " response" : "",
           uart_tool_cmd_name(frame->cmd));
    printf("  seq: %u\n", frame->seq);
    printf("  payload_len: %u\n", frame->payload_len);
    printf("  payload: ");
    tool_print_hex(frame->payload, frame->payload_len);
    putchar('\n');
    print_uart_payload_detail(frame);
}

static speed_t baud_to_speed(unsigned int baud)
{
    switch (baud) {
    case 9600U:
        return B9600;
    case 19200U:
        return B19200;
    case 38400U:
        return B38400;
    case 57600U:
        return B57600;
    case 115200U:
        return B115200;
    case 230400U:
        return B230400;
    case 460800U:
        return B460800;
    case 500000U:
        return B500000;
    case 576000U:
        return B576000;
    case 921600U:
        return B921600;
    case 1000000U:
        return B1000000;
    case 1500000U:
        return B1500000;
    case 2000000U:
        return B2000000;
    case 2500000U:
        return B2500000;
    case 3000000U:
        return B3000000;
    case 3500000U:
        return B3500000;
    case 4000000U:
        return B4000000;
    default:
        return 0;
    }
}

static int open_serial(const uart_tool_serial_config_t *cfg)
{
    struct termios tio;
    speed_t speed = baud_to_speed(cfg->baud);
    int fd;

    if (speed == 0) {
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
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_1ms();
            continue;
        }
        return -1;
    }

    return 0;
}

int uart_tool_serial_request(const uart_tool_serial_config_t *cfg, uint16_t cmd,
                             const uint8_t *payload, uint16_t payload_len,
                             uart_tool_frame_t *rsp)
{
    int fd;
    uint8_t tx[UART_TOOL_MAX_FRAME];
    size_t tx_len;
    long long deadline;
    uart_tool_stream_parser_t parser;

    tx_len = uart_tool_build_frame(cmd, cfg->seq, 0U, payload, payload_len, tx, sizeof(tx));
    if (tx_len == 0U) {
        errno = EINVAL;
        return -1;
    }

    fd = open_serial(cfg);
    if (fd < 0)
        return -1;

    if (cfg->verbose) {
        printf("tx: ");
        tool_print_hex(tx, tx_len);
        putchar('\n');
    }

    if (write_all(fd, tx, tx_len) < 0) {
        close(fd);
        return -1;
    }
    tcdrain(fd);

    uart_tool_parser_init(&parser);
    deadline = monotonic_ms() + cfg->timeout_ms;

    while (monotonic_ms() < deadline) {
        fd_set readfds;
        struct timeval timeout;
        uint8_t rx[64];
        ssize_t read_len;
        int ret;
        long long now = monotonic_ms();
        long long remain = deadline - now;

        if (remain < 0)
            remain = 0;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeout.tv_sec = (time_t)(remain / 1000LL);
        timeout.tv_usec = (suseconds_t)((remain % 1000LL) * 1000LL);

        ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (ret == 0)
            break;

        read_len = read(fd, rx, sizeof(rx));
        if (read_len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            close(fd);
            return -1;
        }

        for (ssize_t index = 0; index < read_len; index++) {
            ret = uart_tool_parser_feed(&parser, rx[index], rsp);
            if (ret != 1)
                continue;

            if (cfg->verbose) {
                printf("rx:\n");
                uart_tool_print_frame(rsp);
            }

            if (rsp->seq == cfg->seq && rsp->cmd == (cmd | UART_TOOL_RSP_BIT)) {
                close(fd);
                return 0;
            }

            if (cfg->verbose) {
                printf("rx_ignored: cmd=0x%04X seq=%u\n", rsp->cmd, rsp->seq);
            }
        }
    }

    close(fd);
    errno = ETIMEDOUT;
    return -1;
}
