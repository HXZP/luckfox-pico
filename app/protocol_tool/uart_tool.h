#ifndef UART_TOOL_H_
#define UART_TOOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UART_TOOL_SOF1 0xA5U
#define UART_TOOL_SOF2 0x5AU
#define UART_TOOL_VERSION 0x01U
#define UART_TOOL_HEADER_LEN 10U
#define UART_TOOL_CRC_LEN 2U
#define UART_TOOL_MAX_PAYLOAD 128U
#define UART_TOOL_MAX_FRAME (UART_TOOL_HEADER_LEN + UART_TOOL_MAX_PAYLOAD + UART_TOOL_CRC_LEN)

#define UART_TOOL_CMD_PING 0x0001U
#define UART_TOOL_CMD_GET_VERSION 0x0002U
#define UART_TOOL_RSP_BIT 0x8000U

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint16_t cmd;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[UART_TOOL_MAX_PAYLOAD];
} uart_tool_frame_t;

typedef struct {
    int state;
    uint8_t buf[UART_TOOL_MAX_FRAME];
    size_t len;
    size_t expected_len;
} uart_tool_stream_parser_t;

typedef struct {
    const char *port;
    unsigned int baud;
    unsigned int timeout_ms;
    uint16_t seq;
    bool verbose;
} uart_tool_serial_config_t;

size_t uart_tool_build_frame(uint16_t cmd, uint16_t seq, uint8_t flags,
                             const uint8_t *payload, uint16_t payload_len,
                             uint8_t *out, size_t out_cap);
int uart_tool_decode_frame_bytes(const uint8_t *data, size_t len,
                                 uart_tool_frame_t *frame);
void uart_tool_parser_init(uart_tool_stream_parser_t *parser);
int uart_tool_parser_feed(uart_tool_stream_parser_t *parser, uint8_t byte,
                          uart_tool_frame_t *frame);

const char *uart_tool_cmd_name(uint16_t cmd);
const char *uart_tool_status_text(int16_t status);
void uart_tool_print_frame(const uart_tool_frame_t *frame);

int uart_tool_serial_request(const uart_tool_serial_config_t *cfg, uint16_t cmd,
                             const uint8_t *payload, uint16_t payload_len,
                             uart_tool_frame_t *rsp);

#endif
