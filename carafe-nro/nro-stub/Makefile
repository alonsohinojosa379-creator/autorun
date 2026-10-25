.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Define DEVKITPRO")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
TARGET := carafe-launcher
BUILD := build
SOURCES := source
CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lnx
OFILES := main.o
all: $(TARGET).nro
$(TARGET).elf: $(addprefix $(BUILD)/,$(OFILES))
	$(CC) $(LDFLAGS) $^ -L$(DEVKITPRO)/libnx/lib $(LIBS) -o $@
$(BUILD)/%.o: $(SOURCES)/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(TARGET).nro: $(TARGET).elf
	elf2nro $< $@
clean:
	rm -rf $(BUILD) *.elf *.nro *.map
