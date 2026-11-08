#!/bin/sh
# build-noxcode.sh — build the two kexts using only the Command Line Tools
#
# Usage (run on macOS; neither Xcode.app nor sudo is required):
#   sh Scripts/build-noxcode.sh [options]
#   MACKERNELSDK=/path/to/MacKernelSDK LILU=/path/to/Lilu.kext sh Scripts/build-noxcode.sh
#
# Why xcodebuild is not used:
#   A machine that only has the Command Line Tools cannot build through the project
#   (/usr/bin/xcodebuild is a stub and every -project invocation fails with "requires
#   Xcode"). Building a kext only needs clang, the MacKernelSDK kernel headers,
#   libkmod/libkmodc++, and ld -kext, all of which the CLT provides.
#
# What this script does NOT build:
#   AMD Power Gadget and APGLaunchHelper are Swift/AppKit products and still require
#   Xcode; use Scripts/build.sh for those. Only the two kexts are produced here.
#
# Dependencies (not vendored in the repository):
#   MacKernelSDK  git submodule, initialise with:
#                 git submodule update --init MacKernelSDK
#   Lilu.kext     for Headers/kern_*.hpp (both kexts include them)
#   VirtualSMC    for VirtualSMCSDK/kern_vsmcapi.hpp (SMCAMDProcessor.kext only);
#                 either VirtualSMC.kext/Contents/Resources or a VirtualSMC source tree
#
# Output: build/noxcode.XXXXXX/ containing AMDRyzenCPUPowerManagement.kext and
#         SMCAMDProcessor.kext. Every run gets its own directory (older outputs are
#         kept); the path of the newest build is written to build/latest-noxcode-build.txt
# Note: this script only builds. It does not deploy anything, does not touch
#       /Library/Extensions and does not rebuild the kernel collections.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PROJ="$ROOT/SMCAMDProcessor.xcodeproj/project.pbxproj"

SDK_ARG=${MACKERNELSDK:-}
LILU_ARG=${LILU:-}
VSMC_ARG=${VIRTUALSMC:-}
OUT_ARG=
ONLY=all
CONFIG=Release
OPT=-O3

usage() {
    cat <<'EOF'
Usage: sh Scripts/build-noxcode.sh [options]

Options:
  --sdk <dir>       MacKernelSDK root (default: <repo>/MacKernelSDK, else $MACKERNELSDK)
  --lilu <dir>      Lilu.kext directory (default: <repo>/Lilu.kext, ../sdk/Lilu.kext, $LILU)
  --vsmc <path>     VirtualSMC.kext or a VirtualSMC source tree
                    (default: <repo>/VirtualSMC.kext, ../sdk/VirtualSMC.kext,
                     <repo>/VirtualSMC, $VIRTUALSMC)
  --only <which>    amd | smc | all (default: all)
  --debug           Debug configuration: -O0 and -DDEBUG=1
  --out <dir>       output directory (default: build/noxcode.XXXXXX, kept per run)
  -h, --help        show this help

Environment:
  MACKERNELSDK, LILU, VIRTUALSMC are used when the matching option is not given.

The kext version comes from SMCAMDProcessor.xcodeproj/project.pbxproj
(AMDRYZEN_KEXT_VERSION for the power management kext, CURRENT_PROJECT_VERSION for
the SMC plugin), so this script and the Xcode build stay in sync.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk)    SDK_ARG="${2:?--sdk requires a value}"; shift 2 ;;
        --lilu)   LILU_ARG="${2:?--lilu requires a value}"; shift 2 ;;
        --vsmc)   VSMC_ARG="${2:?--vsmc requires a value}"; shift 2 ;;
        --out)    OUT_ARG="${2:?--out requires a value}"; shift 2 ;;
        --only)   ONLY="${2:?--only requires a value}"; shift 2 ;;
        --debug)  CONFIG=Debug; OPT=-O0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
done

case "$ONLY" in
    amd|smc|all) ;;
    *) echo "--only must be amd, smc or all" >&2; exit 1 ;;
esac

[ "$(uname -s)" = "Darwin" ] || { echo "This script can only run on macOS" >&2; exit 1; }

die() { echo "[error] $*" >&2; exit 1; }

# --------------------------------------------------------------- versions --
# Read straight from the project so a version bump only happens in one place.
proj_cf_version() { # $1 = MODULE_NAME value
    awk -v target="$1" '
        /CURRENT_PROJECT_VERSION = /{ v = $3; gsub(/[";]/, "", v); cpv = v }
        $0 ~ ("MODULE_NAME = " target ";"){ print cpv; exit }
    ' "$PROJ"
}

proj_kext_version() {
    awk '
        /AMDRYZEN_KEXT_VERSION = /{ v = $3; gsub(/[";]/, "", v); print v; exit }
    ' "$PROJ"
}

[ -f "$PROJ" ] || die "project file not found: $PROJ"

AMD_VER=$(proj_kext_version)
SMC_VER=$(proj_cf_version wtf.spinach.SMCAMDProcessor)
[ -n "$AMD_VER" ] || die "could not read AMDRYZEN_KEXT_VERSION from the project"
[ -n "$SMC_VER" ] || die "could not read the SMCAMDProcessor kext version from the project"

# ------------------------------------------------------------------- deps --
find_sdk() {
    for cand in "$SDK_ARG" "$ROOT/MacKernelSDK" "$HOME/MacKernelSDK" "$HOME/src/MacKernelSDK"; do
        [ -n "$cand" ] || continue
        if [ -d "$cand/Headers/IOKit" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

find_lilu() {
    for cand in "$LILU_ARG" "$ROOT/Lilu.kext" "$ROOT/../sdk/Lilu.kext" "$HOME/Lilu.kext"; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/Contents/Resources/Headers/kern_util.hpp" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

# Prints the directory that contains VirtualSMCSDK/kern_vsmcapi.hpp.
find_vsmc() {
    for cand in "$VSMC_ARG" "$ROOT/VirtualSMC.kext" "$ROOT/../sdk/VirtualSMC.kext" "$ROOT/VirtualSMC" "$HOME/VirtualSMC"; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/VirtualSMCSDK/kern_vsmcapi.hpp" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
        if [ -f "$cand/Contents/Resources/VirtualSMCSDK/kern_vsmcapi.hpp" ]; then
            printf '%s\n' "$cand/Contents/Resources"
            return 0
        fi
    done
    return 0
}

SDK=$(find_sdk)
[ -n "$SDK" ] || die "MacKernelSDK not found. Initialise the submodule (git submodule update --init MacKernelSDK) or pass --sdk <dir>"
SDK=$(CDPATH= cd -- "$SDK" && pwd)
[ -f "$SDK/Library/universal/libkmodc++.a" ] || die "incomplete MacKernelSDK, missing Library/universal/libkmodc++.a: $SDK"
[ -f "$SDK/Library/x86_64/libkmod.a" ] || die "incomplete MacKernelSDK, missing Library/x86_64/libkmod.a: $SDK"

LILU=$(find_lilu)
if [ -z "$LILU" ]; then
    die "Lilu.kext not found. Both kexts include Headers/kern_*.hpp and Headers/plugin_start.hpp from it.
       Place it at \$REPO/Lilu.kext (or ../sdk/Lilu.kext), or pass --lilu <dir>."
fi
LILU=$(CDPATH= cd -- "$LILU" && pwd)

VSMC_INC=
if [ "$ONLY" != "amd" ]; then
    VSMC_INC=$(find_vsmc)
    if [ -z "$VSMC_INC" ]; then
        die "VirtualSMCSDK not found. SMCAMDProcessor.kext includes <VirtualSMCSDK/kern_vsmcapi.hpp>.
       Place VirtualSMC.kext at \$REPO/VirtualSMC.kext (or ../sdk/VirtualSMC.kext), or pass
       --vsmc <VirtualSMC.kext | source tree>, or build only the power management kext with --only amd."
    fi
    VSMC_INC=$(CDPATH= cd -- "$VSMC_INC" && pwd)
fi

echo "== Environment =="
echo "config : $CONFIG ($OPT)"
echo "repo   : $ROOT"
xcrun clang --version | head -1
echo "sdk    : $SDK"
echo "lilu   : $LILU"
if [ -n "$VSMC_INC" ]; then
    echo "vsmc   : $VSMC_INC"
fi
echo "version: AMDRyzenCPUPowerManagement $AMD_VER, SMCAMDProcessor $SMC_VER"

# ---------------------------------------------------------------- work dir --
mkdir -p "$ROOT/build"
if [ -n "$OUT_ARG" ]; then
    W="$OUT_ARG"
    mkdir -p "$W"
else
    W=$(mktemp -d "$ROOT/build/noxcode.XXXXXX")
fi
W=$(CDPATH= cd -- "$W" && pwd)
echo "work   : $W"

# ------------------------------------------------------------------ flags --
# Mirrors the kext targets in project.pbxproj (x86_64, kernel mode, -O3 in Release
# and the SSE/vectoriser flags listed there). -fno-non-call-exceptions from the
# project is left out: clang does not implement it.
CFLAGS_COMMON="-arch x86_64 -mkernel -fapple-kext -mno-red-zone
 -nostdinc++ -fno-builtin -fno-common
 -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -ftree-vectorize
 -fstack-protector -fno-threadsafe-statics -fno-asynchronous-unwind-tables
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -D__APPLE_KEXT__
 $OPT -Wno-unused-parameter -Wno-unused-function -Wno-deprecated-declarations"
CFLAGS_CXX="-std=gnu++17 -fno-exceptions -fno-rtti"
CFLAGS_C="-std=gnu11"

# ------------------------------------------------------------------- kmod --
# xcodebuild normally runs CreateKModInfo.perl to emit the translation unit that
# carries kmod_info. Bypassing Xcode means providing it here, otherwise the binary
# has no _kmod_info and the kernel rejects the kext.
#   _start/_stop come from libkmod and run the C++ runtime initialisation (static
#   OSMetaClass registrations); _realmain/_antimain point at the MODULE_START /
#   MODULE_STOP functions when the target defines them, otherwise they are null.
gen_kmod_info() { # $1=file $2=bundle id $3=version $4=module start sym|"" $5=module stop sym|""
    _start_sym=0
    _stop_sym=0
    if [ -n "$4" ]; then _start_sym="(kmod_start_func_t *)&$4"; fi
    if [ -n "$5" ]; then _stop_sym="(kmod_stop_func_t *)&$5"; fi

    cat > "$1" <<KMOD_EOF
#include <mach/kmod.h>
#include <libkern/OSKextLib.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL($2, "$3", _start, _stop);

__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = $_start_sym;
__attribute__((visibility("hidden"))) kmod_stop_func_t  *_antimain = $_stop_sym;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
KMOD_EOF
}

# --------------------------------------------------------------- building --
have_defined_symbol() {
    nm -g "$1" 2>/dev/null | grep -Eq "^[0-9a-fA-F]+ [A-Za-z] $2\$"
}

expand_plist() { # $1=source plist $2=destination $3=kext name $4=bundle id $5=version
    sed -e "s/\$(DEVELOPMENT_LANGUAGE)/en/g" \
        -e "s/\$(EXECUTABLE_NAME)/$3/g" \
        -e "s/\$(TARGET_NAME)/$3/g" \
        -e "s/\$(PRODUCT_NAME)/$3/g" \
        -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$4/g" \
        -e "s/\$(PRODUCT_BUNDLE_PACKAGE_TYPE)/KEXT/g" \
        -e "s/\$(MARKETING_VERSION)/$5/g" \
        -e "s/\$(CURRENT_PROJECT_VERSION)/$5/g" \
        "$1" > "$2"
    if grep -E '\$\([A-Za-z_]+\)' "$2" >/dev/null; then
        echo "[error] unexpanded Xcode variable in $2:" >&2
        grep -E '\$\([A-Za-z_]+\)' "$2" >&2
        exit 1
    fi
    plutil -lint "$2" >/dev/null || die "Info.plist is not a valid property list: $2"
}

build_kext() { # $1=name $2=bundle $3=version $4=modstart $5=modstop $6=srcdir $7=sources $8=includes $9=defines
    KNAME=$1; KBUNDLE=$2; KVER=$3; KMODSTART=$4; KMODSTOP=$5
    KSRC=$6; KSOURCES=$7; KINC=$8; KDEFS=$9

    KEXT="$W/$KNAME.kext"
    OBJ="$W/obj-$KNAME"
    BIN="$KEXT/Contents/MacOS/$KNAME"

    echo
    echo "== $KNAME $KVER =="
    [ -d "$KSRC" ] || die "source directory not found: $KSRC"
    for name in $KSOURCES; do
        [ -f "$KSRC/$name" ] || die "missing source file: $KSRC/$name"
    done

    mkdir -p "$OBJ" "$KEXT/Contents/MacOS"

    # Match PBXSourcesBuildPhase: an explicit list, no directory globs, so an
    # unrelated file dropped into the tree never ends up in the kext.
    for name in $KSOURCES; do
        o="$OBJ/$(echo "$name" | tr '/.' '__').o"
        case "$name" in
            *.c)  echo "  CC  $name"
                  # shellcheck disable=SC2086
                  xcrun clang $CFLAGS_COMMON $CFLAGS_C $KDEFS $KINC -I "$KSRC" -c "$KSRC/$name" -o "$o" ;;
            *.cpp|*.cc) echo "  CXX $name"
                  # shellcheck disable=SC2086
                  xcrun clang++ $CFLAGS_COMMON $CFLAGS_CXX $KDEFS $KINC -I "$KSRC" -c "$KSRC/$name" -o "$o" ;;
            *) die "unexpected source type: $name" ;;
        esac
    done

    gen_kmod_info "$OBJ/kmod-info.cpp" "$KBUNDLE" "$KVER" "$KMODSTART" "$KMODSTOP"
    echo "  CXX kmod-info.cpp"
    # shellcheck disable=SC2086
    xcrun clang++ $CFLAGS_COMMON $CFLAGS_CXX -I "$SDK/Headers" -c "$OBJ/kmod-info.cpp" -o "$OBJ/kmod-info.o"

    echo "  LD  $KNAME"
    # -kext permits undefined symbols (resolved by the kernel at load time against
    # the kernel and the already loaded kexts), so driver-internal symbols are
    # checked separately below.
    xcrun ld -kext -arch x86_64 "$OBJ"/*.o \
        "$SDK/Library/universal/libkmodc++.a" \
        "$SDK/Library/x86_64/libkmod.a" \
        -o "$BIN"

    # ------------------------------------------------------- verification --
    have_defined_symbol "$BIN" _kmod_info || die "$KNAME: _kmod_info is missing, the kext cannot be loaded"
    have_defined_symbol "$BIN" _start     || die "$KNAME: _start is missing (libkmod did not provide it)"
    have_defined_symbol "$BIN" _stop      || die "$KNAME: _stop is missing (libkmod did not provide it)"

    FILETYPE=$(otool -h "$BIN" | awk 'NR>1{print $5}' | tr -d ' ' | tail -1)
    [ "$FILETYPE" = "11" ] || die "$KNAME: filetype=$FILETYPE, expected 11 (MH_KEXT_BUNDLE)"

    expand_plist "$KSRC/Info.plist" "$KEXT/Contents/Info.plist" "$KNAME" "$KBUNDLE" "$KVER"

    codesign --force --sign - --timestamp=none "$KEXT" >/dev/null \
        || die "$KNAME: ad-hoc signing failed"
    codesign --verify --strict "$KEXT" || die "$KNAME: signature verification failed"

    echo "  ok  $KEXT"
    BUILT="$BUILT
$KEXT"
}

# Unresolved symbols that belong to the kext's own sources mean a source file was
# left out of the list above. Symbols from the kernel or from another kext are
# expected and stay unresolved on purpose.
check_internal_symbols() { # $1=kext $2=regexp of own namespaces
    BIN="$1/Contents/MacOS/$(basename "$1" .kext)"
    if nm -u "$BIN" | grep -E "$2" > "$W/unresolved-$(basename "$1" .kext).txt"; then
        echo "[error] $(basename "$1"): unresolved symbols from its own sources" >&2
        cat "$W/unresolved-$(basename "$1" .kext).txt" >&2
        exit 1
    fi
    echo "  ok  no unresolved driver-internal symbols"
}

BUILT=

if [ "$ONLY" = "all" ] || [ "$ONLY" = "amd" ]; then
    build_kext AMDRyzenCPUPowerManagement \
        wtf.spinach.AMDRyzenCPUPowerManagement \
        "$AMD_VER" \
        amdryzencpupm_kern_start amdryzencpupm_kern_stop \
        "$ROOT/AMDRyzenCPUPowerManagement" \
        "AMDRyzenCPUPowerManagement.cpp AMDRyzenCPUPMUserClient.cpp pmAMDRyzen.c symresolver/kernel_resolver.c SuperIO/ISSuperIOIT86XXEFamily.cpp SuperIO/ISSuperIONCT668X.cpp SuperIO/ISSuperIONCT67XXFamily.cpp" \
        "-I $SDK/Headers -I $LILU/Contents/Resources" \
        "-DPRODUCT_NAME=AMDRyzenCPUPowerManagement -DMODULE_VERSION=$AMD_VER -DPMRYZEN_IDLE_IO_CSTATE"
    check_internal_symbols "$W/AMDRyzenCPUPowerManagement.kext" \
        'AMDRyzenCPUPowerManagement::|AMDRyzenCPUPMUserClient::|pmRyzen_|ISSuperIO|ISLPCPort|kernel_resolver|_ksym_'
fi

if [ "$ONLY" = "all" ] || [ "$ONLY" = "smc" ]; then
    build_kext SMCAMDProcessor \
        wtf.spinach.SMCAMDProcessor \
        "$SMC_VER" \
        "" "" \
        "$ROOT/SMCAMDProcessor" \
        "SMCAMDProcessor.cpp Keyimplementations.cpp" \
        "-I $SDK/Headers -I $SDK -I $LILU/Contents/Resources -I $VSMC_INC -I $ROOT/AMDRyzenCPUPowerManagement" \
        ""
    # AMDRyzenCPUPowerManagement::* stays unresolved by design: this kext has no
    # code of that class, it resolves it against the already loaded kext.
    check_internal_symbols "$W/SMCAMDProcessor.kext" \
        'SMCAMDProcessor::|TempPackage::|TempCore::|EnergyPackage::|ClockCore::'
fi

# ------------------------------------------------------------------ summary --
mkdir -p "$ROOT/build"

echo
echo "== Summary =="
for k in $BUILT; do
    echo "  $(basename "$k")"
    echo "    CFBundleVersion           : $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$k/Contents/Info.plist")"
    echo "    CFBundleShortVersionString: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$k/Contents/Info.plist")"
    echo "    architectures             : $(lipo -archs "$k/Contents/MacOS/$(basename "$k" .kext)")"
done

printf '%s\n' "$W" > "$ROOT/build/latest-noxcode-build.txt"
echo
echo "BUILD OK: $W"
echo "Latest build path recorded in build/latest-noxcode-build.txt"
echo "Nothing was deployed: /Library/Extensions and the kernel collections are untouched."
