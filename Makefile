# Main Makefile

LVGL_DIR_NAME ?= lvgl
LVGL_DIR ?= ${shell pwd}

# Toolchain setup (defaults to bundled Sigmastar toolchain)
TOOLCHAIN ?= $(PWD)/toolchain/sigmastar-infinity6e
SYSROOT ?= $(TOOLCHAIN)/arm-openipc-linux-gnueabihf/sysroot
CC ?= $(TOOLCHAIN)/bin/arm-openipc-linux-gnueabihf-gcc

include $(LVGL_DIR)/$(LVGL_DIR_NAME)/lvgl.mk

SDK := ./sdk
DRV ?= ./firmware/general/package/sigmastar-osdrv-infinity6e/files/lib

CFLAGS += -Wno-address-of-packed-member -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6E__

# Size-first build flags
CFLAGS += -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
LDFLAGS += --sysroot=$(SYSROOT) -Wl,--gc-sections -s -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib

SRCS := main.c
OSD_SEND_SRC := osd_send.c

OUTPUT_NAME := lvgltest
OUTPUT ?= $(abspath $(OUTPUT_NAME))
OSD_SEND_OUTPUT_NAME := osd_send
OSD_SEND_OUTPUT ?= $(abspath $(OSD_SEND_OUTPUT_NAME))

BUILD_DIR := build

# Convert LVGL CSRCS into object files inside build directory
LVGL_OBJS := $(addprefix $(BUILD_DIR)/, $(CSRCS:.c=.o))

# Convert your own sources into objects
APP_OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))
OSD_SEND_OBJ := $(addprefix $(BUILD_DIR)/, $(OSD_SEND_SRC:.c=.o))

# Include paths
INCLUDES := -I$(SDK)/include \
            -I$(SYSROOT)/usr/include \
            -I$(PWD) \
            -I$(LVGL_DIR)/$(LVGL_DIR_NAME)

LIBS := -lcam_os_wrapper -lmi_rgn -lmi_sys -ldl

# Target
all: $(OUTPUT) $(OSD_SEND_OUTPUT)

$(OUTPUT): $(APP_OBJS) $(LVGL_OBJS)
	@mkdir -p $(@D)
	$(CC) $(APP_OBJS) $(LVGL_OBJS) $(INCLUDES) $(CFLAGS) $(LDFLAGS) -L$(DRV) $(LIBS) -o $@

$(OSD_SEND_OUTPUT): $(OSD_SEND_OBJ)
	@mkdir -p $(@D)
	$(CC) $(OSD_SEND_OBJ) $(CFLAGS) $(LDFLAGS) -o $@

# Build rule for all .c files (app + LVGL)
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(OUTPUT) $(OSD_SEND_OUTPUT)

.PHONY: all clean
