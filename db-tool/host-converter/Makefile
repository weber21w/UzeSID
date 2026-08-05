PROJECT_ROOT := ../..
STUBS := stubs
CC ?= cc
TARGET := uzesid_capture
CFLAGS ?= -O3 -g0
CFLAGS += -std=gnu99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
CFLAGS += -I$(STUBS) -I$(PROJECT_ROOT)
CFLAGS += -DUZESID_HAVE_PFF=1 -DUZESID_ENABLE_CACHE_WRITE=0 -DUZESID_ENABLE_CAPTURE=1 -DUZESID_CAPTURE_MAX_EVENTS=255
CFLAGS += -DUZESID_HOST_CONVERTER=1
SID_NATIVE_15KHZ ?= 0
ifeq ($(SID_NATIVE_15KHZ),1)
CFLAGS += -DUZESID_SYNTH_DUP=1
endif
UZESID_MIX_SHIFT ?= 0
CFLAGS += -DUZESID_MIX_SHIFT=$(UZESID_MIX_SHIFT)
LDFLAGS += -Wl,--gc-sections

OBJECTS := uzesid_capture.o UzeSID_host.o Emulation_host.o Cache_host.o

.PHONY: all clean check-source-headers
all: check-source-headers $(TARGET)


check-source-headers:
	@grep -q '^#ifndef UZESID_CACHE_H' $(PROJECT_ROOT)/Cache.h || { \
		echo "ERROR: $(PROJECT_ROOT)/Cache.h is not the UzeSID C header (it may have been overwritten by Songlengths.md5)."; \
		echo "Restore Cache.h from the project archive before building."; exit 1; \
	}

$(OBJECTS): | check-source-headers

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

uzesid_capture.o: uzesid_capture.c $(PROJECT_ROOT)/UzeSID.h $(PROJECT_ROOT)/Emulation.h $(PROJECT_ROOT)/Cache.h
	$(CC) $(CFLAGS) -c $< -o $@

UzeSID_host.o: $(PROJECT_ROOT)/UzeSID.c $(PROJECT_ROOT)/UzeSID.h $(PROJECT_ROOT)/Emulation.h $(PROJECT_ROOT)/Cache.h
	$(CC) $(CFLAGS) -Dmain=uzesid_rom_main -c $< -o $@

Emulation_host.o: $(PROJECT_ROOT)/Emulation.c $(PROJECT_ROOT)/Emulation.h $(PROJECT_ROOT)/Cache.h
	$(CC) $(CFLAGS) -c $< -o $@

Cache_host.o: $(PROJECT_ROOT)/Cache.c $(PROJECT_ROOT)/Cache.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)
