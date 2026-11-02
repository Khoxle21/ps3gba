# Executable-memory probe app (see jitprobe.c). Built by tools/jitprobe.sh.
.SUFFIXES:
include $(PSL1GHT)/ppu_rules

TARGET		:=	jitprobe
BUILD		:=	build
TITLE		:=	JIT probe
APPID		:=	PGBA00099
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
# IMPORTS="audio gcm io sysutil net netctl sysmodule": import those system libraries (any subset)
ifneq ($(IMPORTS),)
CFLAGS		+=	-DIMPORTS $(foreach lib,$(IMPORTS),-DIMP_$(lib))
endif
ifneq ($(PROC_PARAM),)
CFLAGS		+=	-DPROC_PARAM
endif
ifneq ($(CAVE_MB),)
CFLAGS		+=	-DCAVE_MB=$(CAVE_MB)
endif
LDFLAGS		=	$(MACHDEP)
LIBS		:=	-lsysutil -lrt -llv2
ifneq ($(IMPORTS),)
LIBS		:=	-laudio -lgcm_sys -lio -lsysutil -lnetctl -lnet -lsysmodule -lrt -llv2
endif
# AUDIOSTUB=init|used: cellAudio through our own stub (audio_stub.S) instead of libaudio
ifneq ($(AUDIOSTUB),)
CFLAGS		+=	-DIMPORTS -DIMP_audio
ASFLAGS		+=	$(if $(filter used,$(AUDIOSTUB)),-DAUDIOSTUB_USED)
LIBS		:=	-lsysutil -lrt -llv2
EXTRA_OFILES	:=	audio_stub.o
endif

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(CURDIR)
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)
export LD		:=	$(CC)
export OFILES	:=	jitprobe.o $(EXTRA_OFILES)
export INCLUDE	:=	$(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self $(OUTPUT)*.pkg

pkg: $(BUILD) $(OUTPUT).pkg

else

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
	$(VERB) echo linking ... $(notdir $@)
	$(VERB) $(LD) $^ $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@
ifeq ($(WRITABLE_TEXT),1)
	@# Mark the code segment (first program header) RWX: RPCS3 maps it read-only otherwise.
	$(VERB) printf '\000\000\000\007' | dd of=$@ bs=1 seek=68 conv=notrunc 2>/dev/null
endif

endif
