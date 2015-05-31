MAIN_MAKEFILE=1
include config.mak

vpath %.c    $(SRC_PATH)
vpath %.h    $(SRC_PATH)
vpath %.S    $(SRC_PATH)
vpath %.asm  $(SRC_PATH)
vpath %.v    $(SRC_PATH)
vpath %.texi $(SRC_PATH)


PROGS-$(CONFIG_FFMPEG)   += ffmpeg
PROGS-$(CONFIG_FFPLAY)   += ffplay

PROGS      := $(PROGS-yes:%=%$(EXESUF))
PROGS_G     = $(PROGS-yes:%=%_g$(EXESUF))
OBJS        = $(PROGS-yes:%=%.o) ffmpeg_transcoder.o ffmpeg_plex.o stringconversions.o cmdutils.o
MANPAGES    = $(PROGS-yes:%=doc/%.1)
PODPAGES    = $(PROGS-yes:%=doc/%.pod)
HTMLPAGES   = $(PROGS-yes:%=doc/%.html)
TOOLS       = $(addprefix tools/, $(addsuffix $(EXESUF), cws2fws graph2dot lavfi-showfiltfmts pktdumper probetest qt-faststart trasher))
TESTTOOLS   = audiogen videogen rotozoom tiny_psnr base64
HOSTPROGS  := $(TESTTOOLS:%=tests/%)

BASENAMES   = ffmpeg ffplay
ALLPROGS    = $(BASENAMES:%=%$(EXESUF))
ALLPROGS_G  = $(BASENAMES:%=%_g$(EXESUF))
ALLMANPAGES = $(BASENAMES:%=%.1)

FFLIBS-$(CONFIG_AVDEVICE) += avdevice
FFLIBS-$(CONFIG_AVFILTER) += avfilter
FFLIBS-$(CONFIG_AVFORMAT) += avformat
FFLIBS-$(CONFIG_AVCODEC)  += avcodec
FFLIBS-$(CONFIG_POSTPROC) += postproc
FFLIBS-$(CONFIG_SWSCALE)  += swscale

FFLIBS := avutil

DATA_FILES := $(wildcard $(SRC_PATH)/ffpresets/*.ffpreset)

SKIPHEADERS = cmdutils_common_opts.h

include $(SRC_PATH)/common.mak

FF_LDFLAGS   := $(FFLDFLAGS)
FF_EXTRALIBS := $(FFEXTRALIBS)
FF_DEP_LIBS  := $(DEP_LIBS)

#ALL_TARGETS-$(CONFIG_DOC)       += documentation

ifdef PROGS
INSTALL_TARGETS-yes             += install-progs install-data
INSTALL_TARGETS-$(CONFIG_DOC)   += install-man
endif
INSTALL_PROGS_TARGETS-$(CONFIG_SHARED) = install-libs

default: all
all: $(FF_DEP_LIBS) ffmpeg_transcoder.o ffmpeg_plex.o stringconversions.o $(PROGS) $(ALL_TARGETS-yes)

$(PROGS): %$(EXESUF): %_g$(EXESUF)
	$(CP) $< $@
	$(STRIP) $@

$(TOOLS): %$(EXESUF): %.o
	$(LD) $(LDFLAGS) -o $@ $< $(ELIBS)

tools/cws2fws$(EXESUF): ELIBS = -lz

config.h: .config
.config: $(wildcard $(FFLIBS:%=$(SRC_PATH)/lib%/all*.c))
	@-tput bold 2>/dev/null
	@-printf '\nWARNING: $(?F) newer than config.h, rerun configure\n\n'
	@-tput sgr0 2>/dev/null

SUBDIR_VARS := OBJS FFLIBS CLEANFILES DIRS TESTPROGS EXAMPLES SKIPHEADERS \
               ALTIVEC-OBJS MMX-OBJS NEON-OBJS X86-OBJS YASM-OBJS-FFT YASM-OBJS \
               HOSTPROGS BUILT_HEADERS TESTOBJS ARCH_HEADERS ARMV6-OBJS TOOLS

define RESET
$(1) :=
$(1)-yes :=
endef

define DOSUBDIR
$(foreach V,$(SUBDIR_VARS),$(eval $(call RESET,$(V))))
SUBDIR := $(1)/
include $(SRC_PATH)/$(1)/Makefile
endef

$(foreach D,$(FFLIBS),$(eval $(call DOSUBDIR,lib$(D))))

ffplay.o: CFLAGS += $(SDL_CFLAGS)
ffplay_g$(EXESUF): FF_EXTRALIBS += $(SDL_LIBS)
ffserver_g$(EXESUF): LDFLAGS += $(FFSERVERLDFLAGS)

%_g$(EXESUF): %.o cmdutils.o $(FF_DEP_LIBS) ffmpeg_transcoder.o ffmpeg_plex.o stringconversions.o
	$(LD) $(LDFLAGS) -o $@ $< cmdutils.o $(FF_EXTRALIBS) ffmpeg_transcoder.o ffmpeg_plex.o stringconversions.o $(PLEX_LATE_LIBS)

OBJDIRS += tools

-include $(wildcard tools/*.d)

VERSION_SH  = $(SRC_PATH)/version.sh
GIT_LOG     = $(SRC_PATH)/.git/logs/HEAD

.version: $(wildcard $(GIT_LOG)) $(VERSION_SH) config.mak
.version: M=@

version.h .version:
	$(M)$(VERSION_SH) $(SRC_PATH) version.h $(EXTRA_VERSION)
	$(Q)touch .version

# force version.sh to run whenever version might have changed
-include .version

ffmpeg_transcoder.o ffmpeg_transcoder.d: version.h
ffmpeg_plex.o ffmpeg_plex.d: version.h
stringconversions.o stringconversions.d: version.h

ifdef PROGS
install: install-progs install-data
endif

install: install-libs install-headers

install-libs: install-libs-yes

install-progs-yes:
install-progs-$(CONFIG_SHARED): install-libs

install-progs: install-progs-yes $(PROGS)
	$(Q)mkdir -p "$(BINDIR)"
	$(INSTALL) -c -m 755 $(PROGS) "$(BINDIR)"

install-data: $(DATA_FILES)
	$(Q)mkdir -p "$(DATADIR)"
	$(INSTALL) -m 644 $(DATA_FILES) "$(DATADIR)"

uninstall: uninstall-libs uninstall-headers uninstall-progs uninstall-data

uninstall-progs:
	$(RM) $(addprefix "$(BINDIR)/", $(ALLPROGS))

uninstall-data:
	$(RM) -r "$(DATADIR)"

clean::
	$(RM) $(ALLPROGS) $(ALLPROGS_G)
	$(RM) $(CLEANSUFFIXES)
	$(RM) $(TOOLS)
	$(RM) $(CLEANSUFFIXES:%=tools/%)

distclean::
	$(RM) $(DISTCLEANSUFFIXES)
	$(RM) config.* .version version.h libavutil/avconfig.h

config:
	$(SRC_PATH)/configure $(value FFMPEG_CONFIGURATION)

include $(SRC_PATH)/doc/Makefile
include $(SRC_PATH)/tests/Makefile

$(sort $(OBJDIRS)):
	$(Q)mkdir -p $@

# Dummy rule to stop make trying to rebuild removed or renamed headers
%.h:
	@:

# Disable suffix rules.  Most of the builtin rules are suffix rules,
# so this saves some time on slow systems.
.SUFFIXES:

.PHONY: all all-yes alltools *clean config examples install*
.PHONY: testprogs uninstall*
