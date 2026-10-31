###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = nextui
# The ffmpeg headers are used at compile time and the libraries are NEVER
# linked: video/vidload.c dlopen()s them by soname at first use. Adding
# -lavcodec (or anything else libav*) to LDFLAGS below would put an entry in
# nextui.elf's DT_NEEDED and cost boot time on every single boot, including
# the boots that never show a preview. workspace/h700/check-video-preview.sh
# fails if that ever happens, and reads both this line and the built ELF.
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/scaler.c ../common/utils.c ../common/config.c ../common/api.c ../../$(PLATFORM)/platform/platform.c
ifeq ($(PLATFORM),h700)
INCDIR += -I../../$(PLATFORM)/ffmpeg/output/include
SOURCE += ../common/video/vidload.c ../common/video/vidplayer.c ../common/video/vidaudio.c
endif

CC = $(CROSS_COMPILE)gcc
CFLAGS  += $(OPT)
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS	 += -lmsettings

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../../$(PLATFORM)/libmsettings && make
