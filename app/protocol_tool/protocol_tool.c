#include "can_tool.h"
#include "led_tool.h"
#include "tool_common.h"
#include "uart_tool.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void serial_config_defaults(uart_tool_serial_config_t *cfg)
{
    cfg->port = "/dev/ttyS0";
    cfg->baud = 1000000U;
    cfg->timeout_ms = 1000U;
    cfg->seq = 1U;
    cfg->verbose = false;
}

static int parse_uart_request_options(int argc, char **argv,
                                      uart_tool_serial_config_t *cfg,
                                      uint8_t *payload,
                                      size_t payload_cap,
                                      size_t *payload_len)
{
    *payload_len = 0;

    for (int arg_index = 0; arg_index < argc; arg_index++) {
        uint32_t value;

        if (strcmp(argv[arg_index], "--port") == 0) {
            if (++arg_index >= argc) {
                fprintf(stderr, "--port requires value\n");
                return -1;
            }
            cfg->port = argv[arg_index];
        } else if (strcmp(argv[arg_index], "--baud") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 4000000U, &value) < 0) {
                fprintf(stderr, "--baud requires integer\n");
                return -1;
            }
            cfg->baud = value;
        } else if (strcmp(argv[arg_index], "--timeout-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 60000U, &value) < 0) {
                fprintf(stderr, "--timeout-ms requires integer\n");
                return -1;
            }
            cfg->timeout_ms = value;
        } else if (strcmp(argv[arg_index], "--seq") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 0xffffU, &value) < 0) {
                fprintf(stderr, "--seq requires 0..65535\n");
                return -1;
            }
            cfg->seq = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(argv[arg_index], "--payload") == 0) {
            arg_index++;
            if (arg_index >= argc ||
                tool_parse_hex_bytes(argc - arg_index, &argv[arg_index],
                                     payload, payload_cap, payload_len) < 0) {
                fprintf(stderr, "--payload requires hex bytes\n");
                return -1;
            }
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[arg_index]);
            return -1;
        }
    }

    return 0;
}

static int run_uart_request(int argc, char **argv, uint16_t cmd)
{
    uart_tool_serial_config_t cfg;
    uart_tool_frame_t rsp;
    uint8_t payload[UART_TOOL_MAX_PAYLOAD];
    size_t payload_len = 0;

    serial_config_defaults(&cfg);
    if (parse_uart_request_options(argc, argv, &cfg, payload, sizeof(payload), &payload_len) < 0)
        return 2;

    if (uart_tool_serial_request(&cfg, cmd, payload, (uint16_t)payload_len, &rsp) < 0) {
        fprintf(stderr, "UART request failed on %s: %s\n", cfg.port, strerror(errno));
        return 1;
    }

    if (!cfg.verbose)
        uart_tool_print_frame(&rsp);
    return 0;
}

typedef struct {
    uart_tool_serial_config_t serial;
    led_tool_headlight_config_t headlight;
    bool use_headlight;
    bool use_tail;
    uint8_t queue_mode;
    uint16_t request_id;
    uint16_t action_id;
    unsigned int fade_ms;
    unsigned int dark_ms;
    unsigned int fade_step_ms;
    uint16_t blink_repeat;
    uint16_t blink_on_ms;
    uint16_t blink_off_ms;
    uint8_t tail_red;
    uint8_t tail_green;
    uint8_t tail_blue;
} led_boot_config_t;

static void led_boot_defaults(led_boot_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    serial_config_defaults(&cfg->serial);
    cfg->serial.timeout_ms = 1500U;
    cfg->serial.seq = 0x0110U;
    led_tool_headlight_defaults(&cfg->headlight);
    cfg->use_headlight = true;
    cfg->use_tail = true;
    cfg->queue_mode = LED_TOOL_QUEUE_INTERRUPT_REPLACE;
    cfg->request_id = 0xB001U;
    cfg->action_id = 1U;
    cfg->fade_ms = 1000U;
    cfg->dark_ms = 300U;
    cfg->fade_step_ms = 10U;
    cfg->blink_repeat = 5U;
    cfg->blink_on_ms = 50U;
    cfg->blink_off_ms = 50U;
    cfg->tail_red = 255U;
    cfg->tail_green = 220U;
    cfg->tail_blue = 140U;
}

static int parse_led_boot_options(int argc, char **argv, led_boot_config_t *cfg)
{
    for (int arg_index = 0; arg_index < argc; arg_index++) {
        uint32_t value;

        if (strcmp(argv[arg_index], "--port") == 0) {
            if (++arg_index >= argc) {
                fprintf(stderr, "--port requires value\n");
                return -1;
            }
            cfg->serial.port = argv[arg_index];
        } else if (strcmp(argv[arg_index], "--baud") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 4000000U, &value) < 0) {
                fprintf(stderr, "--baud requires integer\n");
                return -1;
            }
            cfg->serial.baud = value;
        } else if (strcmp(argv[arg_index], "--timeout-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 60000U, &value) < 0) {
                fprintf(stderr, "--timeout-ms requires integer\n");
                return -1;
            }
            cfg->serial.timeout_ms = value;
        } else if (strcmp(argv[arg_index], "--seq") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 0xffffU, &value) < 0) {
                fprintf(stderr, "--seq requires 0..65535\n");
                return -1;
            }
            cfg->serial.seq = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--request-id") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 0xffffU, &value) < 0) {
                fprintf(stderr, "--request-id requires 0..65535\n");
                return -1;
            }
            cfg->request_id = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--action-id") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 0xffffU, &value) < 0) {
                fprintf(stderr, "--action-id requires 0..65535\n");
                return -1;
            }
            cfg->action_id = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--queue-mode") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 2U, &value) < 0) {
                fprintf(stderr, "--queue-mode requires 0..2\n");
                return -1;
            }
            cfg->queue_mode = (uint8_t)value;
        } else if (strcmp(argv[arg_index], "--chip") == 0) {
            if (++arg_index >= argc ||
                led_tool_headlight_set_chip(&cfg->headlight, argv[arg_index]) < 0) {
                fprintf(stderr, "--chip requires pwmchipN, N, sysfs path, or device name\n");
                return -1;
            }
        } else if (strcmp(argv[arg_index], "--channel") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 32U, &value) < 0) {
                fprintf(stderr, "--channel requires 0..32\n");
                return -1;
            }
            cfg->headlight.channel = (int)value;
        } else if (strcmp(argv[arg_index], "--freq") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 1000000U, &value) < 0) {
                fprintf(stderr, "--freq requires integer\n");
                return -1;
            }
            cfg->headlight.frequency_hz = value;
        } else if (strcmp(argv[arg_index], "--brightness") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 100U, &value) < 0) {
                fprintf(stderr, "--brightness requires 0..100\n");
                return -1;
            }
            cfg->headlight.brightness = value;
        } else if (strcmp(argv[arg_index], "--fade-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 60000U, &value) < 0) {
                fprintf(stderr, "--fade-ms requires 0..60000\n");
                return -1;
            }
            cfg->fade_ms = value;
        } else if (strcmp(argv[arg_index], "--dark-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 0U, 60000U, &value) < 0) {
                fprintf(stderr, "--dark-ms requires 0..60000\n");
                return -1;
            }
            cfg->dark_ms = value;
        } else if (strcmp(argv[arg_index], "--fade-step-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 1000U, &value) < 0) {
                fprintf(stderr, "--fade-step-ms requires 1..1000\n");
                return -1;
            }
            cfg->fade_step_ms = value;
        } else if (strcmp(argv[arg_index], "--blink-repeat") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 1U, 10000U, &value) < 0) {
                fprintf(stderr, "--blink-repeat requires 1..10000\n");
                return -1;
            }
            cfg->blink_repeat = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--blink-on-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 10U, 60000U, &value) < 0) {
                fprintf(stderr, "--blink-on-ms requires 10..60000\n");
                return -1;
            }
            cfg->blink_on_ms = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--blink-off-ms") == 0) {
            if (++arg_index >= argc ||
                tool_parse_u32_arg(argv[arg_index], 10U, 60000U, &value) < 0) {
                fprintf(stderr, "--blink-off-ms requires 10..60000\n");
                return -1;
            }
            cfg->blink_off_ms = (uint16_t)value;
        } else if (strcmp(argv[arg_index], "--tail-rgb") == 0) {
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (arg_index + 3 >= argc ||
                tool_parse_u32_arg(argv[arg_index + 1], 0U, 255U, &red) < 0 ||
                tool_parse_u32_arg(argv[arg_index + 2], 0U, 255U, &green) < 0 ||
                tool_parse_u32_arg(argv[arg_index + 3], 0U, 255U, &blue) < 0) {
                fprintf(stderr, "--tail-rgb requires <red> <green> <blue>\n");
                return -1;
            }
            cfg->tail_red = (uint8_t)red;
            cfg->tail_green = (uint8_t)green;
            cfg->tail_blue = (uint8_t)blue;
            arg_index += 3;
        } else if (strcmp(argv[arg_index], "--no-headlight") == 0) {
            cfg->use_headlight = false;
        } else if (strcmp(argv[arg_index], "--no-tail") == 0) {
            cfg->use_tail = false;
        } else if (strcmp(argv[arg_index], "--verbose") == 0) {
            cfg->serial.verbose = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[arg_index]);
            return -1;
        }
    }

    if (!cfg->use_headlight && !cfg->use_tail) {
        fprintf(stderr, "led boot needs headlight, tail, or both\n");
        return -1;
    }

    return 0;
}

static void set_tail_targets(led_tool_rgb_target_t targets[LED_TOOL_MAX_TARGETS],
                             uint8_t red, uint8_t green, uint8_t blue)
{
    targets[0].address = 0U;
    targets[0].red = red;
    targets[0].green = green;
    targets[0].blue = blue;
    targets[1].address = 1U;
    targets[1].red = red;
    targets[1].green = green;
    targets[1].blue = blue;
}

static int send_led_enqueue(const uart_tool_serial_config_t *base_cfg,
                            uint16_t seq,
                            const uint8_t *payload,
                            size_t payload_len,
                            const char *label)
{
    uart_tool_serial_config_t cfg = *base_cfg;
    uart_tool_frame_t rsp;
    led_tool_enqueue_response_t led_rsp;

    if (payload_len > UART_TOOL_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }

    cfg.seq = seq;
    if (uart_tool_serial_request(&cfg, LED_TOOL_CMD_ACTION_ENQUEUE,
                                 payload, (uint16_t)payload_len, &rsp) < 0) {
        fprintf(stderr, "led: %s request failed on %s: %s\n",
                label, cfg.port, strerror(errno));
        return -1;
    }

    if (led_tool_parse_enqueue_response(rsp.payload, rsp.payload_len, &led_rsp) < 0) {
        fprintf(stderr, "led: %s invalid response payload\n", label);
        return -1;
    }

    if (led_rsp.status != 0) {
        fprintf(stderr, "led: %s rejected: status %d (%s)\n",
                label, led_rsp.status, uart_tool_status_text(led_rsp.status));
        errno = EIO;
        return -1;
    }

    if (cfg.verbose && led_rsp.has_detail) {
        printf("led_%s: request_id=%u accepted=%u queue_used=%u\n",
               label, led_rsp.request_id, led_rsp.accepted_count, led_rsp.queue_used);
    }

    return 0;
}

static int cmd_led_boot(int argc, char **argv)
{
    led_boot_config_t cfg;
    led_tool_headlight_t light;
    led_tool_rgb_target_t tail_targets[LED_TOOL_MAX_TARGETS];
    uint8_t payload[UART_TOOL_MAX_PAYLOAD];
    size_t payload_len;
    bool light_ready = false;
    int rc = 0;

    led_boot_defaults(&cfg);
    if (parse_led_boot_options(argc, argv, &cfg) < 0)
        return 2;

    memset(&light, 0, sizeof(light));

    if (cfg.serial.verbose) {
        printf("led_boot: fade=%ums dark=%ums blink=%ux%u/%ums tail_rgb=%u,%u,%u queue=%s\n",
               cfg.fade_ms, cfg.dark_ms, (unsigned int)cfg.blink_repeat,
               (unsigned int)cfg.blink_on_ms, (unsigned int)cfg.blink_off_ms,
               (unsigned int)cfg.tail_red, (unsigned int)cfg.tail_green,
               (unsigned int)cfg.tail_blue,
               led_tool_queue_mode_name(cfg.queue_mode));
    }

    if (cfg.use_tail) {
        set_tail_targets(tail_targets, 0U, 0U, 0U);
        payload_len = led_tool_build_hold_enqueue(cfg.request_id, cfg.action_id,
                                                  cfg.queue_mode, 0U,
                                                  tail_targets, LED_TOOL_MAX_TARGETS,
                                                  payload, sizeof(payload));
        if (payload_len == 0U)
            return 1;
        if (send_led_enqueue(&cfg.serial, cfg.serial.seq, payload, payload_len,
                             "tail_off") < 0)
            return 1;
    }

    if (cfg.use_headlight) {
        if (led_tool_headlight_init(&light, &cfg.headlight) < 0) {
            fprintf(stderr, "headlight: failed to init PWM %s channel %d: %s\n",
                    cfg.headlight.chip_path, cfg.headlight.channel, strerror(errno));
            return 1;
        }
        light_ready = true;

        if (cfg.serial.verbose) {
            printf("headlight: chip=%s channel=%d freq=%u brightness=%u\n",
                   cfg.headlight.chip_path, cfg.headlight.channel,
                   cfg.headlight.frequency_hz, cfg.headlight.brightness);
        }

        if (led_tool_headlight_fade(&light, 0U, cfg.headlight.brightness,
                                    cfg.fade_ms, cfg.fade_step_ms) < 0 ||
            led_tool_headlight_set_brightness(&light, 0U) < 0 ||
            led_tool_sleep_ms(cfg.dark_ms) < 0) {
            fprintf(stderr, "headlight: fade/dark stage failed: %s\n", strerror(errno));
            rc = -1;
            goto out;
        }
    } else if (cfg.fade_ms + cfg.dark_ms > 0U) {
        if (led_tool_sleep_ms(cfg.fade_ms + cfg.dark_ms) < 0) {
            rc = -1;
            goto out;
        }
    }

    if (cfg.use_tail) {
        set_tail_targets(tail_targets, cfg.tail_red, cfg.tail_green, cfg.tail_blue);
        payload_len = led_tool_build_blink_enqueue((uint16_t)(cfg.request_id + 1U),
                                                   (uint16_t)(cfg.action_id + 1U),
                                                   cfg.queue_mode,
                                                   cfg.blink_repeat,
                                                   cfg.blink_on_ms,
                                                   cfg.blink_off_ms,
                                                   tail_targets, LED_TOOL_MAX_TARGETS,
                                                   payload, sizeof(payload));
        if (payload_len == 0U ||
            send_led_enqueue(&cfg.serial, (uint16_t)(cfg.serial.seq + 1U),
                             payload, payload_len, "tail_blink") < 0) {
            rc = -1;
            goto out;
        }
    }

    if (cfg.use_headlight) {
        if (led_tool_headlight_blink(&light, cfg.headlight.brightness,
                                     cfg.blink_repeat, cfg.blink_on_ms,
                                     cfg.blink_off_ms) < 0) {
            fprintf(stderr, "headlight: blink stage failed: %s\n", strerror(errno));
            rc = -1;
            goto out;
        }
    } else if (cfg.use_tail) {
        unsigned int wait_ms = (unsigned int)cfg.blink_repeat *
                               (unsigned int)(cfg.blink_on_ms + cfg.blink_off_ms);

        if (led_tool_sleep_ms(wait_ms) < 0)
            rc = -1;
    }

out:
    if (light_ready && led_tool_headlight_cleanup(&light) < 0 && rc == 0) {
        fprintf(stderr, "headlight: cleanup failed: %s\n", strerror(errno));
        rc = -1;
    }

    if (rc == 0)
        printf("led_boot: ok\n");

    return rc == 0 ? 0 : 1;
}

static int cmd_uart_decode(int argc, char **argv)
{
    uint8_t data[UART_TOOL_MAX_FRAME];
    size_t len = 0;
    uart_tool_frame_t frame;

    if (argc < 1 || tool_parse_hex_bytes(argc, argv, data, sizeof(data), &len) < 0) {
        fprintf(stderr, "invalid UART frame hex\n");
        return 2;
    }

    if (uart_tool_decode_frame_bytes(data, len, &frame) < 0) {
        fprintf(stderr, "decode failed: %s\n", strerror(errno));
        return 1;
    }

    uart_tool_print_frame(&frame);
    return 0;
}

static int cmd_uart_build(int argc, char **argv)
{
    uint32_t cmd;
    uint32_t seq;
    uint8_t payload[UART_TOOL_MAX_PAYLOAD];
    uint8_t frame[UART_TOOL_MAX_FRAME];
    size_t payload_len = 0;
    size_t frame_len;

    if (argc < 2 ||
        tool_parse_u32_arg(argv[0], 0U, 0xffffU, &cmd) < 0 ||
        tool_parse_u32_arg(argv[1], 0U, 0xffffU, &seq) < 0) {
        fprintf(stderr, "usage: protocol_tool uart build <cmd> <seq> [payload-hex...]\n");
        return 2;
    }

    if (argc > 2 &&
        tool_parse_hex_bytes(argc - 2, &argv[2], payload, sizeof(payload), &payload_len) < 0) {
        fprintf(stderr, "invalid payload hex\n");
        return 2;
    }

    frame_len = uart_tool_build_frame((uint16_t)cmd, (uint16_t)seq, 0U,
                                      payload, (uint16_t)payload_len,
                                      frame, sizeof(frame));
    if (frame_len == 0U)
        return 1;

    tool_print_hex(frame, frame_len);
    putchar('\n');
    return 0;
}

static int cmd_uart_request(int argc, char **argv)
{
    uint32_t cmd;

    if (argc < 1 || tool_parse_u32_arg(argv[0], 0U, 0x7fffU, &cmd) < 0) {
        fprintf(stderr, "usage: protocol_tool uart request <cmd> [options] [--payload hex...]\n");
        return 2;
    }

    return run_uart_request(argc - 1, &argv[1], (uint16_t)cmd);
}

static int cmd_uart_ping(int argc, char **argv)
{
    return run_uart_request(argc, argv, UART_TOOL_CMD_PING);
}

static int cmd_uart_version(int argc, char **argv)
{
    return run_uart_request(argc, argv, UART_TOOL_CMD_GET_VERSION);
}

static int cmd_can_decode(int argc, char **argv)
{
    uint32_t id;
    uint8_t data[CAN_TOOL_DLC];
    size_t len = 0;

    if (argc < 2 ||
        tool_parse_u32_arg(argv[0], 0U, 0x7ffU, &id) < 0 ||
        tool_parse_hex_bytes(argc - 1, &argv[1], data, sizeof(data), &len) < 0 ||
        len != CAN_TOOL_DLC) {
        fprintf(stderr, "usage: protocol_tool can decode <std-id> <8-byte-data-hex...>\n");
        return 2;
    }

    can_tool_print_frame((uint16_t)id, data);
    return 0;
}

static int cmd_can_build(int argc, char **argv)
{
    uint32_t id;
    uint32_t cmd;
    uint8_t data[CAN_TOOL_DLC];

    if (argc < 2 ||
        tool_parse_u32_arg(argv[0], 0U, 0x7ffU, &id) < 0 ||
        tool_parse_u32_arg(argv[1], 0U, 0xffU, &cmd) < 0) {
        fprintf(stderr, "usage: protocol_tool can build <std-id> <cmd> [args...]\n");
        return 2;
    }

    if (can_tool_build_data((uint16_t)id, (uint8_t)cmd, argc - 2, &argv[2], data) < 0)
        return 2;

    printf("id: 0x%03X\n", id);
    printf("data: ");
    tool_print_hex(data, sizeof(data));
    putchar('\n');
    can_tool_decode_payload((uint16_t)id, data);
    return 0;
}

static int cmd_selftest(void)
{
    uint8_t payload[] = {0x11U, 0x22U, 0x33U};
    uint8_t frame[UART_TOOL_MAX_FRAME];
    size_t frame_len;
    uart_tool_frame_t decoded;
    uart_tool_stream_parser_t parser;
    uint8_t rsp_payload[2U + sizeof(payload)];
    uint8_t rsp[UART_TOOL_MAX_FRAME];
    size_t rsp_len;
    uint8_t can_data[CAN_TOOL_DLC];
    led_tool_rgb_target_t led_targets[LED_TOOL_MAX_TARGETS];
    uint8_t led_payload[UART_TOOL_MAX_PAYLOAD];
    size_t led_payload_len;
    uint8_t led_rsp_payload[6U];
    led_tool_enqueue_response_t led_rsp;

    frame_len = uart_tool_build_frame(UART_TOOL_CMD_PING, 1U, 0U,
                                      payload, sizeof(payload), frame, sizeof(frame));
    if (frame_len == 0U || uart_tool_decode_frame_bytes(frame, frame_len, &decoded) < 0)
        return 1;
    if (decoded.cmd != UART_TOOL_CMD_PING || decoded.seq != 1U ||
        decoded.payload_len != sizeof(payload) ||
        memcmp(decoded.payload, payload, sizeof(payload)) != 0)
        return 1;

    uart_tool_parser_init(&parser);
    for (size_t index = 0; index < frame_len; index++) {
        int ret = uart_tool_parser_feed(&parser, frame[index], &decoded);

        if (index + 1U < frame_len && ret != 0)
            return 1;
        if (index + 1U == frame_len && ret != 1)
            return 1;
    }

    tool_put_le16(rsp_payload, 0U);
    memcpy(&rsp_payload[2], payload, sizeof(payload));
    rsp_len = uart_tool_build_frame(UART_TOOL_CMD_PING | UART_TOOL_RSP_BIT, 1U, 0U,
                                    rsp_payload, sizeof(rsp_payload), rsp, sizeof(rsp));
    if (rsp_len == 0U ||
        uart_tool_decode_frame_bytes(rsp, rsp_len, &decoded) < 0 ||
        decoded.cmd != (UART_TOOL_CMD_PING | UART_TOOL_RSP_BIT) ||
        tool_get_i16_le(decoded.payload) != 0)
        return 1;

    if (can_tool_build_data(0x102U, 0x03U, 1, (char *[]){"12000"}, can_data) < 0 ||
        can_data[0] != 0x03U || tool_get_i32_le(&can_data[1]) != 12000)
        return 1;

    set_tail_targets(led_targets, 255U, 220U, 140U);
    led_payload_len = led_tool_build_blink_enqueue(0xB002U, 2U,
                                                   LED_TOOL_QUEUE_INTERRUPT_REPLACE,
                                                   5U, 50U, 50U,
                                                   led_targets, LED_TOOL_MAX_TARGETS,
                                                   led_payload, sizeof(led_payload));
    if (led_payload_len != 24U || led_payload[0] != LED_TOOL_ACTION_VERSION ||
        led_payload[3] != LED_TOOL_QUEUE_INTERRUPT_REPLACE || led_payload[4] != 1U ||
        led_payload[5] != 18U || led_payload[8] != LED_TOOL_ACTION_BLINK)
        return 1;

    tool_put_le16(led_rsp_payload, 0U);
    tool_put_le16(&led_rsp_payload[2], 0xB002U);
    led_rsp_payload[4] = 1U;
    led_rsp_payload[5] = 0U;
    if (led_tool_parse_enqueue_response(led_rsp_payload, sizeof(led_rsp_payload), &led_rsp) < 0 ||
        led_rsp.status != 0 || !led_rsp.has_detail || led_rsp.request_id != 0xB002U ||
        led_rsp.accepted_count != 1U || led_rsp.queue_used != 0U)
        return 1;

    printf("selftest: ok\n");
    printf("sample_uart_ping_req: ");
    tool_print_hex(frame, frame_len);
    putchar('\n');
    printf("sample_uart_ping_rsp: ");
    tool_print_hex(rsp, rsp_len);
    putchar('\n');
    printf("sample_can_report_decode:\n");
    {
        const uint8_t report[CAN_TOOL_DLC] = {0x10U, 0x27U, 0x00U, 0x00U,
                                              0x20U, 0x4eU, 0x00U, 0x00U};
        can_tool_print_frame(0x202U, report);
    }
    printf("sample_led_tail_blink_payload: ");
    tool_print_hex(led_payload, led_payload_len);
    putchar('\n');
    return 0;
}

static void print_usage(const char *argv0)
{
    printf(
        "Usage:\n"
        "  %s selftest\n"
        "  %s uart build <cmd> <seq> [payload-hex...]\n"
        "  %s uart decode <frame-hex...>\n"
        "  %s uart request <cmd> [--port /dev/ttyS0] [--baud 1000000] [--seq n] [--timeout-ms n] [--verbose] [--payload hex...]\n"
        "  %s uart ping [--port /dev/ttyS0] [--baud 1000000] [--seq n] [--timeout-ms n] [--verbose] [--payload hex...]\n"
        "  %s uart version [--port /dev/ttyS0] [--baud 1000000] [--seq n] [--timeout-ms n] [--verbose]\n"
        "  %s led boot [--port /dev/ttyS0] [--baud 1000000] [--chip ff490020] [--brightness 100] [--tail-rgb 255 220 140]\n"
        "  %s can build <std-id> <cmd> [args...]\n"
        "  %s can decode <std-id> <8-byte-data-hex...>\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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
        if (strcmp(argv[2], "request") == 0)
            return cmd_uart_request(argc - 3, &argv[3]);
        if (strcmp(argv[2], "ping") == 0)
            return cmd_uart_ping(argc - 3, &argv[3]);
        if (strcmp(argv[2], "version") == 0)
            return cmd_uart_version(argc - 3, &argv[3]);
    } else if (strcmp(argv[1], "led") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[2], "boot") == 0)
            return cmd_led_boot(argc - 3, &argv[3]);
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
