#include "tool_common.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t tool_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t tool_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int16_t tool_get_i16_le(const uint8_t *p)
{
    return (int16_t)tool_get_le16(p);
}

int32_t tool_get_i32_le(const uint8_t *p)
{
    return (int32_t)tool_get_le32(p);
}

void tool_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)(value >> 8);
}

void tool_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)((value >> 8) & 0xffU);
    p[2] = (uint8_t)((value >> 16) & 0xffU);
    p[3] = (uint8_t)((value >> 24) & 0xffU);
}

uint16_t tool_crc16_ccitt(uint16_t init, const uint8_t *data, size_t len)
{
    uint16_t crc = init;

    for (size_t index = 0; index < len; index++) {
        crc ^= (uint16_t)data[index] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000U) != 0U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc = (uint16_t)(crc << 1);
        }
    }

    return crc;
}

void tool_print_hex(const uint8_t *data, size_t len)
{
    for (size_t index = 0; index < len; index++) {
        if (index != 0U)
            putchar(' ');
        printf("%02X", data[index]);
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

int tool_parse_hex_bytes(int argc, char **argv, uint8_t *out, size_t out_cap,
                         size_t *out_len)
{
    size_t len = 0;

    for (int arg_index = 0; arg_index < argc; arg_index++) {
        const char *s = argv[arg_index];
        int high_nibble = -1;

        if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
            s += 2;

        for (; *s != '\0'; s++) {
            int value;

            if (*s == ':' || *s == ',' || *s == '-' || *s == '_' || *s == ' ')
                continue;

            value = parse_hex_nibble(*s);
            if (value < 0)
                return -1;

            if (high_nibble < 0) {
                high_nibble = value;
            } else {
                if (len >= out_cap)
                    return -1;
                out[len++] = (uint8_t)((high_nibble << 4) | value);
                high_nibble = -1;
            }
        }

        if (high_nibble >= 0)
            return -1;
    }

    *out_len = len;
    return 0;
}

int tool_parse_u32_arg(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' ||
        value < (unsigned long)min || value > (unsigned long)max)
        return -1;

    *out = (uint32_t)value;
    return 0;
}

int tool_parse_i32_arg(const char *s, int32_t min, int32_t max, int32_t *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' ||
        value < (long)min || value > (long)max)
        return -1;

    *out = (int32_t)value;
    return 0;
}
