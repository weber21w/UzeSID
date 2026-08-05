.DEFAULT_GOAL := all

## General Flags
RAM_LIMIT ?= 4096
STACK_RESERVE ?= 512
PROJECT = UzeSID
MCU = atmega644
TARGET = $(PROJECT).elf
CC = avr-gcc
INFO=../gameinfo.properties
UZEBIN_DIR = ../../../bin
FLASH_LIMIT = 61440

## Kernel settings
KERNEL_DIR = ../../../kernel

KERNEL_OPTIONS = -DVIDEO_MODE=3 -DINTRO_LOGO=0 -DTILE_WIDTH=8 -DSCREEN_TILES_V=15 -DFIRST_RENDER_LINE=68
KERNEL_OPTIONS += -DMAX_SPRITES=1 -DRAM_TILES_COUNT=4 -DRESOLUTION_EXT=1 -DVRAM_TILES_H=32 ##-DSNES_MOUSE=1
KERNEL_OPTIONS += -DUART=0 -DUART_RX_BUFFER_SIZE=256 -DUART_TX_BUFFER_SIZE=8 -DINCLUDE_DEFAULT_WAVES=0
# UzeSID never asks the kernel to format EEPROM.  Keep the read/write block API,
# but omit the format signature/table and startup format probe.
KERNEL_OPTIONS += -DNO_EEPROM_FORMAT=1
# The stock note step/length tables live in uzeboxSoundEngine.o, which this
# custom-mixer build does not link.  Custom kernel branches may expose an
# additional table-suppression macro; pass its name with, for example:
#   make KERNEL_NOTE_TABLE_FLAG=NO_NOTE_LENGTH_TABLE
KERNEL_NOTE_TABLE_FLAG ?=
ifneq ($(strip $(KERNEL_NOTE_TABLE_FLAG)),)
KERNEL_OPTIONS += -D$(KERNEL_NOTE_TABLE_FLAG)=1
endif
# Match the known-good UzeMOD custom-mixer kernel configuration.  The kernel
# initializes mix_buf, mix_pos, mix_bank, video, and interrupts before main().
# NO_CHANNELS removes only the unused stock synth/music channel ABI.
KERNEL_OPTIONS += -DSOUND_MIXER=0 -DENABLE_MIXER=0 -DNO_CHANNELS=1 -DCONTROLLERS_VSYNC_READ=0 -DTRANSLUCENT_COLOR=0xC7
KERNEL_OPTIONS += -DPF_USE_LSEEK=1 -DPF_USE_DIR=1 -DPF_USE_WRITE=1 -D_PF_USE_WRITE=1 -D_FS_USE_WRITE=1 -DSPIRAM_DETECT_SIZE=1

## Demo / app settings
##DEMO_OPTIONS = -DFORMAT_EEPROM=1
DEMO_OPTIONS += -DUZESID_HAVE_PFF
DEMO_OPTIONS += -DUZESID_USE_REAL_UZESID_HEADER
DEMO_OPTIONS += -DUZESID_DB_OFFSET=61952UL
DEMO_OPTIONS += -DUZESID_ROM_FILENAME=\"UZESID.UZE\"
UZESID_DB_WRITE ?= 1
UZESID_ENABLE_CAPTURE ?= 1
UZESID_ENABLE_CACHE_WRITE ?= 1
DEMO_OPTIONS += -DUZESID_DB_WRITE=$(UZESID_DB_WRITE)
DEMO_OPTIONS += -DUZESID_ENABLE_CAPTURE=$(UZESID_ENABLE_CAPTURE)
DEMO_OPTIONS += -DUZESID_ENABLE_CACHE_WRITE=$(UZESID_ENABLE_CACHE_WRITE)
DEMO_OPTIONS += -DUZESID_ENABLE_VISUALIZER=0
DEMO_OPTIONS += -DUZESID_ENABLE_ILLEGAL_OPS=0
# The ROM always uses the built-in UzeSID sink.  Omit the generic callback/tick
# playback path; the desktop converter keeps it because it does not define this.
DEMO_OPTIONS += -DUZESID_DIRECT_SID_ONLY=1
# Experimental full-rate SID oscillator path.  The DAC is always 15.72 kHz;
# this changes the SID core from 7.86 kHz + interpolation to native 15.72 kHz.
SID_NATIVE_15KHZ ?= 0
ifeq ($(SID_NATIVE_15KHZ),1)
DEMO_OPTIONS += -DUZESID_SYNTH_DUP=1
endif

# Optional 6 dB per-voice attenuation for direct A/B testing.  This is one
# arithmetic shift in the existing voice multiply path, not a second mixer.
UZESID_MIX_SHIFT ?= 0
DEMO_OPTIONS += -DUZESID_MIX_SHIFT=$(UZESID_MIX_SHIFT)

# Make normally cannot tell that command-line preprocessor values changed.
# Track the synthesis/mix configuration explicitly so switching from native15
# back to plain `make` cannot silently reuse native objects (or vice versa).
UZESID_BUILD_REV ?= 20260805_cachemix1
BUILD_CONFIG_STAMP := .build-config-$(UZESID_BUILD_REV)-s$(SID_NATIVE_15KHZ)-m$(UZESID_MIX_SHIFT)-d$(UZESID_DB_WRITE)-c$(UZESID_ENABLE_CACHE_WRITE)

## Options common to compile, link and assembly rules
COMMON = -mmcu=$(MCU)

## Compile options common for all C compilation units.
CFLAGS = $(COMMON)
CFLAGS += -Wall -gdwarf-2 -std=gnu99 -DF_CPU=28636360UL -Os -fsigned-char -ffunction-sections -flto
CFLAGS += -MD -MP -MT $(*F).o -MF dep/$(@F).d
CFLAGS += $(KERNEL_OPTIONS)
CFLAGS += $(DEMO_OPTIONS)
CFLAGS += -mcall-prologues -fdata-sections -ffreestanding -fomit-frame-pointer

## Assembly specific flags
ASMFLAGS = $(COMMON)
ASMFLAGS += $(CFLAGS)
ASMFLAGS += -x assembler-with-cpp -Wa,-gdwarf2

## Linker flags
LDFLAGS = $(COMMON)
LDFLAGS += -Wl,-Map=$(PROJECT).map
LDFLAGS += -Wl,-gc-sections,--relax -flto

## Intel Hex file production flags
HEX_FLASH_FLAGS = -R .eeprom

HEX_EEPROM_FLAGS = -j .eeprom
HEX_EEPROM_FLAGS += --set-section-flags=.eeprom="alloc,load"
HEX_EEPROM_FLAGS += --change-section-lma .eeprom=0 --no-change-warnings

## Objects that must be built in order to link
OBJECTS = \
	uzeboxVideoEngineCore.o \
	uzeboxCore.o \
	uzeboxVideoEngine.o \
	pff.o \
	diskio.o \
	spiram.o \
	UzeSID.o \
	Cache.o \
	Emulation.o \
	soundMixerCustom.o

## Objects explicitly added by the user
LINKONLYOBJECTS =



.PHONY: check-source-headers
check-source-headers:
	@grep -q '^#ifndef UZESID_CACHE_H' ../Cache.h || { \
		echo "ERROR: ../Cache.h is not the UzeSID C header (it may have been overwritten by Songlengths.md5)."; \
		echo "Restore Cache.h from the project archive before building."; exit 1; \
	}

$(BUILD_CONFIG_STAMP):
	@rm -f .build-config-* $(OBJECTS) $(TARGET) $(PROJECT).hex $(PROJECT).eep $(PROJECT).lss $(PROJECT).uze *.uze
	@touch $@
	@if [ "$(SID_NATIVE_15KHZ)" = "1" ]; then mode="15.72 kHz native"; else mode="7.86 kHz + interpolation"; fi; \
	 echo "Build configuration: $$mode, voice shift=$(UZESID_MIX_SHIFT), DB write=$(UZESID_DB_WRITE)"

$(OBJECTS): | check-source-headers $(BUILD_CONFIG_STAMP)

## Writable database generation
DB_TOOL_DIR = ../db-tool
DB_SID_DIR = $(DB_TOOL_DIR)/sids
DB_FILE = $(DB_TOOL_DIR)/Songlengths_writable.bin
DB_PREBUILT_HEADER = $(DB_TOOL_DIR)/prebuilt_sids.inc
DB_PACK_TOOL = $(DB_TOOL_DIR)/pack_uze_dependency.py
PACK_INFO = .gameinfo.pack.properties
DB_TAIL_SIZE ?= 128M
DB_DIR_ENTRIES ?= 512
DB_SPI_BANKS ?= 64
DB_JOBS ?= $(shell n=$$(nproc 2>/dev/null || echo 1); [ $$n -gt 8 ] && echo 8 || echo $$n)
DB_SID_FILES := $(shell find $(DB_SID_DIR) -type f \( -iname '*.sid' \) 2>/dev/null)
DB_TOOL_FILES := \
	$(DB_TOOL_DIR)/build_custom_database.py \
	$(DB_TOOL_DIR)/build_lidx.py \
	$(DB_TOOL_DIR)/Songlengths.md5 \
	$(DB_TOOL_DIR)/Songlengths.local.md5 \
	$(DB_TOOL_DIR)/Songlengths.override.md5 \
	$(DB_TOOL_DIR)/host-converter/Makefile \
	$(DB_TOOL_DIR)/host-converter/uzesid_capture.c \
	../UzeSID.c ../UzeSID.h ../Emulation.c ../Emulation.h ../Cache.c ../Cache.h

## Include Directories
INCLUDES = -I"$(KERNEL_DIR)" -I".."

## Resources
#RESOURCES = ../data/tiles.inc #../data/adpcm.inc

## Build
all: check-source-headers database $(RESOURCES) $(TARGET) $(PROJECT).hex $(PROJECT).eep $(PROJECT).lss $(PROJECT).uze size check-ram

database: $(DB_FILE)

$(DB_FILE): $(DB_SID_FILES) $(DB_TOOL_FILES)
	python3 $(DB_TOOL_DIR)/build_custom_database.py $(DB_SID_DIR) \
		--output $(DB_FILE) --tail-size $(DB_TAIL_SIZE) \
		--dir-entries $(DB_DIR_ENTRIES) --reserve 0 \
		--spi-banks $(DB_SPI_BANKS) \
		--prebuilt-header $(DB_PREBUILT_HEADER) \
		-j $(DB_JOBS)

## Rebuild graphics resources
../data/tiles.inc: ../data/tiles.png ../data/tiles.xml
	$(UZEBIN_DIR)/gconvert ../data/tiles.xml

## Compile Kernel files
uzeboxVideoEngineCore.o: $(KERNEL_DIR)/uzeboxVideoEngineCore.s Makefile
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

uzeboxCore.o: $(KERNEL_DIR)/uzeboxCore.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

soundMixerCustom.o: ../soundMixerCustom.s Makefile
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

uzeboxVideoEngine.o: $(KERNEL_DIR)/uzeboxVideoEngine.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

diskio.o: $(KERNEL_DIR)/petitfatfs/diskio.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

pff.o: $(KERNEL_DIR)/petitfatfs/pff.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

spiram.o: $(KERNEL_DIR)/spiram.s $(DIRS) Makefile
	$(CC) $(INCLUDES) $(ASMFLAGS) -c $< -o $@

## Compile UzeSID sources
$(DB_PREBUILT_HEADER): $(DB_FILE)
	@test -f $@ || { echo "ERROR: database build did not create $@"; exit 1; }

UzeSID.o: ../UzeSID.c $(DB_PREBUILT_HEADER) Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

Cache.o: ../Cache.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

Emulation.o: ../Emulation.c Makefile
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

## Link
$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LINKONLYOBJECTS) $(LIBDIRS) $(LIBS) -o $(TARGET)

$(PROJECT).hex $(PROJECT).uze: check-size check-ram

%.hex: $(TARGET)
	avr-objcopy -O ihex $(HEX_FLASH_FLAGS) $< $@

%.eep: $(TARGET)
	-avr-objcopy $(HEX_EEPROM_FLAGS) -O ihex $< $@ || exit 0

%.lss: $(TARGET)
	avr-objdump -h -S $< > $@

$(PACK_INFO): $(INFO) Makefile
	@sed '/^dependency=/d' $(INFO) > $@

%.uze: %.hex $(TARGET) $(DB_FILE) $(PACK_INFO) $(DB_PACK_TOOL)
	$(UZEBIN_DIR)/packrom $(PROJECT).hex $@ $(PACK_INFO)
	python3 $(DB_PACK_TOOL) $@ $(DB_FILE)

check-size: $(TARGET)
	@used=`avr-size -C --mcu=$(MCU) $(TARGET) | awk '/Program:/ {print $$2}'`; \
	if [ -z "$$used" ]; then \
		echo "ERROR: could not determine flash usage"; exit 1; \
	elif [ "$$used" -gt "$(FLASH_LIMIT)" ]; then \
		echo "ERROR: $(PROJECT) uses $$used bytes; packed Uzebox limit is $(FLASH_LIMIT) bytes"; exit 1; \
	else \
		echo "Flash image: $$used / $(FLASH_LIMIT) bytes"; \
	fi

size: ${TARGET}
	@echo
	@avr-size -C --mcu=${MCU} ${TARGET}

## Static SRAM safety check
check-ram: $(TARGET)
	@data=`avr-size -C --mcu=$(MCU) $(TARGET) | awk '/Data:/ {print $$2}'`; \
	limit=$$(( $(RAM_LIMIT) - $(STACK_RESERVE) )); \
	if [ -z "$$data" ]; then \
		echo "ERROR: Could not determine static SRAM use"; exit 1; \
	elif [ "$$data" -gt "$$limit" ]; then \
		echo "ERROR: Static SRAM $$data bytes leaves less than $(STACK_RESERVE) bytes for stack/interrupts (limit $$limit)"; exit 1; \
	else \
		echo "Static SRAM: $$data / $(RAM_LIMIT) bytes; reserved stack/interrupt margin: $(STACK_RESERVE) bytes"; \
	fi

.PHONY: clean clean-db database validate-db check-size check-ram check-source-headers host-converter custom-db-help normal native15 native15-full quiet native15-quiet

validate-db:
	python3 ../db-tool/validate_database.py


host-converter:
	$(MAKE) -C ../db-tool/host-converter

# Explicit clean normal build.  Plain `make` also switches back correctly via
# BUILD_CONFIG_STAMP, but this target is convenient for release testing.
normal:
	$(MAKE) clean
	$(MAKE) SID_NATIVE_15KHZ=0 UZESID_MIX_SHIFT=0 all

# Native 15.72 kHz synthesis retains the same capture and persistent database
# write features as the normal build.
native15:
	$(MAKE) clean
	$(MAKE) SID_NATIVE_15KHZ=1 UZESID_MIX_SHIFT=$(UZESID_MIX_SHIFT) all

# 6 dB A/B builds.  Persistent DB read/write remains enabled.
quiet:
	$(MAKE) clean
	$(MAKE) SID_NATIVE_15KHZ=0 UZESID_MIX_SHIFT=1 all

native15-quiet:
	$(MAKE) clean
	$(MAKE) SID_NATIVE_15KHZ=1 UZESID_MIX_SHIFT=1 all

# Backward-compatible name from the earlier size-diagnostic package.
native15-full: native15

custom-db-help:
	python3 ../db-tool/build_custom_database.py --help

clean-db:
	-rm -f $(DB_FILE) $(DB_FILE).manifest.json $(DB_PREBUILT_HEADER)

clean:
	-rm -rf $(RESOURCES) $(OBJECTS) $(PROJECT).* dep/* *.uze .build-config-* $(PACK_INFO)

## Other dependencies
-include $(shell mkdir dep 2>/dev/null) $(wildcard dep/*)
