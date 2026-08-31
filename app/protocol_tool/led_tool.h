#ifndef LED_TOOL_H_
#define LED_TOOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LED_TOOL_CMD_ACTION_ENQUEUE 0x0110U
#define LED_TOOL_CMD_ACTION_EVENT 0x0111U

#define LED_TOOL_ACTION_VERSION 1U
#define LED_TOOL_QUEUE_APPEND 0U
#define LED_TOOL_QUEUE_REPLACE_PENDING 1U
#define LED_TOOL_QUEUE_INTERRUPT_REPLACE 2U

#define LED_TOOL_ACTION_HOLD 0x01U
#define LED_TOOL_ACTION_BLINK 0x02U
#define LED_TOOL_MAX_TARGETS 2U

#define LED_TOOL_DEFAULT_PWM_NAME "ff490020"
#define LED_TOOL_DEFAULT_PWM_CHANNEL 0
#define LED_TOOL_DEFAULT_PWM_FREQUENCY_HZ 1000U
#define LED_TOOL_DEFAULT_HEADLIGHT_BRIGHTNESS 100U

#ifndef LED_TOOL_PATH_MAX
#define LED_TOOL_PATH_MAX 4096
#endif

typedef struct {
    uint8_t address;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_tool_rgb_target_t;

typedef struct {
    int16_t status;
    bool has_detail;
    uint16_t request_id;
    uint8_t accepted_count;
    uint8_t queue_used;
} led_tool_enqueue_response_t;

typedef struct {
    uint8_t version;
    uint16_t request_id;
    uint16_t action_id;
    uint8_t event;
    int16_t result;
    uint8_t queue_used;
} led_tool_action_event_t;

typedef struct {
    char chip_path[LED_TOOL_PATH_MAX];
    int channel;
    unsigned int frequency_hz;
    unsigned int brightness;
} led_tool_headlight_config_t;

typedef struct {
    char pwm_path[LED_TOOL_PATH_MAX];
    led_tool_headlight_config_t cfg;
    uint32_t period_ns;
    bool exported_by_us;
    bool configured;
} led_tool_headlight_t;

void led_tool_headlight_defaults(led_tool_headlight_config_t *cfg);
int led_tool_headlight_set_chip(led_tool_headlight_config_t *cfg, const char *chip);
int led_tool_headlight_init(led_tool_headlight_t *light,
                            const led_tool_headlight_config_t *cfg);
int led_tool_headlight_cleanup(led_tool_headlight_t *light);
int led_tool_headlight_set_brightness(led_tool_headlight_t *light,
                                      unsigned int percent);
int led_tool_headlight_fade(led_tool_headlight_t *light,
                            unsigned int start_percent,
                            unsigned int end_percent,
                            unsigned int duration_ms,
                            unsigned int step_ms);
int led_tool_headlight_blink(led_tool_headlight_t *light,
                             unsigned int brightness,
                             uint16_t repeat,
                             uint16_t on_ms,
                             uint16_t off_ms);
int led_tool_sleep_ms(unsigned int ms);

size_t led_tool_build_hold_enqueue(uint16_t request_id,
                                   uint16_t action_id,
                                   uint8_t queue_mode,
                                   uint32_t duration_ms,
                                   const led_tool_rgb_target_t *targets,
                                   uint8_t target_count,
                                   uint8_t *out,
                                   size_t out_cap);
size_t led_tool_build_blink_enqueue(uint16_t request_id,
                                    uint16_t action_id,
                                    uint8_t queue_mode,
                                    uint16_t repeat_count,
                                    uint16_t on_time_ms,
                                    uint16_t off_time_ms,
                                    const led_tool_rgb_target_t *targets,
                                    uint8_t target_count,
                                    uint8_t *out,
                                    size_t out_cap);

int led_tool_parse_enqueue_response(const uint8_t *payload,
                                    uint16_t payload_len,
                                    led_tool_enqueue_response_t *rsp);
int led_tool_parse_action_event(const uint8_t *payload,
                                uint16_t payload_len,
                                led_tool_action_event_t *event);

const char *led_tool_queue_mode_name(uint8_t mode);
const char *led_tool_action_event_name(uint8_t event);

#endif
