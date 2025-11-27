# Main Makefile

LVGL_DIR_NAME ?= lvgl
LVGL_DIR ?= ${shell pwd}

include $(LVGL_DIR)/$(LVGL_DIR_NAME)/lvgl.mk

SDK := ./sdk

CFLAGS += -Wno-address-of-packed-member -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6E__

SRCS := main.c

OUTPUT_NAME := lvgltest
OUTPUT ?= $(abspath $(OUTPUT_NAME))

BUILD_DIR := build

# Convert LVGL CSRCS into object files inside build directory
LVGL_OBJS := $(addprefix $(BUILD_DIR)/, $(CSRCS:.c=.o))

# Convert your own sources into objects
APP_OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))

# Include paths
INCLUDES := -I$(SDK)/include \
            -I$(TOOLCHAIN)/usr/include \
            -I$(PWD) \
            -I$(LVGL_DIR)/$(LVGL_DIR_NAME)

LIBS := -lcam_os_wrapper -lmi_rgn -lmi_sys

# Target
all: $(OUTPUT)

$(OUTPUT): $(APP_OBJS) $(LVGL_OBJS)
	@mkdir -p $(@D)
	$(CC) $(APP_OBJS) $(LVGL_OBJS) $(INCLUDES) -L$(DRV) $(CFLAGS) $(LIBS) -Os -s -o $@

# Build rule for all .c files (app + LVGL)
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(OUTPUT)

.PHONY: all clean
