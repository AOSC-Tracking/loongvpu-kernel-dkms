###########################################################################
# Kbuild fragment for loonggpu.ko
###########################################################################

#
# Define Loongson_{SOURCES,OBJECTS}
#


GSGPU_VDEC_SOURCES ?=
GSGPU_VDEC_SOURCES_CXX ?=

GSGPU_VDEC_SOURCES += src/gsgpu_vdec.c

GSGPU_VDEC_OBJECTS = $(patsubst %.c,%.o,$(GSGPU_VDEC_SOURCES))

obj-m += ls2k1000la_vpu.o
ls2k1000la_vpu-y := $(GSGPU_VDEC_OBJECTS)

GSGPU_VDEC_KO = src/ls2k1000la_vpu.ko

#
# Define ls2k1000la_vpu.ko-specific CFLAGS.
#

GSGPU_VDEC_CFLAGS += -I$(src)/src
GSGPU_VDEC_CFLAGS += -DGSGPU_VDEC_UNDEF_LEGACY_BIT_MACROS

ifeq ($(LG_BUILD_TYPE),release)
 GSGPU_VDEC_CFLAGS += -UDEBUG -U_DEBUG -DNDEBUG
endif

ifeq ($(LG_BUILD_TYPE),develop)
 GSGPU_VDEC_CFLAGS += -UDEBUG -U_DEBUG -DNDEBUG -DNV_MEM_LOGGER
endif

ifeq ($(LG_BUILD_TYPE),debug)
 GSGPU_VDEC_CFLAGS += -DDEBUG -D_DEBUG -UNDEBUG -DNV_MEM_LOGGER
endif

$(call ASSIGN_PER_OBJ_CFLAGS, $(GSGPU_VDEC_OBJECTS), $(GSGPU_VDEC_CFLAGS))


# Linux kernel v5.12 and later looks at "always-y", Linux kernel versions
# before v5.6 looks at "always"; kernel versions between v5.12 and v5.6
# look at both.

always += $(GSGPU_VDEC_INTERFACE)
always-y += $(GSGPU_VDEC_INTERFACE)

$(obj)/$(GSGPU_VDEC_INTERFACE): $(addprefix $(obj)/,$(GSGPU_VDEC_OBJECTS))
	$(LD) -r -o $@ $^

#
# Register the conftests needed by gsgpu_vdec.ko
#
LG_OBJECTS_DEPEND_ON_CONFTEST += $(GSGPU_VDEC_OBJECTS)

LG_CONFTEST_TYPE_COMPILE_TESTS += class_create_has_owner_arg
LG_CONFTEST_TYPE_COMPILE_TESTS += class_devnode_has_const
