TARGET ?= headlight_control
BUILD_DIR ?= build
SRC := headlight_control.c

TOOLCHAIN ?= /home/luckfox-pico-zero/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
CROSS_COMPILE ?= $(TOOLCHAIN)/arm-rockchip830-linux-uclibcgnueabihf-
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif

CFLAGS ?= -O2 -Wall -Wextra -Wshadow -Wformat=2 -std=c99
LDFLAGS ?=

.PHONY: all clean host

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

host:
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) $(SRC) -o $(BUILD_DIR)/$(TARGET)-host $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
