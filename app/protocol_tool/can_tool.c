#include "can_tool.h"

#include "tool_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *can_status_text(uint8_t status)
{
    switch (status) {
    case 0x00U:
        return "ok";
    case 0x01U:
        return "invalid command";
    case 0x02U:
        return "invalid parameter";
    case 0x03U:
        return "invalid mode";
    case 0x04U:
        return "CAN error";
    default:
        return "unknown";
    }
}

static const char *can_cmd_text(uint8_t cmd)
{
    switch (cmd) {
    case 0x01U:
        return "set_mode";
    case 0x02U:
        return "set_current_target";
    case 0x03U:
        return "set_velocity_target";
    case 0x04U:
        return "set_position_target";
    case 0x10U:
        return "set_velocity_pid";
    case 0x11U:
        return "read_velocity_pid";
    case 0x12U:
        return "set_position_pid";
    case 0x13U:
        return "read_position_pid";
    case 0x20U:
        return "set_node_id";
    case 0x21U:
        return "read_node_id";
    case 0x22U:
        return "read_app_version";
    case 0x23U:
        return "set_report_config";
    case 0x24U:
        return "read_report_config";
    case 0x25U:
        return "reset_can_config";
    case 0x26U:
        return "set_foc_config";
    case 0x27U:
        return "read_foc_config";
    case 0x30U:
        return "calibrate_zero";
    case 0x31U:
        return "read_zero";
    case 0x40U:
        return "enter_boot_ota";
    case 0x60U:
        return "management_discovery";
    case 0x61U:
        return "identify";
    case 0x62U:
        return "configure_node_id";
    default:
        return "unknown";
    }
}

static const char *pid_param_text(uint8_t param)
{
    switch (param) {
    case 0x00U:
        return "P";
    case 0x01U:
        return "I";
    case 0x02U:
        return "D";
    case 0x03U:
        return "I_ACC_MAX";
    case 0x04U:
        return "OUT_MAX";
    default:
        return "unknown";
    }
}

static const char *foc_param_text(uint8_t param)
{
    switch (param) {
    case 0x00U:
        return "pole_pairs";
    case 0x01U:
        return "master_voltage_mv";
    case 0x02U:
        return "control_hz";
    case 0x03U:
        return "sensor_hz";
    case 0x04U:
        return "phase_map";
    default:
        return "unknown";
    }
}

void can_tool_print_id_class(uint16_t id)
{
    if (id == 0x17eU)
        printf("frame_class: management command, node_id=0x7E\n");
    else if (id == 0x1feU)
        printf("frame_class: management response, node_id=0x7E\n");
    else if (id == 0x27eU)
        printf("frame_class: management report, node_id=0x7E\n");
    else if (id >= 0x101U && id <= 0x17fU)
        printf("frame_class: host command, node_id=0x%02X\n", id - 0x100U);
    else if (id >= 0x181U && id <= 0x1ffU)
        printf("frame_class: motor response, node_id=0x%02X\n", id - 0x180U);
    else if (id >= 0x201U && id <= 0x27fU)
        printf("frame_class: motor report, node_id=0x%02X\n", id - 0x200U);
    else if (id >= 0x401U && id <= 0x47fU)
        printf("frame_class: ota control, node_id=0x%02X\n", id - 0x400U);
    else if (id >= 0x501U && id <= 0x57fU)
        printf("frame_class: ota response, node_id=0x%02X\n", id - 0x500U);
    else
        printf("frame_class: unknown\n");
}

static bool can_is_response_id(uint16_t id)
{
    return id == 0x1feU || (id >= 0x181U && id <= 0x1ffU) ||
           (id >= 0x501U && id <= 0x57fU);
}

static bool can_is_motor_report_id(uint16_t id)
{
    return id >= 0x201U && id <= 0x27fU && id != 0x27eU;
}

static void decode_can_request_payload(uint8_t cmd, const uint8_t data[CAN_TOOL_DLC])
{
    switch (cmd) {
    case 0x01U:
        printf("enable: %u\n", data[1]);
        printf("control_mode: %u (%s)\n",
               data[2],
               data[2] == 0U ? "current" :
               data[2] == 1U ? "velocity" :
               data[2] == 2U ? "position" : "unknown");
        break;
    case 0x02U:
        printf("current_target: %d\n", tool_get_i32_le(&data[1]));
        break;
    case 0x03U:
        printf("velocity_target_mrad_s: %d\n", tool_get_i32_le(&data[1]));
        break;
    case 0x04U:
        printf("position_target_mrad: %d\n", tool_get_i32_le(&data[1]));
        break;
    case 0x10U:
    case 0x12U:
        printf("pid_param: 0x%02X (%s)\n", data[1], pid_param_text(data[1]));
        printf("pid_value_raw: %d\n", tool_get_i32_le(&data[2]));
        if (data[1] <= 0x02U)
            printf("pid_value_float: %.3f\n", tool_get_i32_le(&data[2]) / 1000.0);
        break;
    case 0x11U:
    case 0x13U:
        printf("pid_param: 0x%02X (%s)\n", data[1], pid_param_text(data[1]));
        break;
    case 0x20U:
        printf("node_id: 0x%02X\n", data[1]);
        break;
    case 0x23U:
        printf("report_enable: %u\n", data[1]);
        printf("report_period_ms: %u\n", tool_get_le16(&data[2]));
        break;
    case 0x25U:
        printf("confirm_code: 0x%02X\n", data[1]);
        break;
    case 0x26U:
        printf("foc_param: 0x%02X (%s)\n", data[1], foc_param_text(data[1]));
        printf("foc_value: %u\n", tool_get_le32(&data[2]));
        break;
    case 0x27U:
        printf("foc_param: 0x%02X (%s)\n", data[1], foc_param_text(data[1]));
        break;
    case 0x60U:
        printf("configured: %u\n", data[1]);
        printf("node_id: 0x%02X\n", data[2]);
        printf("uid32: 0x%08X\n", tool_get_le32(&data[3]));
        break;
    case 0x61U:
        printf("blink_hz: %u\n", data[1]);
        printf("duration_s: %u\n", data[2]);
        printf("uid32: 0x%08X\n", tool_get_le32(&data[3]));
        break;
    case 0x62U:
        printf("new_node_id: 0x%02X\n", data[1]);
        printf("uid32: 0x%08X\n", tool_get_le32(&data[3]));
        break;
    default:
        break;
    }
}

static void decode_can_response_payload(uint8_t cmd, const uint8_t data[CAN_TOOL_DLC])
{
    printf("status: 0x%02X (%s)\n", data[1], can_status_text(data[1]));

    switch (cmd) {
    case 0x11U:
    case 0x13U:
        printf("pid_param: 0x%02X (%s)\n", data[2], pid_param_text(data[2]));
        printf("pid_value_raw: %d\n", tool_get_i32_le(&data[3]));
        if (data[2] <= 0x02U)
            printf("pid_value_float: %.3f\n", tool_get_i32_le(&data[3]) / 1000.0);
        break;
    case 0x20U:
    case 0x21U:
        printf("node_id: 0x%02X\n", data[2]);
        break;
    case 0x22U:
        printf("app_version: %u.%u.%u\n", data[2], data[3], data[4]);
        break;
    case 0x23U:
    case 0x24U:
        printf("report_enable: %u\n", data[2]);
        printf("report_period_ms: %u\n", tool_get_le16(&data[3]));
        break;
    case 0x26U:
    case 0x27U:
        printf("foc_param: 0x%02X (%s)\n", data[2], foc_param_text(data[2]));
        printf("foc_value: %u\n", tool_get_le32(&data[3]));
        break;
    case 0x31U:
        printf("zero_mrad: %d\n", tool_get_i32_le(&data[2]));
        break;
    case 0x61U:
        printf("actual_hz: %u\n", data[2]);
        printf("actual_duration_s: %u\n", data[3]);
        printf("uid32: 0x%08X\n", tool_get_le32(&data[4]));
        break;
    case 0x62U:
        printf("new_node_id: 0x%02X\n", data[2]);
        printf("uid32: 0x%08X\n", tool_get_le32(&data[3]));
        break;
    default:
        break;
    }
}

void can_tool_decode_payload(uint16_t id, const uint8_t data[CAN_TOOL_DLC])
{
    if (can_is_motor_report_id(id)) {
        printf("report_position_mrad: %d\n", tool_get_i32_le(&data[0]));
        printf("report_velocity_mrad_s: %d\n", tool_get_i32_le(&data[4]));
        return;
    }

    printf("cmd: 0x%02X (%s)\n", data[0], can_cmd_text(data[0]));
    if (can_is_response_id(id))
        decode_can_response_payload(data[0], data);
    else
        decode_can_request_payload(data[0], data);
}

void can_tool_print_frame(uint16_t id, const uint8_t data[CAN_TOOL_DLC])
{
    printf("can_frame:\n");
    printf("id: 0x%03X\n", id);
    can_tool_print_id_class(id);
    printf("data: ");
    tool_print_hex(data, CAN_TOOL_DLC);
    putchar('\n');
    can_tool_decode_payload(id, data);
}

int can_tool_build_data(uint16_t id, uint8_t cmd, int argc, char **argv,
                        uint8_t data[CAN_TOOL_DLC])
{
    (void)id;
    memset(data, 0, CAN_TOOL_DLC);
    data[0] = cmd;

    if (cmd == 0x01U) {
        uint32_t enable;
        uint32_t mode;

        if (argc != 2 ||
            tool_parse_u32_arg(argv[0], 0U, 1U, &enable) < 0 ||
            tool_parse_u32_arg(argv[1], 0U, 2U, &mode) < 0) {
            fprintf(stderr, "set_mode args: <enable 0|1> <mode 0=current 1=velocity 2=position>\n");
            return -1;
        }
        data[1] = (uint8_t)enable;
        data[2] = (uint8_t)mode;
    } else if (cmd == 0x02U || cmd == 0x03U || cmd == 0x04U) {
        int32_t value;

        if (argc != 1 || tool_parse_i32_arg(argv[0], INT32_MIN, INT32_MAX, &value) < 0) {
            fprintf(stderr, "target args: <int32-value>\n");
            return -1;
        }
        tool_put_le32(&data[1], (uint32_t)value);
    } else if (cmd == 0x10U || cmd == 0x12U) {
        uint32_t param;
        int32_t value;

        if (argc != 2 ||
            tool_parse_u32_arg(argv[0], 0U, 0xffU, &param) < 0 ||
            tool_parse_i32_arg(argv[1], INT32_MIN, INT32_MAX, &value) < 0) {
            fprintf(stderr, "set-pid args: <param> <int32-value>\n");
            return -1;
        }
        data[1] = (uint8_t)param;
        tool_put_le32(&data[2], (uint32_t)value);
    } else if (cmd == 0x26U) {
        uint32_t param;
        uint32_t value;

        if (argc != 2 ||
            tool_parse_u32_arg(argv[0], 0U, 0xffU, &param) < 0 ||
            tool_parse_u32_arg(argv[1], 0U, UINT32_MAX, &value) < 0) {
            fprintf(stderr, "set-foc args: <param> <uint32-value>\n");
            return -1;
        }
        data[1] = (uint8_t)param;
        tool_put_le32(&data[2], value);
    } else if (cmd == 0x11U || cmd == 0x13U || cmd == 0x27U) {
        uint32_t param;

        if (argc != 1 || tool_parse_u32_arg(argv[0], 0U, 0xffU, &param) < 0) {
            fprintf(stderr, "read-param args: <param>\n");
            return -1;
        }
        data[1] = (uint8_t)param;
    } else if (cmd == 0x20U) {
        uint32_t node;

        if (argc != 1 || tool_parse_u32_arg(argv[0], 0U, 0xffU, &node) < 0) {
            fprintf(stderr, "set_node_id args: <node-id>\n");
            return -1;
        }
        data[1] = (uint8_t)node;
    } else if (cmd == 0x23U) {
        uint32_t enable;
        uint32_t period;

        if (argc != 2 ||
            tool_parse_u32_arg(argv[0], 0U, 1U, &enable) < 0 ||
            tool_parse_u32_arg(argv[1], 0U, 0xffffU, &period) < 0) {
            fprintf(stderr, "report config args: <enable> <period-ms>\n");
            return -1;
        }
        data[1] = (uint8_t)enable;
        tool_put_le16(&data[2], (uint16_t)period);
    } else if (cmd == 0x25U) {
        if (argc != 0) {
            fprintf(stderr, "reset_can_config takes no args\n");
            return -1;
        }
        data[1] = 0xa5U;
    } else if (cmd == 0x61U) {
        uint32_t hz;
        uint32_t seconds;
        uint32_t uid;

        if (argc != 3 ||
            tool_parse_u32_arg(argv[0], 0U, 255U, &hz) < 0 ||
            tool_parse_u32_arg(argv[1], 0U, 255U, &seconds) < 0 ||
            tool_parse_u32_arg(argv[2], 0U, UINT32_MAX, &uid) < 0) {
            fprintf(stderr, "identify args: <hz> <duration-s> <uid32>\n");
            return -1;
        }
        data[1] = (uint8_t)hz;
        data[2] = (uint8_t)seconds;
        tool_put_le32(&data[3], uid);
    } else if (cmd == 0x62U) {
        uint32_t node;
        uint32_t uid;

        if (argc != 2 ||
            tool_parse_u32_arg(argv[0], 0U, 255U, &node) < 0 ||
            tool_parse_u32_arg(argv[1], 0U, UINT32_MAX, &uid) < 0) {
            fprintf(stderr, "configure_node_id args: <new-node-id> <uid32>\n");
            return -1;
        }
        data[1] = (uint8_t)node;
        tool_put_le32(&data[3], uid);
    } else if (argc > 0) {
        size_t len = 0;

        if (tool_parse_hex_bytes(argc, argv, &data[1], CAN_TOOL_DLC - 1U, &len) < 0) {
            fprintf(stderr, "invalid raw CAN args\n");
            return -1;
        }
    }

    return 0;
}
