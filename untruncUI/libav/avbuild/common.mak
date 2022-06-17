#
# common bits used by all libraries
#

include $(SRC_PATH)/avbuild/arch.mak

OBJS      += $(OBJS-yes)
FFLIBS    := $($(NAME)_FFLIBS) $(FFLIBS-yes) $(FFLIBS)
TESTPROGS += $(TESTPROGS-yes)

FFEXTRALIBS := $(FFLIBS:%=$(LD_LIB)) $(foreach lib,EXTRALIBS-$(NAME) $(FFLIBS:%=EXTRALIBS-%),$($(lib))) $(EXTRALIBS)

OBJS      := $(sort $(OBJS:%=$(SUBDIR)%))
TESTOBJS  := $(TESTOBJS:%=$(SUBDIR)tests/%) $(TESTPROGS:%=$(SUBDIR)tests/%.o)
TESTPROGS := $(TESTPROGS:%=$(SUBDIR)tests/%$(EXESUF))
HOSTOBJS  := $(HOSTPROGS:%=$(SUBDIR)%.o)
HOSTPROGS := $(HOSTPROGS:%=$(SUBDIR)%$(HOSTEXESUF))
TOOLS     += $(TOOLS-yes)
TOOLOBJS  := $(TOOLS:%=tools/%.o)
TOOLS     := $(TOOLS:%=tools/%$(EXESUF))
HEADERS   += $(HEADERS-yes)

PATH_LIBNAME = $(foreach NAME,$(1),lib$(NAME)/$($(2)LIBNAME))
DEP_LIBS := $(foreach lib,$(FFLIBS),$(call PATH_LIBNAME,$(lib),$(CONFIG_SHARED:yes=S)))
STATIC_DEP_LIBS := $(foreach lib,$(FFLIBS),$(call PATH_LIBNAME,$(lib)))

SRC_DIR    := $(SRC_PATH)/lib$(NAME)
ALLHEADERS := $(subst $(SRC_DIR)/,$(SUBDIR),$(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/$(ARCH)/*.h))
SKIPHEADERS += $(ARCH_HEADERS:%=$(ARCH)/%) $(SKIPHEADERS-)
SKIPHEADERS := $(SKIPHEADERS:%=$(SUBDIR)%)
HOBJS        = $(filter-out $(SKIPHEADERS:.h=.h.o),$(ALLHEADERS:.h=.h.o))
$(HOBJS):     CCFLAGS += $(CFLAGS_HEADERS)
checkheaders: $(HOBJS)
.SECONDARY:   $(HOBJS:.o=.c)

alltools: $(TOOLS)

$(HOSTOBJS): %.o: %.c
	$(COMPILE_HOSTC)

$(HOSTPROGS): %$(HOSTEXESUF): %.o
	$(HOSTLD) $(HOSTLDFLAGS) $(HOSTLD_O) $^ $(HOSTEXTRALIBS)

$(OBJS):     | $(sort $(dir $(OBJS)))
$(HOBJS):    | $(sort $(dir $(HOBJS)))
$(HOSTOBJS): | $(sort $(dir $(HOSTOBJS)))
$(TESTOBJS): | $(sort $(dir $(TESTOBJS)))
$(TOOLOBJS): | tools

OUTDIRS := $(OUTDIRS) $(dir $(OBJS) $(HOBJS) $(HOSTOBJS) $(TESTOBJS))

CLEANSUFFIXES     = *.d *.gcda *.gcno *.h.c *.map *.o *.pc *.ver *.version *~
LIBSUFFIXES       = *.a *.lib *.so *.so.* *.dylib *.dll *.def *.dll.a

define RULES
clean::
	$(RM) $(HOSTPROGS) $(TESTPROGS) $(TOOLS)
endef

$(eval $(RULES))

-include $(wildcard $(OBJS:.o=.d) $(HOSTOBJS:.o=.d) $(TESTOBJS:.o=.d) $(HOBJS:.o=.d))
