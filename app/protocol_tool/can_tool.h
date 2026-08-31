#ifndef CAN_TOOL_H_
#define CAN_TOOL_H_

#include <stdint.h>

#define CAN_TOOL_DLC 8U

int can_tool_build_data(uint16_t id, uint8_t cmd, int argc, char **argv,
                        uint8_t data[CAN_TOOL_DLC]);
void can_tool_print_frame(uint16_t id, const uint8_t data[CAN_TOOL_DLC]);
void can_tool_print_id_class(uint16_t id);
void can_tool_decode_payload(uint16_t id, const uint8_t data[CAN_TOOL_DLC]);

#endif
