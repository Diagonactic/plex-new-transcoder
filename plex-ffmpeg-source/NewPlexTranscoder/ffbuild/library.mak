include $(SRC_PATH)/ffbuild/common.mak

ifeq (,$(filter %clean config,$(MAKECMDGOALS)))
-include $(SUBDIR)lib$(NAME).version
endif

LIBVERSION := $(lib$(NAME)_VERSION)
LIBMAJOR   := $(lib$(NAME)_VERSION_MAJOR)
LIBMINOR   := $(lib$(NAME)_VERSION_MINOR)
INCINSTDIR := $(INCDIR)/lib$(NAME)

INSTHEADERS := $(INSTHEADERS) $(HEADERS:%=$(SUBDIR)%)

EXTLIBS := $(foreach type, $(COMPONENTTYPES), $(filter %$(type), $(EXTERNALS)))

EXTFFLIBS := $(EXTFFLIBS) -l$(NAME)

install-lib$(NAME)-components:

ORIGNAME := $(NAME)

NAME = $(EXTLIB)
UPPERNAME = $(shell echo $(EXTLIB) | tr a-z A-Z)
TYPE = $(notdir $(subst _,/,$(UPPERNAME)))
define EXTRULE
-include $(wildcard $(patsubst $(filter-out $(OBJS-$(NAME):%=$(SUBDIR)%), $(OBJS)), %.o, %.d) $(OBJS-$(NAME):%=$(SUBDIR)%-$(NAME).d) $(SUBDIR)$(NAME)-extlib_init.d)
$(SUBDIR)$(NAME)-extlib_init.o: libavutil/extlib_init.c
	$$(COMPILE_C) -DEXTLIBNAME=ff_$(NAME) -DBUILDING_EXTERNAL_$(TYPE) -DHAVE_AV_CONFIG_H -DFFLIB_$(ORIGNAME) -DEXTLIBHWACCELS="$(HWACCELS-$(NAME))" -DEXTLIBHWACCEL_PTRS="$(HWACCELS-$(NAME):%=&%)"
$(SUBDIR)%.o-$(NAME).o: $(SUBDIR)%.c
	$$(COMPILE_C) -DBUILDING_$(UPPERNAME)_EXTERNAL -DBUILDING_EXTERNAL -DHAVE_AV_CONFIG_H
$(SUBDIR)%.o-$(NAME).o: $(SUBDIR)%.S
	$$(COMPILE_S) -DBUILDING_$(UPPERNAME)_EXTERNAL -DBUILDING_EXTERNAL -DHAVE_AV_CONFIG_H
$(SUBDIR)%.o-$(NAME).o: $(SUBDIR)%.asm
	$$(DEPX86ASM) $$(X86ASMFLAGS) -I $$(<D)/ -M -o $$@ $$< > $$(@:.o=.d)
	$$(X86ASM) $$(X86ASMFLAGS) -I $$(<D)/ -o $$@ $$<
	-$$(if $$(ASMSTRIPFLAGS), $$(STRIP) $$(ASMSTRIPFLAGS) $$@)
$(SUBDIR)lib$(NAME).ver:
	$(Q)echo $(NAME)_1 "{ global: av_init_library; local: *; };" | $(VERSION_SCRIPT_POSTPROCESS_CMD) > $$@
$(SUBDIR)$(SLIBNAME): $(filter-out $(OBJS-$(NAME):%=$(SUBDIR)%), $(OBJS)) $(OBJS-$(NAME):%=$(SUBDIR)%-$(NAME).o) $(SUBDIR)$(NAME)-extlib_init.o $(SUBDIR)lib$(NAME).ver $(FF_STATIC_DEP_LIBS)
	$(SLIB_CREATE_DEF_CMD)
	$$(LD) $(SHFLAGS) $(EXTLIBFLAGS) $(LDFLAGS) $(LDSOFLAGS) $$(LD_O) $$(filter %.o,$$^) $(FF_STATIC_DEP_LIBS) $(FFEXTRALIBS)
install-$(NAME): $(SUBDIR)$(SLIBNAME)
	$(Q)mkdir -p "$(SHLIBDIR)/components/"
	$$(INSTALL) -m 755 $$< "$(SHLIBDIR)/components/$(SLIBNAME)"
	$$(STRIP) "$(SHLIBDIR)/components/$(SLIBNAME)"
install-lib$(ORIGNAME)-components: install-$(NAME)
all: $(SUBDIR)$(SLIBNAME)
endef

$(foreach EXTLIB, $(EXTLIBS), $(eval $(EXTRULE)))

NAME := $(ORIGNAME)

all-$(CONFIG_STATIC): $(SUBDIR)$(LIBNAME)  $(SUBDIR)lib$(FULLNAME).pc
all-$(CONFIG_SHARED): $(SUBDIR)$(SLIBNAME) $(SUBDIR)lib$(FULLNAME).pc

LIBOBJS := $(OBJS) $(SUBDIR)%.h.o $(TESTOBJS)
$(LIBOBJS) $(LIBOBJS:.o=.s) $(LIBOBJS:.o=.i):   CPPFLAGS += -DHAVE_AV_CONFIG_H

$(SUBDIR)$(LIBNAME): $(OBJS)
	$(RM) $@
	$(AR) $(ARFLAGS) $(AR_O) $^
	$(RANLIB) $@

install-headers: install-lib$(NAME)-headers install-lib$(NAME)-pkgconfig

install-components: install-lib$(ORIGNAME)-components

install-libs-$(CONFIG_STATIC): install-lib$(NAME)-static
install-libs-$(CONFIG_SHARED): install-lib$(NAME)-shared

define RULES
$(TOOLS):     THISLIB = $(FULLNAME:%=$(LD_LIB))
$(TESTPROGS): THISLIB = $(SUBDIR)$(LIBNAME)

$(LIBOBJS): CPPFLAGS += -DBUILDING_$(NAME)

$(TESTPROGS) $(TOOLS): %$(EXESUF): %.o
	$$(LD) $(LDFLAGS) $(LDEXEFLAGS) $$(LD_O) $$(filter %.o,$$^) $$(THISLIB) $(FFEXTRALIBS) $$(EXTRALIBS-$$(*F)) $$(ELIBS)

$(SUBDIR)lib$(NAME).version: $(SUBDIR)version.h | $(SUBDIR)
	$$(M) $$(SRC_PATH)/ffbuild/libversion.sh $(NAME) $$< > $$@

$(SUBDIR)lib$(FULLNAME).pc: $(SUBDIR)version.h ffbuild/config.sh | $(SUBDIR)
	$$(M) $$(SRC_PATH)/ffbuild/pkgconfig_generate.sh $(NAME) "$(DESC)"

$(SUBDIR)lib$(NAME).ver: $(SUBDIR)lib$(NAME).v $(OBJS)
	$$(M)sed 's/MAJOR/$(lib$(NAME)_VERSION_MAJOR)/' $$< | $(VERSION_SCRIPT_POSTPROCESS_CMD) > $$@

$(SUBDIR)$(SLIBNAME): $(SUBDIR)$(SLIBNAME_WITH_MAJOR)
	$(Q)cd ./$(SUBDIR) && $(LN_S) $(SLIBNAME_WITH_MAJOR) $(SLIBNAME)

$(SUBDIR)$(SLIBNAME_WITH_MAJOR): $(OBJS) $(SLIBOBJS) $(SUBDIR)lib$(NAME).ver
	$(SLIB_CREATE_DEF_CMD)
	$$(LD) $(SHFLAGS) $(LDFLAGS) $(LDSOFLAGS) $$(LD_O) $$(filter %.o,$$^) $(FFEXTRALIBS)
	$(SLIB_EXTRA_CMD)

ifdef SUBDIR
$(SUBDIR)$(SLIBNAME_WITH_MAJOR): $(DEP_LIBS)
endif

clean::
	$$(call RM_SPLIT,$(addprefix $(SUBDIR),$(CLEANFILES) $(CLEANSUFFIXES) $(LIBSUFFIXES)) \
	    $(CLEANSUFFIXES:%=$(SUBDIR)$(ARCH)/%) $(CLEANSUFFIXES:%=$(SUBDIR)tests/%))

install-lib$(NAME)-shared: $(SUBDIR)$(SLIBNAME)
	$(Q)mkdir -p "$(SHLIBDIR)"
	$$(INSTALL) -m 755 $$< "$(SHLIBDIR)/$(SLIB_INSTALL_NAME)"
	$$(STRIP) "$(SHLIBDIR)/$(SLIB_INSTALL_NAME)"
	$(Q)$(foreach F,$(SLIB_INSTALL_LINKS),(cd "$(SHLIBDIR)" && $(LN_S) $(SLIB_INSTALL_NAME) $(F));)
	$(if $(SLIB_INSTALL_EXTRA_SHLIB),$$(INSTALL) -m 644 $(SLIB_INSTALL_EXTRA_SHLIB:%=$(SUBDIR)%) "$(SHLIBDIR)")
	$(if $(SLIB_INSTALL_EXTRA_LIB),$(Q)mkdir -p "$(LIBDIR)")
	$(if $(SLIB_INSTALL_EXTRA_LIB),$$(INSTALL) -m 644 $(SLIB_INSTALL_EXTRA_LIB:%=$(SUBDIR)%) "$(LIBDIR)")

install-lib$(NAME)-static: $(SUBDIR)$(LIBNAME)
	$(Q)mkdir -p "$(LIBDIR)"
	$$(INSTALL) -m 644 $$< "$(LIBDIR)"
	$(LIB_INSTALL_EXTRA_CMD)

install-lib$(NAME)-headers: $(addprefix $(SUBDIR),$(HEADERS) $(BUILT_HEADERS))
	$(Q)mkdir -p "$(INCINSTDIR)"
	$$(INSTALL) -m 644 $$^ "$(INCINSTDIR)"

install-lib$(NAME)-pkgconfig: $(SUBDIR)lib$(FULLNAME).pc
	$(Q)mkdir -p "$(PKGCONFIGDIR)"
	$$(INSTALL) -m 644 $$^ "$(PKGCONFIGDIR)"

uninstall-libs::
	-$(RM) "$(SHLIBDIR)/$(SLIBNAME_WITH_MAJOR)" \
	       "$(SHLIBDIR)/$(SLIBNAME)"            \
	       "$(SHLIBDIR)/$(SLIBNAME_WITH_VERSION)"
	-$(RM)  $(SLIB_INSTALL_EXTRA_SHLIB:%="$(SHLIBDIR)/%")
	-$(RM)  $(SLIB_INSTALL_EXTRA_LIB:%="$(LIBDIR)/%")
	-$(RM) "$(LIBDIR)/$(LIBNAME)"

uninstall-headers::
	$(RM) $(addprefix "$(INCINSTDIR)/",$(HEADERS) $(BUILT_HEADERS))
	-rmdir "$(INCINSTDIR)"

uninstall-pkgconfig::
	$(RM) "$(PKGCONFIGDIR)/lib$(FULLNAME).pc"
endef

$(eval $(RULES))

$(TOOLS):     $(DEP_LIBS) $(SUBDIR)$($(CONFIG_SHARED:yes=S)LIBNAME)
$(TESTPROGS): $(DEP_LIBS) $(SUBDIR)$(LIBNAME)

testprogs: $(TESTPROGS)
