###########################################################################
# Kbuild fragment for hantro.ko
###########################################################################

#
# Define Hantro_{SOURCES,OBJECTS}
#

include $(src)/ipoffset/project_config.mk

HANTRO_SOURCES ?=
HANTRO_SOURCES_CXX ?=


HANTRO_SOURCES += hantro/hantro_drv.c
HANTRO_SOURCES += hantro/hantro_fence.c
HANTRO_SOURCES += hantro/vcmdswhwregisters.c
HANTRO_SOURCES += hantro/bidirect_list.c
HANTRO_SOURCES += hantro/devicemgr.c
HANTRO_SOURCES += hantro/hantro_fs.c
HANTRO_SOURCES += hantro/hantro_drm.c
HANTRO_SOURCES += hantro/hantro_dmabuf.c
HANTRO_SOURCES += hantro/hantro_mem_pool.c

ifeq ($(VSI_FPGA_MEM),y)
	HANTRO_SOURCES += hantro/hantro_fpga_mem.c
endif

ifeq ($(PCIE),y)
	HANTRO_SOURCES += hantro/hantro_pcie.c
endif

ifeq ($(VSI_VCE),y)
	HANTRO_SOURCES += hantro/hx280enc.c
endif

ifeq ($(VSI_VCD),y)
	HANTRO_SOURCES += hantro/hantrodec.c
endif

ifeq ($(VSI_VCMD),y)
	HANTRO_SOURCES += hantro/hantro_vcmd.c
	HANTRO_SOURCES += hantro/hantro_vcmd_slice.c
endif

ifeq ($(VSI_CACHE),y)
	HANTRO_SOURCES += hantro/hantrocache.c
endif

ifeq ($(VSI_DEC400),y)
	HANTRO_SOURCES += hantro/hantrodec400.c
endif

ifeq ($(VSI_AXIFE),y)
	HANTRO_SOURCES += hantro/hantro_axife.c
endif

ifeq ($(VSI_MMU),y)
	HANTRO_SOURCES += hantro/hantro_mmu.c
endif

HANTRO_OBJECTS = $(patsubst %.c,%.o,$(HANTRO_SOURCES))

obj-m += ls2k3000_vpu.o
ls2k3000_vpu-y := $(HANTRO_OBJECTS)

HANTRO_KO = hantro/ls2k3000_vpu.ko

#
# Define ls2k3000_vpu.ko-specific CFLAGS.
#

HANTRO_CFLAGS += -I$(src)/hantro
HANTRO_CFLAGS +=-DUSE_CMA
HANTRO_CFLAGS += -DHANTRO_UNDEF_LEGACY_BIT_MACROS

ifeq ($(LG_BUILD_TYPE),release)
 HANTRO_CFLAGS += -UDEBUG -U_DEBUG -DNDEBUG
endif

ifeq ($(LG_BUILD_TYPE),develop)
 HANTRO_CFLAGS += -UDEBUG -U_DEBUG -DNDEBUG -DNV_MEM_LOGGER
endif

ifeq ($(LG_BUILD_TYPE),debug)
 HANTRO_CFLAGS += -DDEBUG -D_DEBUG -UNDEBUG -DNV_MEM_LOGGER
endif

ifeq ($(VSI_FPGA_MEM),y)
 HANTRO_CFLAGS += -DVSI_FPGA_MEM
endif

ifeq ($(VSI_FPGA_PCIE),y)
 HANTRO_CFLAGS += -DVSI_FPGA_PCIE
endif

ifeq ($(PCIE),y)
 HANTRO_CFLAGS += -DPCIE_EN
endif

ifeq ($(VSI_VCE),y)
 HANTRO_CFLAGS += -DHAS_VCE
endif

ifeq ($(VSI_VCD),y)
 HANTRO_CFLAGS += -DHAS_VCD
endif

ifeq ($(VSI_VCMD),y)
 HANTRO_CFLAGS += -DHAS_VCMD
endif

ifeq ($(VSI_CACHE),y)
 HANTRO_CFLAGS += -DHAS_CACHECORE
endif

ifeq ($(VSI_DEC400),y)
 HANTRO_CFLAGS += -DHAS_DEC400
endif

ifeq ($(VSI_AXIFE),y)
 HANTRO_CFLAGS += -DHAS_AXIFE
endif

ifeq ($(VSI_MMU),y)
 HANTRO_CFLAGS += -DHAS_MMU
endif

ifeq ($(VSI_SUPPORT_PM),y)
 HANTRO_CFLAGS += -DVSI_CONFIG_PM
endif

$(call ASSIGN_PER_OBJ_CFLAGS, $(HANTRO_OBJECTS), $(HANTRO_CFLAGS))


# Linux kernel v5.12 and later looks at "always-y", Linux kernel versions
# before v5.6 looks at "always"; kernel versions between v5.12 and v5.6
# look at both.

always += $(HANTRO_INTERFACE)
always-y += $(HANTRO_INTERFACE)

$(obj)/$(HANTRO_INTERFACE): $(addprefix $(obj)/,$(HANTRO_OBJECTS))
	$(LD) -r -o $@ $^

#
# Register the conftests needed by hantro.ko
#
LG_OBJECTS_DEPEND_ON_CONFTEST += $(HANTRO_OBJECTS)

LG_CONFTEST_TYPE_COMPILE_TESTS += class_create_has_owner_arg
LG_CONFTEST_TYPE_COMPILE_TESTS += class_devnode_has_const
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_gem_object_has_funcs
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_buf_ops_has_cache_sgt_mapping
LG_CONFTEST_TYPE_COMPILE_TESTS += vm_flags_set
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_driver_has_gem_prime_mmap
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_driver_has_gem_dumb_destroy
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_ioctl_flags
LG_CONFTEST_TYPE_COMPILE_TESTS += platform_driver_remove_ret_type
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_dev_fini
LG_CONFTEST_TYPE_COMPILE_TESTS += mmap_read_lock
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_buf_ops_vmap_arg_dma_buf
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_buf_ops_vmap_arg_dma_buf_map
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_resv_wait_timeout
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_resv_add_fence
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_resv_get_excl
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_gem_object_has_resv
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_driver_has_gem_prime_export
LG_CONFTEST_TYPE_COMPILE_TESTS += gem_prime_export_has_device
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_mode_config_has_allow_fb_modifiers
LG_CONFTEST_TYPE_COMPILE_TESTS += vm_operations_fault_ret_int
LG_CONFTEST_TYPE_COMPILE_TESTS += dma_resv_reserve_fences
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_gem_object_reference
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_driver_prime_flag_present
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_gem_object_put_unlocked
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_get_format_info_use_pixel_format
LG_CONFTEST_TYPE_COMPILE_TESTS += drm_helper_mode_fill_fb_struct_passes_info
