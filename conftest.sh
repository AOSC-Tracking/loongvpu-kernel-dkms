#!/bin/sh

PATH="${PATH}:/bin:/sbin:/usr/bin"

# make sure we are in the directory containing this script
SCRIPTDIR=`dirname $0`
cd $SCRIPTDIR

CC="$1"
ARCH=$2
SOURCES=$3
HEADERS=$SOURCES/include
OUTPUT=$4
XEN_PRESENT=1
PREEMPT_RT_PRESENT=0

# VGX_BUILD parameter defined only for VGX builds (vGPU Host driver)
# VGX_KVM_BUILD parameter defined only vGPU builds on KVM hypervisor
# GRID_BUILD parameter defined only for GRID builds (GRID Guest driver)
# GRID_BUILD_CSP parameter defined only for GRID CSP builds (GRID Guest driver for CSPs)

test_xen() {
    #
    # Determine if the target kernel is a Xen kernel. It used to be
    # sufficient to check for CONFIG_XEN, but the introduction of
    # modular para-virtualization (CONFIG_PARAVIRT, etc.) and
    # Xen guest support, it is no longer possible to determine the
    # target environment at build time. Therefore, if both
    # CONFIG_XEN and CONFIG_PARAVIRT are present, text_xen() treats
    # the kernel as a stand-alone kernel.
    #
    if ! test_configuration_option CONFIG_XEN ||
         test_configuration_option CONFIG_PARAVIRT; then
        XEN_PRESENT=0
    fi
}

append_conftest() {
    #
    # Echo data from stdin: this is a transitional function to make it easier
    # to port conftests from drivers with parallel conftest generation to
    # older driver versions
    #

    while read LINE; do
        echo ${LINE}
    done
}

test_header_presence() {
    #
    # Determine if the given header file (which may or may not be
    # present) is provided by the target kernel.
    #
    # Input:
    #   $1: relative file path
    #
    # This routine creates an upper case, underscore version of each of the
    # relative file paths, and uses that as the token to either define or
    # undefine in a C header file. For example, linux/fence.h becomes
    # NV_LINUX_FENCE_H_PRESENT, and that is either defined or undefined, in the
    # output (which goes to stdout, just like the rest of this file).

    TEST_CFLAGS="-E -M $CFLAGS"

    file="$1"
    file_define=LG_`echo $file | tr '/.\-a-z' '___A-Z'`_PRESENT

    CODE="#include <$file>"

    if echo "$CODE" | $CC $TEST_CFLAGS - > /dev/null 2>&1; then
        echo "#define $file_define"
    else
        # If preprocessing failed, it could have been because the header
        # file under test is not present, or because it is present but
        # depends upon the inclusion of other header files. Attempting
        # preprocessing again with -MG will ignore a missing header file
        # but will still fail if the header file is present.
        if echo "$CODE" | $CC $TEST_CFLAGS -MG - > /dev/null 2>&1; then
            echo "#undef $file_define"
        else
            echo "#define $file_define"
        fi
    fi
}

build_cflags() {
    ISYSTEM=`$CC -print-file-name=include 2> /dev/null`
    BASE_CFLAGS="-O2 -D__KERNEL__ \
-DKBUILD_BASENAME=\"#conftest$$\" -DKBUILD_MODNAME=\"#conftest$$\" \
-nostdinc -isystem $ISYSTEM \
-Wno-implicit-function-declaration -Wno-strict-prototypes"

    if [ "$OUTPUT" != "$SOURCES" ]; then
        OUTPUT_CFLAGS="-I$OUTPUT/include2 -I$OUTPUT/include"
        if [ -f "$OUTPUT/include/generated/autoconf.h" ]; then
            AUTOCONF_FILE="$OUTPUT/include/generated/autoconf.h"
        else
            AUTOCONF_FILE="$OUTPUT/include/linux/autoconf.h"
        fi
    else
        if [ -f "$HEADERS/generated/autoconf.h" ]; then
            AUTOCONF_FILE="$HEADERS/generated/autoconf.h"
        else
            AUTOCONF_FILE="$HEADERS/linux/autoconf.h"
        fi
    fi

    test_xen

    if [ "$XEN_PRESENT" != "0" ]; then
        MACH_CFLAGS="-I$HEADERS/asm/mach-xen"
    fi

    KERNEL_ARCH="$ARCH"

    if [ "$ARCH" = "i386" -o "$ARCH" = "x86_64" ]; then
        if [ -d "$SOURCES/arch/x86" ]; then
            KERNEL_ARCH="x86"
        fi
    fi

    SOURCE_HEADERS="$HEADERS"
    SOURCE_ARCH_HEADERS="$SOURCES/arch/$KERNEL_ARCH/include"
    OUTPUT_HEADERS="$OUTPUT/include"
    OUTPUT_ARCH_HEADERS="$OUTPUT/arch/$KERNEL_ARCH/include"

    # Look for mach- directories on this arch, and add it to the list of
    # includes if that platform is enabled in the configuration file, which
    # may have a definition like this:
    #   #define CONFIG_ARCH_<MACHUPPERCASE> 1
    for _mach_dir in `ls -1d $SOURCES/arch/$KERNEL_ARCH/mach-* 2>/dev/null`; do
        _mach=`echo $_mach_dir | \
            sed -e "s,$SOURCES/arch/$KERNEL_ARCH/mach-,," | \
            tr 'a-z' 'A-Z'`
        grep "CONFIG_ARCH_$_mach \+1" $AUTOCONF_FILE > /dev/null 2>&1
        if [ $? -eq 0 ]; then
            MACH_CFLAGS="$MACH_CFLAGS -I$_mach_dir/include"
        fi
    done

    if [ "$ARCH" = "arm" ]; then
        MACH_CFLAGS="$MACH_CFLAGS -D__LINUX_ARM_ARCH__=7"
    fi

    # Add the mach-default includes (only found on x86/older kernels)
    MACH_CFLAGS="$MACH_CFLAGS -I$SOURCE_HEADERS/asm-$KERNEL_ARCH/mach-default"
    MACH_CFLAGS="$MACH_CFLAGS -I$SOURCE_ARCH_HEADERS/asm/mach-default"

    CFLAGS="$BASE_CFLAGS $MACH_CFLAGS $OUTPUT_CFLAGS -include $AUTOCONF_FILE"
    CFLAGS="$CFLAGS -I$SOURCE_HEADERS"
    CFLAGS="$CFLAGS -I$SOURCE_HEADERS/uapi"
    CFLAGS="$CFLAGS -I$SOURCE_HEADERS/xen"
    CFLAGS="$CFLAGS -I$OUTPUT_HEADERS/generated/uapi"
    CFLAGS="$CFLAGS -I$SOURCE_ARCH_HEADERS"
    CFLAGS="$CFLAGS -I$SOURCE_ARCH_HEADERS/uapi"
    CFLAGS="$CFLAGS -I$OUTPUT_ARCH_HEADERS/generated"
    CFLAGS="$CFLAGS -I$OUTPUT_ARCH_HEADERS/generated/uapi"

    if [ -n "$BUILD_PARAMS" ]; then
        CFLAGS="$CFLAGS -D$BUILD_PARAMS"
    fi

    # Check if gcc supports asm goto and set CC_HAVE_ASM_GOTO if it does.
    # Older kernels perform this check and set this flag in Kbuild, and since
    # conftest.sh runs outside of Kbuild it ends up building without this flag.
    # Starting with commit e9666d10a5677a494260d60d1fa0b73cc7646eb3 this test
    # is done within Kconfig, and the preprocessor flag is no longer needed.

    GCC_GOTO_SH="$SOURCES/build/gcc-goto.sh"

    if [ -f "$GCC_GOTO_SH" ]; then
        # Newer versions of gcc-goto.sh don't print anything on success, but
        # this is okay, since it's no longer necessary to set CC_HAVE_ASM_GOTO
        # based on the output of those versions of gcc-goto.sh.
        if [ `/bin/sh "$GCC_GOTO_SH" "$CC"` = "y" ]; then
            CFLAGS="$CFLAGS -DCC_HAVE_ASM_GOTO"
        fi
    fi

    #
    # If CONFIG_HAVE_FENTRY is enabled and gcc supports -mfentry flags then set
    # CC_USING_FENTRY and add -mfentry into cflags.
    #
    # linux/ftrace.h file indirectly gets included into the conftest source and
    # fails to get compiled, because conftest.sh runs outside of Kbuild it ends
    # up building without -mfentry and CC_USING_FENTRY flags.
    #
    grep "CONFIG_HAVE_FENTRY \+1" $AUTOCONF_FILE > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "" > conftest$$.c

        $CC -mfentry -c -x c conftest$$.c > /dev/null 2>&1
        rm -f conftest$$.c

        if [ -f conftest$$.o ]; then
            rm -f conftest$$.o

            CFLAGS="$CFLAGS -mfentry -DCC_USING_FENTRY"
        fi
    fi

    #
    # If CONFIG_CPU_LOONGSON64 is enabled then reset _LOONGARCH_ISA to
    # _LOONGARCH_ISA_LOONGARCH64.
    #
    grep "CONFIG_CPU_LOONGSON64 \+1" $AUTOCONF_FILE > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        CFLAGS="$CFLAGS -U_LOONGARCH_ISA -D_LOONGARCH_ISA=_LOONGARCH_ISA_LOONGARCH64"
    fi
}

CONFTEST_PREAMBLE="#include \"conftest/headers.h\"
    #if defined(LG_LINUX_KCONFIG_H_PRESENT)
    #include <linux/kconfig.h>
    #endif
    #if defined(LG_GENERATED_AUTOCONF_H_PRESENT)
    #include <generated/autoconf.h>
    #else
    #include <linux/autoconf.h>
    #endif
    #if defined(CONFIG_XEN) && \
        defined(CONFIG_XEN_INTERFACE_VERSION) &&  !defined(__XEN_INTERFACE_VERSION__)
    #define __XEN_INTERFACE_VERSION__ CONFIG_XEN_INTERFACE_VERSION
    #endif
    #if defined(CONFIG_KASAN) && defined(CONFIG_ARM64)
    #if defined(CONFIG_KASAN_SW_TAGS)
    #define KASAN_SHADOW_SCALE_SHIFT 4
    #else
    #define KASAN_SHADOW_SCALE_SHIFT 3
    #endif
    #endif"

test_configuration_option() {
    #
    # Check to see if the given configuration option is defined
    #

    get_configuration_option $1 >/dev/null 2>&1

    return $?

}

set_configuration() {
    #
    # Set a specific configuration option.  This function is called to always
    # enable a configuration, in order to verify whether the test code for that
    # configuration is no longer required and the corresponding
    # conditionally-compiled code in the driver can be removed.
    #
    DEF="$1"

    if [ "$3" = "" ]
    then
        VAL=""
        CAT="$2"
    else
        VAL="$2"
        CAT="$3"
    fi

    echo "#define ${DEF} ${VAL}" | append_conftest "${CAT}"
}

unset_configuration() {
    #
    # Un-set a specific configuration option.  This function is called to
    # always disable a configuration, in order to verify whether the test
    # code for that configuration is no longer required and the corresponding
    # conditionally-compiled code in the driver can be removed.
    #
    DEF="$1"
    CAT="$2"

    echo "#undef ${DEF}" | append_conftest "${CAT}"
}

compile_check_conftest() {
    #
    # Compile the current conftest C file and check+output the result
    #
    CODE="$1"
    DEF="$2"
    VAL="$3"
    CAT="$4"

    echo "$CONFTEST_PREAMBLE
    $CODE" > conftest$$.c

    $CC $CFLAGS -c conftest$$.c > /dev/null 2>&1
    rm -f conftest$$.c

    if [ -f conftest$$.o ]; then
        rm -f conftest$$.o
        if [ "${CAT}" = "functions" ]; then
            #
            # The logic for "functions" compilation tests is inverted compared to
            # other compilation steps: if the function is present, the code
            # snippet will fail to compile because the function call won't match
            # the prototype. If the function is not present, the code snippet
            # will produce an object file with the function as an unresolved
            # symbol.
            #
            echo "#undef ${DEF}" | append_conftest "${CAT}"
        else
            echo "#define ${DEF} ${VAL}" | append_conftest "${CAT}"
        fi
        return
    else
        if [ "${CAT}" = "functions" ]; then
            echo "#define ${DEF} ${VAL}" | append_conftest "${CAT}"
        else
            echo "#undef ${DEF}" | append_conftest "${CAT}"
        fi
        return
    fi
}

export_symbol_present_conftest() {
    #
    # Check Module.symvers to see whether the given symbol is present.
    #

    SYMBOL="$1"
    TAB='	'

    if grep -e "${TAB}${SYMBOL}${TAB}.*${TAB}EXPORT_SYMBOL\(_GPL\)\?\s*\$" \
               "$OUTPUT/Module.symvers" >/dev/null 2>&1; then
        echo "#define LS_IS_EXPORT_SYMBOL_PRESENT_$SYMBOL 1" |
            append_conftest "symbols"
    else
        # May be a false negative if Module.symvers is absent or incomplete,
        # or if the Module.symvers format changes.
        echo "#define LS_IS_EXPORT_SYMBOL_PRESENT_$SYMBOL 0" |
            append_conftest "symbols"
    fi
}

export_symbol_gpl_conftest() {
    #
    # Check Module.symvers to see whether the given symbol is present and its
    # export type is GPL-only (including deprecated GPL-only symbols).
    #

    SYMBOL="$1"
    TAB='	'

    if grep -e "${TAB}${SYMBOL}${TAB}.*${TAB}EXPORT_\(UNUSED_\)*SYMBOL_GPL\s*\$" \
               "$OUTPUT/Module.symvers" >/dev/null 2>&1; then
        echo "#define LG_IS_EXPORT_SYMBOL_GPL_$SYMBOL 1" |
            append_conftest "symbols"
    else
        # May be a false negative if Module.symvers is absent or incomplete,
        # or if the Module.symvers format changes.
        echo "#define LG_IS_EXPORT_SYMBOL_GPL_$SYMBOL 0" |
            append_conftest "symbols"
    fi
}

get_configuration_option() {
    #
    # Print the value of given configuration option, if defined
    #
    RET=1
    OPTION=$1

    OLD_FILE="linux/autoconf.h"
    NEW_FILE="generated/autoconf.h"
    FILE=""

    if [ -f $HEADERS/$NEW_FILE -o -f $OUTPUT/include/$NEW_FILE ]; then
        FILE=$NEW_FILE
    elif [ -f $HEADERS/$OLD_FILE -o -f $OUTPUT/include/$OLD_FILE ]; then
        FILE=$OLD_FILE
    fi

    if [ -n "$FILE" ]; then
        #
        # We are looking at a configured source tree; verify
        # that its configuration includes the given option
        # via a compile check, and print the option's value.
        #

        if [ -f $HEADERS/$FILE ]; then
            INCLUDE_DIRECTORY=$HEADERS
        elif [ -f $OUTPUT/include/$FILE ]; then
            INCLUDE_DIRECTORY=$OUTPUT/include
        else
            return 1
        fi

        echo "#include <$FILE>
        #ifndef $OPTION
        #error $OPTION not defined!
        #endif

        $OPTION
        " > conftest$$.c

        $CC -E -P -I$INCLUDE_DIRECTORY -o conftest$$ conftest$$.c > /dev/null 2>&1

        if [ -e conftest$$ ]; then
            tr -d '\r\n\t ' < conftest$$
            RET=$?
        fi

        rm -f conftest$$.c conftest$$
    else
        CONFIG=$OUTPUT/.config
        if [ -f $CONFIG ] && grep "^$OPTION=" $CONFIG; then
            grep "^$OPTION=" $CONFIG | cut -f 2- -d "="
            RET=$?
        fi
    fi

    return $RET

}

check_for_ib_peer_memory_symbols() {
    kernel_dir="$1"
    module_symvers="${kernel_dir}/Module.symvers"

    sym_ib_register="ib_register_peer_memory_client"
    sym_ib_unregister="ib_unregister_peer_memory_client"
    tab='	'

    # Return 0 for true(no errors), 1 for false
    if [ ! -f "${module_symvers}" ]; then
        return 1
    fi

    if grep -e "${tab}${sym_ib_register}${tab}.*${tab}EXPORT_SYMBOL.*\$"    \
               "${module_symvers}" > /dev/null 2>&1 &&
       grep -e "${tab}${sym_ib_unregister}${tab}.*${tab}EXPORT_SYMBOL.*\$"  \
               "${module_symvers}" > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

compile_test() {
    case "$1" in
        drm_driver_prime_flag_present)
            #
            # Determine whether driver feature flag DRIVER_PRIME is present.
            #
            # The DRIVER_PRIME flag was added by commit 3248877ea179 (drm:
            # base prime/dma-buf support (v5)) in v3.4 (2011-11-25) and is
            # removed by commit 0424fdaf883a ("drm/prime: Actually remove
            # DRIVER_PRIME everywhere") in v5.4.
            #
            # DRIVER_PRIME define is changed to enum value by commit
            # 0e2a933b02c9 (drm: Switch DRIVER_ flags to an enum) in v5.1
            # (2019-01-29).
            #
            CODE="
            #if defined(LG_DRM_DRM_DRV_H_PRESENT)
            #include <drm/drm_drv.h>
            #endif

            unsigned int drm_driver_prime_flag_present_conftest(void) {
                return DRIVER_PRIME;
            }"

            compile_check_conftest "$CODE" "LG_DRM_DRIVER_PRIME_FLAG_PRESENT" "" "types"
        ;;

        drm_gem_object_reference)
            #
            # Determine if drm_gem_object_reference exists.
            #
            # Changed by commit 3e70fd160cf0b1945225eaa08dd2cb8544f21cb8 ("drm:
            # remove deprecated "[__]drm_gem_object_[un]reference[_locked]" functions") in 4.20-rc4
            #
            CODE="
            #include <drm/drm_gem.h>
            typeof(drm_gem_object_reference) conftest_drm_gem_object_reference;
            void conftest_drm_gem_object_reference(struct drm_gem_object *obj) {
                return;
            }"
            compile_check_conftest "$CODE" "LG_DRM_GEM_OBJECT_REFERENCE" "" "types"
        ;;

        drm_gem_object_put_unlocked)
            #
            # Determine if drm_gem_object_put_unlocked exists.
            #
            # Changed by commit 2f4dd13d4bb8a85f6d5b66a18989509924e4f5e9 ("drm/gem:
            # add drm_gem_object_put helper") in 5.7-rc7
            #
            CODE="
            #include <drm/drm_gem.h>
            typeof(drm_gem_object_put_unlocked) conftest_drm_gem_object_put_unlocked;
            void conftest_drm_gem_object_put_unlocked(struct drm_gem_object *obj) {
                return;
            }"
            compile_check_conftest "$CODE" "LG_DRM_GEM_OBJECT_PUT_UNLOCKED" "" "types"
        ;;

        dma_resv_reserve_fences)
            #
            # Determine if dma_resv_reserve_fences exists.
            #
            # Changed by commit c8d4c18bfbc4ab467188dbe45cc8155759f49d9e ("dma-buf/
            # drivers: make reserving a shared slot mandatory v4") in 5.18-rc2
            # proting in 6.6
            #
            CODE="
            #include <linux/dma-resv.h>
            typeof(dma_resv_reserve_fences) conftest_dma_resv_reserve_fences;
            int conftest_dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num_fences) {
                return 0;
            }"
            compile_check_conftest "$CODE" "LG_HAS_DMA_RESV_RESERVE_FENCES" "" "types"
        ;;

        vm_operations_fault_ret_int)
            #
            # Determine if the return type of vm_operations_struct->fault()
            # is int.
            #
            # commit 1c8f422059ae5da07db7406ab916203f9417e396 ("mm:
            # change return type to vm_fault_t") in v4.17-rc1.
            #
            CODE="
            #include <linux/mm.h>
            static const struct vm_operations_struct *ops;
            typeof(*ops->fault) conftest_vm_operations_fault_ret_int;
            int conftest_vm_operations_fault_ret_int(struct vm_fault *vmf) {
                return 0;
            }"
            compile_check_conftest "$CODE" "LG_VM_OPERATIONS_FAULT_RET_INT" "" "types"
        ;;


        drm_mode_config_has_allow_fb_modifiers)
            #
            # Determine if the drm_mode_config->allow_fb_modifiers exists
            #
            # by commit 3d082157a24216ca084082ce421a37d14ecfcfad ("drm:
            # remove allow_fb_modifiers")
            # in v5.17-rc3
            CODE="
            #include <drm/drm_mode_config.h>
            int conftest_drm_mode_config_has_allow_fb_modifiers(void) {
                return offsetof(struct drm_mode_config, allow_fb_modifiers);
            }"
            compile_check_conftest "$CODE" "LG_DRM_MODE_CONFIG_HAS_FB_MOD" "" "types"
        ;;


        gem_prime_export_has_device)
            #
            # Determine if the argument type of drm_driver->gem_prime_export()
            # has drm_device.
            #
            # commit e4fa8457b2197118538a1400b75c898f9faaf164 ("drm/prime::
            # Align gem_prime_export with obj_funcs.export") in v5.2-rc6.
            #
            CODE="
            #include <drm/drm_drv.h>
            static const struct drm_driver *ops;
            typeof(*ops->gem_prime_export) conftest_gem_prime_export_has_device;
            struct dma_buf *conftest_gem_prime_export_has_device(
                                              struct drm_device *dev,
                                              struct drm_gem_object *obj,
                                              int flags) {
                return NULL;
            }"
            compile_check_conftest "$CODE" "LG_GEM_PRIME_EXPORT_HAS_DRM_DEVICE" "" "types"
        ;;


        drm_driver_has_gem_prime_export)
            #
            # Determine if the drm_driver->gem_prime_export exists
            #
            # by commit d693def4fd1c23f1ca5aed1afb9993b3a2069ad2 ("drm:
            # Remove obsolete GEM and PRIME callbacks from struct drm_driver")
            # in v5.9-rc7
            CODE="
            #include <drm/drm_drv.h>
            int conftest_drm_driver_has_gem_prime_export(void) {
                return offsetof(struct drm_driver, gem_prime_export);
            }"
            compile_check_conftest "$CODE" "LG_DRM_DRIVER_HAS_EXPORT" "" "types"
        ;;


        drm_gem_object_has_resv)
            #
            # Determine if the drm_gem_object->resv exists
            #
            # by commit 1ba627148ef5d9dee879585687c4b0ee644f7ab5 ("drm:
            # Add reservation_object to drm_gem_object")
            # in v5.0-rc8
            CODE="
            #include <drm/drm_gem.h>
            int conftest_drm_gem_object_has_resv(void) {
                return offsetof(struct drm_gem_object, resv);
            }"
            compile_check_conftest "$CODE" "LG_DRM_GEM_OBJECT_HAS_RESV" "" "types"
        ;;


        dma_resv_get_excl)
            #
            # Determine if dma_resv_get_excl() is present.
            #
            # dma_resv_get_excl is removed
            # by commit 2254e49cef7015d7697bd1617d19e620e2788ec5 ("dma-resv: Fix kerneldoc") in v5.13-rc5
            #
            CODE="
            #include <linux/dma-resv.h>
            typeof(dma_resv_get_excl) conftest_dma_resv_get_excl;
            struct dma_fence *conftest_dma_resv_get_excl(struct dma_resv *obj) {
                return NULL;
            }"
            compile_check_conftest "$CODE" "LG_DMA_RESV_GET_EXCL_PRESENT" "" "types"
        ;;

        dma_resv_add_fence)
            #
            # Determine if dma_resv_add_fence function exists.
            #
            # Changed by commit 047a1b877ed48098bed71fcfb1d4891e1b54441d ("dma-buf:
            # remove dma_resv workaround") in v5.18-rc2
            # proting in 6.6
            #
            CODE="
            #include <linux/dma-resv.h>
            typeof(dma_resv_add_fence) conftest_dma_resv_add_fence;
            void conftest_dma_resv_add_fence(struct dma_resv *obj,
                                             struct dma_fence *fence,
                                             enum dma_resv_usage usage) {
                return;
            }"
            compile_check_conftest "$CODE" "LG_DMA_RESV_ADD_FENCE" "" "types"
        ;;

        dma_resv_wait_timeout)
            #
            # Determine if the dma_resv_wait_timeout function exists.
            #
            # Changed by commit d3fae3b3daac09961ab871a25093b0ae404282d5 ("dma-buf:
            # drop the _rcu postfix on function names v3") in v5.13-rc5
            #
            CODE="
            #include <linux/dma-resv.h>
            typeof(dma_resv_wait_timeout) conftest_dma_resv_wait_timeout;
            long conftest_dma_resv_wait_timeout(struct dma_resv *obj,
                                                enum dma_resv_usage usage,
                                                bool intr,
                                                unsigned long timeout) {
                return (long)0;
            }"
            compile_check_conftest "$CODE" "LG_DMA_RESV_WAIT_TIMEOUT" "" "types"
        ;;

        dma_buf_ops_vmap_arg_dma_buf)
            #
            # Determine if the argument type of dma_buf_ops->vmap()
            # is dma_buf.
            #
            # commit 6619ccf1bb1d0ebb071f758111efa83918b216fc ("dma-buf:
            # Use struct dma_buf_map in dma_buf_vmap() interfaces") in v5.9-rc8.
            #
            CODE="
            #include <linux/dma-buf.h>

            static const struct dma_buf_ops *ops;
            typeof(*ops->vmap) conftest_dma_buf_ops_vmap_arg_dma_buf;
            void *conftest_dma_buf_ops_vmap_arg_dma_buf(
                    struct dma_buf *arg) {
                void *ret = NULL;
                return ret;
            }"

            compile_check_conftest "$CODE" "LG_DMA_BUF_OPS_VMAP_ARG_DMA_BUF" "" "types"
        ;;


        dma_buf_ops_vmap_arg_dma_buf_map)
            #
            # Determine if the argument type of dma_buf_ops->vmap()
            # has dma_buf_map.
            #
            # commit 20e76f1a70596590dec32a5d1f598fba04859526 ("dma-buf:
            # Use struct dma_buf_map in dma_buf_vunmap() interfaces") in v5.9-rc8.
            #
            CODE="
            #include <linux/dma-buf.h>

            static const struct dma_buf_ops *ops;
            typeof(*ops->vmap) conftest_dma_buf_ops_vmap_arg_dma_buf;
            int conftest_dma_buf_ops_vmap_arg_dma_buf(struct dma_buf *arg,
                                                  struct dma_buf_map *map) {
                return 0;
            }"

            compile_check_conftest "$CODE" "LG_DMA_BUF_OPS_VMAP_ARG_DMA_BUF_MAP" "" "types"
        ;;


        mmap_read_lock)
            #
            # Determine if mmap_read_lock function exists.
            #
            # Added by commit 9740ca4e95b43b91a4a848694a20d01ba6818f7b
            # ("mmap locking API: initial implementation as rwsem wrappers")
            # in v5.8-rc1
            CODE="
            #include <linux/mmap_lock.h>
            typeof(mmap_read_lock) conftest_mmap_read_lock;
            void conftest_mmap_read_lock(struct mm_struct *mm) {
                return;
            }"

            compile_check_conftest "$CODE" "LG_MMAP_READ_LOCK" "" "types"
        ;;


        drm_dev_fini)
            #
            # Determine if drm_dev_fini function exists.
            #
            # Added by commit d33b58d0115e7eee011fddee2d8e25c6a09fb279
            # ("drm: Garbage collect drm_dev_fini")
            # in v5.6-rc7
            CODE="
            #include <drm/drm_drv.h>
            typeof(drm_dev_fini) conftest_drm_dev_fini;
            void conftest_drm_dev_fini(struct drm_device *dev) {
                return;
            }"

            compile_check_conftest "$CODE" "LG_DRM_DEV_FINI" "" "types"
        ;;


        platform_driver_remove_ret_type)
            #
            # Determine if the return type of platform_driver->remove()
            # is void.
            #
            # commit 0edb555a65d1ef047a9805051c36922b52a38a9d ("platform:
            # Make platform_driver::remove() return void") in v6.10-rc2.
            #
            CODE="
            #include <linux/platform_device.h>

            static const struct platform_driver *ops;
            typeof(*ops->remove) conftest_platform_driver_remove_ret_type;
            void conftest_platform_driver_remove_ret_type(
                    struct platform_device *p) {
                return;
            }"

            compile_check_conftest "$CODE" "LG_PLATFORM_REMOVE_RET_VOID" "" "types"
        ;;

        drm_ioctl_flags)
            #
            # Determine if drm_ioctl_flags enum contains the DRM_UNLOCKED.
            #
            # Changed by commit 2798ffcc1d6a788b5769b1fbcf0750dfc06ae98a ("drm:
            # Remove locking for legacy ioctls and DRM_UNLOCKED") in v6.7-rc5.
            #
            CODE="
            #include <drm/drm_ioctl.h>
            enum drm_ioctl_flags conftest_drm_ioctl_flags(void) {
               return DRM_UNLOCKED;
            }"
            compile_check_conftest "$CODE" "LG_DRM_IOCTL_FLAGS_UNLOCKED" "" "types"
        ;;

        drm_driver_has_gem_dumb_destroy)
            #
            # Determine if the drm_driver->dumb_destroy exists
            #
            # by commit 96a7b60f6ddb2bc966fac800c1dd18876a6e3c3f ("drm:
            # remove dumb_destroy callback")
            # in v6.4
            CODE="
            #include <drm/drm_drv.h>
            int conftest_drm_driver_has_gem_dumb_destroy(void) {
                return offsetof(struct drm_driver, gem_dumb_destroy);
            }"
            compile_check_conftest "$CODE" "LG_DRM_DRIVER_HAS_GEM_DUMB_DESTROY" "" "types"
        ;;

        drm_driver_has_gem_prime_mmap)
            #
            # Determine if the drm_driver->gem_prime_mmap exists
            #
            # by commit 0adec22702d497385dbdc52abb165f379a00efba ("drm:
            # Remove struct drm_driver.gem_prime_mmap")
            # in v6.4
            CODE="
            #include <drm/drm_drv.h>
            int conftest_drm_driver_has_gem_prime_mmap(void) {
                return offsetof(struct drm_driver, gem_prime_mmap);
            }"
            compile_check_conftest "$CODE" "LG_DRM_DRIVER_HAS_GEM_PRIME_MMAP" "" "types"
        ;;

        vm_flags_set)
            #
            # Determine if vm_flags_set function exists.
            #
            # Added by commit bc292ab00f6c7a661a8a605c714e8a148f629ef6
            # ("mm: introduce vma->vm_flags wrapper functions")
            # in v6.2-rc8
            CODE="
            #include <linux/mm.h>
            typeof(vm_flags_set) conftest_vm_flags_set;
            void conftest_vm_flags_set(struct vm_area_struct *vma,
                                       vm_flags_t flags) {
                return;
            }"

            compile_check_conftest "$CODE" "LG_VM_FLAGS_SET" "" "types"
        ;;

        drm_gem_object_has_funcs)
            #
            # Determine if the drm_gem_object has an funcs member.
            #
            # Added by commit b39b5394fabc79acbaafb26b777fd348c868bf7e
            # ("drm/gem: Add drm_gem_object_funcs")
            # in v4.20-rc4
            CODE="
            #include <drm/drm_gem.h>
            int conftest_drm_gem_object_has_funcs(void) {
                return offsetof(struct drm_gem_object, funcs);
            }"

            compile_check_conftest "$CODE" "LG_DRM_GEM_OBJECT_HAS_FUNCS" "" "types"
        ;;

        dma_buf_ops_has_cache_sgt_mapping)
            #
            # Determine if the dma_buf_ops has an cache_sgt_mapping member.
            #
            # Added by commit f13e143e7444bffc53f5c2904aeed76646da69d6
            # ("dma-buf: start caching of sg_table objects v2")
            # in v5.2-rc2.
            CODE="
            #include <linux/dma-buf.h>
            int conftest_dma_buf_ops_has_cache_sgt_mapping(void) {
                return offsetof(struct dma_buf_ops, cache_sgt_mapping);
            }"

            compile_check_conftest "$CODE" "LG_DMA_BUF_OPS_HAS_CACHE_SGT_MAPPING" "" "types"
        ;;


        class_create_has_owner_arg)
            #
            # Determine if class_create has 'owner' arg
            #
            # The 'owner' arg was removed by commit 1aaba11da9aa7
            # ("driver core: class: remove module * from class_create())
            # in v6.3-rc3.
            #
            CODE="
            #if defined (LG_LINUX_DEVICE_CLASS_H_PRESENT)
            #include <linux/device/class.h>
            #else
            #include <linux/device.h>
            #endif
            void conftest_class_create_has_owner_arg(void) {
                   class_create(
                           0,
                           0);
            }"

            compile_check_conftest "$CODE" "LG_CLASS_CREATE_HAS_OWNER_ARG" "" "types"

        ;;

        class_devnode_has_const)
            #
            # Determine if class_create has 'owner' arg
            #
            # The 'owner' arg was removed by commit f62b8e6588f
            # ("driver core: make struct class.devnode() take a const *")
            # in v6.1-rc7.
            #
            CODE="
            #if defined (LG_LINUX_DEVICE_CLASS_H_PRESENT)
            #include <linux/device/class.h>
            #else
            #include <linux/device.h>
            #endif
            static char *test_devnode(const struct device *dev, umode_t *mode){}
            void conftest_class_devnode_has_const(void) {
                   struct class * test_class;
                   test_class->devnode = test_devnode;
            }"

            compile_check_conftest "$CODE" "LG_CLASS_DEVNODE_HAS_CONST" "" "types"

        ;;

        uts_release)
            #
            # print the kernel's UTS_RELEASE string.
            #
            echo "#include <generated/utsrelease.h>
            UTS_RELEASE" > conftest$$.c

            $CC $CFLAGS -E -P conftest$$.c
            rm -f conftest$$.c
        ;;
        drm_get_format_info_use_pixel_format)
            #
            # Determine if drm_get_format_info uses pixel_format as argument
            #
            CODE="#include <drm/drm.h>
            #include <drm/drm_fourcc.h>
            void conftest_drm_get_format_info_use_pixel_format(struct drm_device *dev) {
                drm_get_format_info(dev, 0, 0);
            }"

            compile_check_conftest "$CODE" "LG_DRM_GET_FORMAT_INFO_USE_PIXEL_FORMAT" "" "types"
        ;;
        drm_helper_mode_fill_fb_struct_passes_info)
            #
            # Determine if drm_helper_mode_fill_fb_struct function accepts a format info argument.
            #
            CODE="#include <drm/drm_modeset_helper.h>
            void conftest_drm_helper_mode_fill_fb_struct_passes_info(
                    struct drm_device *dev,
                    struct drm_framebuffer *fb,
				    const struct drm_mode_fb_cmd2 *mode_cmd) {
                return drm_helper_mode_fill_fb_struct(dev, fb, 0, mode_cmd);
            }"

            compile_check_conftest "$CODE" "LG_DRM_HELPER_MODE_FILL_FB_STRUCT_PASSES_INFO" "" "types"
        ;;

        # When adding a new conftest entry, please use the correct format for
        # specifying the relevant upstream Linux kernel commit.  Please
        # avoid specifying -rc kernels, and only use SHAs that actually exist
        # in the upstream Linux kernel git repository.
        #
        # Added|Removed|etc by commit <short-sha> ("<commit message") in
        # <kernel-version>.

        *)
            # Unknown test name given
            echo "Error: unknown conftest '$1' requested" >&2
            exit 1
        ;;
    esac
}

case "$5" in
    cc_sanity_check)
        #
        # Check if the selected compiler can create object files
        # in the current environment.
        #
        VERBOSE=$6

        echo "int cc_sanity_check(void) {
            return 0;
        }" > conftest$$.c

        $CC -c conftest$$.c > /dev/null 2>&1
        rm -f conftest$$.c

        if [ ! -f conftest$$.o ]; then
            if [ "$VERBOSE" = "full_output" ]; then
                echo "";
            fi
            if [ "$CC" != "cc" ]; then
                echo "The C compiler '$CC' does not appear to be able to"
                echo "create object files.  Please make sure you have "
                echo "your Linux distribution's libc development package"
                echo "installed and that '$CC' is a valid C compiler";
                echo "name."
            else
                echo "The C compiler '$CC' does not appear to be able to"
                echo "create executables.  Please make sure you have "
                echo "your Linux distribution's gcc and libc development"
                echo "packages installed."
            fi
            if [ "$VERBOSE" = "full_output" ]; then
                echo "";
                echo "*** Failed CC sanity check. Bailing out! ***";
                echo "";
            fi
            exit 1
        else
            rm -f conftest$$.o
            exit 0
        fi
    ;;

    cc_version_check)
        #
        # Verify that the same compiler major and minor version is
        # used for the kernel and kernel module. A mismatch condition is
        # not considered fatal, so this conftest returns a success status
        # code, even if it fails. Failure of the test can be distinguished
        # by testing for empty (success) versus non-empty (failure) output.
        #
        # Some gcc version strings that have proven problematic for parsing
        # in the past:
        #
        #  gcc.real (GCC) 3.3 (Debian)
        #  gcc-Version 3.3 (Debian)
        #  gcc (GCC) 3.1.1 20020606 (Debian prerelease)
        #  version gcc 3.2.3
        #
        #  As of this writing, GCC uses a version number as x.y.z and below
        #  are the typical version strings seen with various distributions.
        #  gcc (GCC) 4.4.7 20120313 (Red Hat 4.4.7-23)
        #  gcc version 4.8.5 20150623 (Red Hat 4.8.5-39) (GCC)
        #  gcc (GCC) 8.3.1 20190507 (Red Hat 8.3.1-4)
        #  gcc (GCC) 10.2.1 20200723 (Red Hat 10.2.1-1)
        #  gcc (Ubuntu 9.3.0-17ubuntu1~20.04) 9.3.0
        #  gcc (Ubuntu 7.5.0-3ubuntu1~16.04) 7.5.0
        #  gcc (Debian 8.3.0-6) 8.3.0
        #  aarch64-linux-gcc.br_real (Buildroot 2020.08-14-ge5a2a90) 9.3.0, GNU ld (GNU Binutils) 2.33.1
        #
        #  In order to extract GCC version correctly for version strings
        #  like the last one above, we first check for x.y.z and if that
        #  fails, we fallback to x.y format.
        VERBOSE=$6

        kernel_compile_h=$OUTPUT/include/generated/compile.h

        if [ ! -f ${kernel_compile_h} ]; then
            # The kernel's compile.h file is not present, so there
            # isn't a convenient way to identify the compiler version
            # used to build the kernel.
            IGNORE_CC_MISMATCH=1
        fi

        if [ -n "$IGNORE_CC_MISMATCH" ]; then
            exit 0
        fi

        kernel_cc_string=`cat ${kernel_compile_h} | \
            grep LINUX_COMPILER | cut -f 2 -d '"'`

        kernel_cc_version=`echo ${kernel_cc_string} | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+' | head -n 1`
        if [ -z "${kernel_cc_version}" ]; then
            kernel_cc_version=`echo ${kernel_cc_string} | grep -o '[0-9]\+\.[0-9]\+' | head -n 1`
        fi
        kernel_cc_major=`echo ${kernel_cc_version} | cut -d '.' -f 1`
        kernel_cc_minor=`echo ${kernel_cc_version} | cut -d '.' -f 2`

        echo "
        #if (__GNUC__ != ${kernel_cc_major}) || (__GNUC_MINOR__ != ${kernel_cc_minor})
        #error \"cc version mismatch\"
        #endif
        " > conftest$$.c

        $CC $CFLAGS -c conftest$$.c > /dev/null 2>&1
        rm -f conftest$$.c

        if [ -f conftest$$.o ]; then
            rm -f conftest$$.o
            exit 0;
        else
            #
            # The gcc version check failed
            #

            if [ "$VERBOSE" = "full_output" ]; then
                echo "";
                echo "Warning: Compiler version check failed:";
                echo "";
                echo "The major and minor number of the compiler used to";
                echo "compile the kernel:";
                echo "";
                echo "${kernel_cc_string}";
                echo "";
                echo "does not match the compiler used here:";
                echo "";
                $CC --version
                echo "";
                echo "It is recommended to set the CC environment variable";
                echo "to the compiler that was used to compile the kernel.";
                echo ""
                echo "To skip the test and silence this warning message, set";
                echo "the IGNORE_CC_MISMATCH environment variable to \"1\".";
                echo "However, mixing compiler versions between the kernel";
                echo "and kernel modules can result in subtle bugs that are";
                echo "difficult to diagnose.";
                echo "";
                echo "*** Failed CC version check. ***";
                echo "";
            elif [ "$VERBOSE" = "just_msg" ]; then
                echo "Warning: The kernel was built with ${kernel_cc_string}, but the" \
                     "current compiler version is `$CC --version | head -n 1`.";
            fi
            exit 0;
        fi
    ;;

    patch_check)
        #
        # Check for any "official" patches that may have been applied and
        # construct a description table for reporting purposes.
        #
        PATCHES=""

        for PATCH in patch-*.h; do
            if [ -f $PATCH ]; then
                echo "#include \"$PATCH\""
                PATCHES="$PATCHES "`echo $PATCH | sed -s 's/patch-\(.*\)\.h/\1/'`
            fi
        done

        echo "static struct {
                const char *short_description;
                const char *description;
              } __nv_patches[] = {"
            for i in $PATCHES; do
                echo "{ \"$i\", LG_PATCH_${i}_DESCRIPTION },"
            done
        echo "{ NULL, NULL } };"

        exit 0
    ;;

    compile_tests)
        #
        # Run a series of compile tests to determine the set of interfaces
        # and features available in the target kernel.
        #
        shift 5

        CFLAGS=$1
        shift

        for i in $*; do compile_test $i; done

        exit 0
    ;;

    dom0_sanity_check)
        #
        # Determine whether running in DOM0.
        #
        VERBOSE=$6

        if [ -n "$VGX_BUILD" ]; then
            if [ -f /proc/xen/capabilities ]; then
                if [ "`cat /proc/xen/capabilities`" == "control_d" ]; then
                    exit 0
                fi
            else
                echo "The kernel is not running in DOM0.";
                echo "";
                if [ "$VERBOSE" = "full_output" ]; then
                    echo "*** Failed DOM0 sanity check. Bailing out! ***";
                    echo "";
                fi
            fi
            exit 1
        fi
    ;;
    test_configuration_option)
        #
        # Check to see if the given config option is set.
        #
        OPTION=$6

        test_configuration_option $OPTION
        exit $?
    ;;

    get_configuration_option)
        #
        # Get the value of the given config option.
        #
        OPTION=$6

        get_configuration_option $OPTION
        exit $?
    ;;


    guess_module_signing_hash)
        #
        # Determine the best cryptographic hash to use for module signing,
        # to the extent that is possible.
        #

        HASH=$(get_configuration_option CONFIG_MODULE_SIG_HASH)

        if [ $? -eq 0 ] && [ -n "$HASH" ]; then
            echo $HASH
            exit 0
        else
            for SHA in 512 384 256 224 1; do
                if test_configuration_option CONFIG_MODULE_SIG_SHA$SHA; then
                    echo sha$SHA
                    exit 0
                fi
            done
        fi
        exit 1
    ;;


    test_kernel_header)
        #
        # Check for the availability of the given kernel header
        #

        CFLAGS=$6

        test_header_presence "${7}"

        exit $?
    ;;


    build_cflags)
        #
        # Generate CFLAGS for use in the compile tests
        #

        build_cflags
        echo $CFLAGS
        exit 0
    ;;

    module_symvers_sanity_check)
        #
        # Check whether Module.symvers exists and contains at least one
        # EXPORT_SYMBOL* symbol from vmlinux
        #

        if [ -n "$IGNORE_MISSING_MODULE_SYMVERS" ]; then
            exit 0
        fi

        TAB='	'

        if [ -f "$OUTPUT/Module.symvers" ] && \
             grep -e "^[^${TAB}]*${TAB}[^${TAB}]*${TAB}\+vmlinux" \
                     "$OUTPUT/Module.symvers" >/dev/null 2>&1; then
            exit 0
        fi

        echo "The Module.symvers file is missing, or does not contain any"
        echo "symbols exported from the kernel. This could cause the LOONGGPU"
        echo "kernel modules to be built against a configuration that does"
        echo "not accurately reflect the actual target kernel."
        echo "The Module.symvers file check can be disabled by setting the"
        echo "environment variable IGNORE_MISSING_MODULE_SYMVERS to 1."

        exit 1
    ;;
esac

