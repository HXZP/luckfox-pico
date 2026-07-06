TARGET ?= imu_pose
BUILD_DIR ?= build
SRC := \
	bmi088.c \
	iio_backend.c \
	imu_pose.c \
	imu_pose_utils.c \
	imu_sim.c \
	loop_stats.c \
	pose_filter.c
OBJ := $(SRC:%.c=$(BUILD_DIR)/%.o)
HOST_OBJ := $(SRC:%.c=$(BUILD_DIR)/host-%.o)

TOOLCHAIN ?= /home/luckfox-pico-zero/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
CROSS_COMPILE ?= $(TOOLCHAIN)/arm-rockchip830-linux-uclibcgnueabihf-
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif

CFLAGS ?= -O2 -Wall -Wextra -Wshadow -Wformat=2 -std=c99
LDFLAGS ?= -lm

.PHONY: all clean host

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c imu_pose.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

host: $(BUILD_DIR)/$(TARGET)-host

$(BUILD_DIR)/$(TARGET)-host: $(HOST_OBJ)
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/host-%.o: %.c imu_pose.h
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
