# Cemu launcher for Nintendo Switch.
.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_TITLE	:=	Cemu
APP_AUTHOR	:=	naga
RELEASE_VERSION	?=	1.2.0
APP_VERSION	?=	$(RELEASE_VERSION)

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET		:=	cemu
BUILD		:=	build
SOURCES		:=	source ../src/platform/switch/common
DATA		:=	data
INCLUDES	:=	../src/platform/switch/common \
			../dependencies/ZArchive/include \
			../build_switch/_deps/libsmb2-src/include \
			../build_switch/_deps/libusbhsfs-src/include
ROMFS		:=	romfs		# emulator and forwarder payloads
ICON		:=	Cemu.jpg

ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

PKGS	:=	sdl2 SDL2_ttf SDL2_image libcurl

CFLAGS	:=	`$(PREFIX)pkg-config --cflags $(PKGS)` -Wall -Wextra -O2 -DNDEBUG -ffunction-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
			-DCEMU_SWITCH_VERSION='"$(APP_VERSION)"' \
			-DCEMU_SWITCH_RELEASE_VERSION='"$(RELEASE_VERSION)"' $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

ifeq ($(UPDATE_TEST),1)
CFLAGS	+=	-DCEMU_SWITCH_UPDATE_TEST
endif

CXXFLAGS	:= $(CFLAGS) -std=gnu++17

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=none

STORAGE_LIBS := $(TOPDIR)/../build_switch/_deps/libsmb2-build/lib/libsmb2.a \
				$(TOPDIR)/../build_switch/_deps/libusbhsfs-build/liblibusbhsfs.a \
				$(PORTLIBS)/lib/libntfs-3g.a
ARCHIVE_LIBS := $(TOPDIR)/../build_switch/dependencies/ZArchive/libzarchive.a
LIBS	:=	$(STORAGE_LIBS) $(ARCHIVE_LIBS) `$(PREFIX)pkg-config --libs $(PKGS)` -lturbojpeg -lzstd -lz -lGLESv2 -lnx

STAGED_MESA20 := $(TOPDIR)/../build_switch/launcher_mesa20/opt/devkitpro/portlibs/switch
ifneq ($(wildcard $(STAGED_MESA20)/lib/libGLESv2.a),)
LAUNCHER_MESA20_ROOT ?= $(STAGED_MESA20)
else
LAUNCHER_MESA20_ROOT ?= $(PORTLIBS)
endif

LIBDIRS	:= $(LAUNCHER_MESA20_ROOT) $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# Link with the C++ driver.
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(strip $(ROMFS)),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(TOPDIR)/romfs/forwarder-icon.jpg: $(APP_ICON)
	@cp "$<" "$@"

$(BUILD): $(TOPDIR)/romfs/forwarder-icon.jpg
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

ROMFS_FILES := $(shell find $(TOPDIR)/$(ROMFS) -type f)
$(OUTPUT).nro : $(APP_ICON) $(ROMFS_FILES)

$(ROMFS_FILES) $(STORAGE_LIBS) $(ARCHIVE_LIBS):
	@test -f "$@" || { echo "Missing build input: $@" >&2; exit 1; }

# The launcher consumes CMake's storage archives directly. Relink when the
# backend or its NTFS dependency changes, even if launcher sources do not.
$(OUTPUT).elf	:	$(OFILES) $(STORAGE_LIBS)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
