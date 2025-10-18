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

        platform_driver_remove_returns_void)
            #
            # Determine if platform_driver_remove returns void
            #
            CODE="
            #include <linux/platform_device.h>
            void test_platform_driver_remove(struct platform_driver *driver) {
                    return driver->remove(NULL);
            }
            "

            compile_check_conftest "$CODE" "LG_PLATFORM_DRIVER_REMOVE_RETURNS_VOID" "" "types"
        
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

