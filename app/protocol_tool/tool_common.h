#ifndef TOOL_COMMON_H_
#define TOOL_COMMON_H_

#include <stddef.h>
#include <stdint.h>

uint16_t tool_get_le16(const uint8_t *p);
uint32_t tool_get_le32(const uint8_t *p);
int16_t tool_get_i16_le(const uint8_t *p);
int32_t tool_get_i32_le(const uint8_t *p);
void tool_put_le16(uint8_t *p, uint16_t value);
void tool_put_le32(uint8_t *p, uint32_t value);

uint16_t tool_crc16_ccitt(uint16_t init, const uint8_t *data, size_t len);
void tool_print_hex(const uint8_t *data, size_t len);

int tool_parse_hex_bytes(int argc, char **argv, uint8_t *out, size_t out_cap,
                         size_t *out_len);
int tool_parse_u32_arg(const char *s, uint32_t min, uint32_t max, uint32_t *out);
int tool_parse_i32_arg(const char *s, int32_t min, int32_t max, int32_t *out);

#endif
