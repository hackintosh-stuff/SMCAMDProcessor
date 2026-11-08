#!/usr/bin/env bash
#
# SMCAMDProcessor local/CI one-shot packaging script
#
# Flow: dependency preparation -> build 4 schemes -> cleanup -> flatten -> zip -> summary
#
# Usage: ./Scripts/build.sh [options]
# See --help for details
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# macOS zip writes a __MACOSX resource-fork directory by default; export this to disable it
export COPYFILE_DISABLE=1

PROJECT="SMCAMDProcessor.xcodeproj"
CONFIGURATION="Release"
ARCHS="x86_64"
DERIVED_DATA="$ROOT/build/DerivedData"
OUT_DIR="$ROOT/build/Release"
ZIP_FILE="$ROOT/build/Release.zip"
SKIP_DEPS=0
VSMC_REF="master"
DO_CLEAN=0
DO_ZIP=1
VERBOSE=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: ./Scripts/build.sh [options]

Options:
  --configuration <Release|Debug>  build configuration (default: Release)
  --arch <arch[,arch...]>          target architectures, comma separated (default: x86_64)
  --derived-data <dir>             DerivedData path (default: build/DerivedData)
  --out <dir>                      archive directory (default: build/Release)
  --zip <file>                     zip path (default: build/Release.zip)
  --vsmc-ref <ref>                 VirtualSMC branch/tag/commit (default: master)
  --skip-deps                      skip dependency preparation
  --clean                          wipe DerivedData and the output directory before building
  --no-zip                         only produce the archive directory, do not zip
  --verbose                        print the full xcodebuild log
  -h, --help                       show this help

Product layout (root of build/Release.zip after extraction):
  AMDRyzenCPUPowerManagement.kext/
  SMCAMDProcessor.kext/
  AMD Power Gadget.app/     (contains Contents/Library/LoginItems/APGLaunchHelper.app)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --configuration) CONFIGURATION="${2:?--configuration requires a value}"; shift 2 ;;
        --arch)          ARCHS="${2:?--arch requires a value}"; shift 2 ;;
        --derived-data)  DERIVED_DATA="${2:?--derived-data requires a value}"; shift 2 ;;
        --out)           OUT_DIR="${2:?--out requires a value}"; shift 2 ;;
        --zip)           ZIP_FILE="${2:?--zip requires a value}"; shift 2 ;;
        --vsmc-ref)      VSMC_REF="${2:?--vsmc-ref requires a value}"; shift 2 ;;
        --with-lilu)     die "--with-lilu was removed: Lilu.kext is now a required dependency" ;;
        --skip-deps)     SKIP_DEPS=1; shift ;;
        --clean)         DO_CLEAN=1; shift ;;
        --no-zip)        DO_ZIP=0; shift ;;
        --verbose)       VERBOSE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "unknown argument: $1 (see --help)" ;;
    esac
done

case "$CONFIGURATION" in
    Release|Debug) ;;
    *) die "unsupported build configuration: $CONFIGURATION" ;;
esac

# Normalize to absolute paths: later stages cd around, relative paths would resolve wrongly
abspath() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$ROOT/$1" ;;
    esac
}
DERIVED_DATA="$(abspath "$DERIVED_DATA")"
OUT_DIR="$(abspath "$OUT_DIR")"
ZIP_FILE="$(abspath "$ZIP_FILE")"

# rm -rf guard: reject empty paths and the root directory
for p in "$DERIVED_DATA" "$OUT_DIR" "$ZIP_FILE"; do
    case "$p" in
        ""|"/") die "refusing to use a dangerous path: '$p'" ;;
    esac
done

# ---------------------------------------------------------------- preflight --
command -v xcodebuild >/dev/null 2>&1 || die "xcodebuild not found, install Xcode and run xcode-select first"
command -v git        >/dev/null 2>&1 || die "git not found"
command -v ditto      >/dev/null 2>&1 || die "ditto not found"
[ -f "$ROOT/$PROJECT/project.pbxproj" ] || die "project file missing: $PROJECT"

if [ "$DO_ZIP" -eq 1 ]; then
    command -v zip >/dev/null 2>&1 || die "zip not found"
fi

# Archive architecture arguments
ARCH_ARGS=()
OLDIFS=$IFS
IFS=','
for a in $ARCHS; do
    if [ -n "$a" ]; then
        ARCH_ARGS+=("-arch" "$a")
    fi
done
IFS=$OLDIFS
[ ${#ARCH_ARGS[@]} -gt 0 ] || die "--arch must not be empty"

PRODUCTS_DIR="$DERIVED_DATA/Build/Products/$CONFIGURATION"
LOG_FILE="$ROOT/build/build.log"

# ---------------------------------------------------------------- deps prep --
# Local layout: prebuilt dependencies may live outside the repository in ../sdk
# (Lilu.kext / VirtualSMC.kext, see .gitignore). Xcode's HEADER_SEARCH_PATHS point
# at the in-repository Lilu.kext / VirtualSMC.kext, so create the symlinks here
# instead of downloading them just to "collect the dependencies".
# CI has no ../sdk, so the behaviour there is unchanged.
link_local_sdk_dep() {
    local name="$1" target="../sdk/$1"
    [ -e "$ROOT/$target" ] || return 0
    if [ -e "$ROOT/$name" ]; then
        return 0
    fi
    # A dangling symlink (target deleted) must be removed first, otherwise the
    # checks below treat it as "ready" while the file is still unreachable and the
    # flow falls back to downloading.
    if [ -L "$ROOT/$name" ]; then
        rm -f "$ROOT/$name"
    fi
    ln -s "$target" "$ROOT/$name" && log "created symlink $name -> $target"
}

prepare_mackernel_sdk() {
    if [ -f "$ROOT/MacKernelSDK/Library/universal/libkmod.a" ]; then
        log "MacKernelSDK already prepared"
        return 0
    fi
    log "initializing MacKernelSDK submodule"
    git -C "$ROOT" submodule update --init --recursive \
        || die "MacKernelSDK submodule initialization failed (network issue? run git submodule update --init --recursive manually)"
}

prepare_virtual_smc() {
    # Two acceptable layouts:
    #   a) source checkout layout (VirtualSMC/ in the repository root) - used by CI
    #   b) prebuilt kext layout (VirtualSMC.kext/Contents/Resources/VirtualSMCSDK)
    #      - used by the local layout. Only b) is needed to compile the kext:
    #      Xcode's HEADER_SEARCH_PATHS point at
    #      ${PROJECT_DIR}/VirtualSMC.kext/Contents/Resources;
    #      ${PROJECT_DIR}/VirtualSMC is only a redundant search path, a missing
    #      directory does not affect the build.
    if [ -f "$ROOT/VirtualSMC/VirtualSMCSDK/kern_vsmcapi.hpp" ]; then
        log "VirtualSMC already prepared (source layout)"
        return 0
    fi
    if [ -f "$ROOT/VirtualSMC.kext/Contents/Resources/VirtualSMCSDK/kern_vsmcapi.hpp" ]; then
        log "VirtualSMC already prepared (prebuilt kext layout)"
        return 0
    fi
    log "cloning VirtualSMC (ref: $VSMC_REF)"
    rm -rf "$ROOT/VirtualSMC"
    if ! git clone --depth 1 --branch "$VSMC_REF" \
            https://github.com/acidanthera/VirtualSMC.git "$ROOT/VirtualSMC" >/dev/null 2>&1; then
        # --branch does not accept a commit SHA, fall back to a full clone plus checkout
        warn "--branch clone failed, trying a full clone followed by checkout $VSMC_REF"
        rm -rf "$ROOT/VirtualSMC"
        git clone https://github.com/acidanthera/VirtualSMC.git "$ROOT/VirtualSMC" >/dev/null 2>&1 \
            || die "VirtualSMC clone failed, check the network or use --vsmc-ref to pick a branch/tag"
        git -C "$ROOT/VirtualSMC" checkout "$VSMC_REF" \
            || die "VirtualSMC checkout of $VSMC_REF failed"
    fi
    [ -f "$ROOT/VirtualSMC/VirtualSMCSDK/kern_vsmcapi.hpp" ] \
        || die "VirtualSMC cloned but VirtualSMCSDK/kern_vsmcapi.hpp is missing, the ref is probably incompatible"
}

prepare_lilu() {
    # Lilu.kext provides Headers/kern_efi.hpp and other kernel headers, a hard
    # requirement for building the kext. Equivalent to the default (prebuilt) mode of
    # the official bootstrap.sh, but without eval'ing a remote script.
    if [ -f "$ROOT/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ]; then
        log "Lilu.kext already prepared"
        return 0
    fi
    local tag url tmp manifest
    manifest="$(curl -Lfs https://api.github.com/repos/acidanthera/Lilu/releases/latest)" \
        || die "failed to query the latest Lilu version (GitHub API unreachable or rate limited?)"
    tag="$(printf '%s' "$manifest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')"
    [ -n "$tag" ] || die "could not parse the latest Lilu version"

    url="https://github.com/acidanthera/Lilu/releases/download/${tag}/Lilu-${tag}-DEBUG.zip"
    tmp="$(mktemp -d)" || die "failed to create a temporary directory"
    log "downloading Lilu $tag"
    curl -Lfs -o "$tmp/lilu.zip" "$url" || { rm -rf "$tmp"; die "Lilu download failed: $url"; }
    unzip -q "$tmp/lilu.zip" -d "$tmp" || { rm -rf "$tmp"; die "failed to extract the Lilu archive"; }
    [ -d "$tmp/Lilu.kext" ] || { rm -rf "$tmp"; die "Lilu.kext not found in the archive"; }

    # If Lilu.kext is a symlink to a dependency outside the repository, remove only
    # the symlink itself - rm -rf does not follow symlinks, but this distinguishes
    # the cases explicitly so ../sdk/Lilu.kext can never be deleted by accident.
    if [ -L "$ROOT/Lilu.kext" ]; then
        rm -f "$ROOT/Lilu.kext"
    else
        rm -rf "$ROOT/Lilu.kext"
    fi
    mv "$tmp/Lilu.kext" "$ROOT/Lilu.kext"
    rm -rf "$tmp"
    [ -f "$ROOT/Lilu.kext/Contents/Resources/Headers/kern_api.hpp" ] \
        || die "unexpected Lilu.kext structure: Contents/Resources/Headers/kern_api.hpp missing"
}

if [ "$SKIP_DEPS" -eq 1 ]; then
    warn "dependency preparation skipped"
else
    # Local layout: when prebuilt dependencies already exist in ../sdk, create the
    # in-repository symlinks first so the checks below succeed without downloading.
    link_local_sdk_dep Lilu.kext
    link_local_sdk_dep VirtualSMC.kext
    prepare_mackernel_sdk
    prepare_lilu
    prepare_virtual_smc
fi

# -------------------------------------------------------------------- build --
build_scheme() {
    local scheme="$1"
    log "building $scheme ($CONFIGURATION, ${ARCHS})"
    mkdir -p "$(dirname "$LOG_FILE")"
    if [ "$VERBOSE" -eq 1 ]; then
        xcodebuild -scheme "$scheme" -jobs 1 "${ARCH_ARGS[@]}" \
            -configuration "$CONFIGURATION" -derivedDataPath "$DERIVED_DATA" \
            2>&1 | tee "$LOG_FILE"
    else
        if ! xcodebuild -scheme "$scheme" -jobs 1 "${ARCH_ARGS[@]}" \
                -configuration "$CONFIGURATION" -derivedDataPath "$DERIVED_DATA" \
                >"$LOG_FILE" 2>&1; then
            printf '\033[1;31m[error]\033[0m %s build failed, tail of the log:\n' "$scheme" >&2
            tail -n 60 "$LOG_FILE" >&2
            exit 1
        fi
    fi
}

if [ "$DO_CLEAN" -eq 1 ]; then
    log "cleaning DerivedData and the output directory"
    rm -rf "$DERIVED_DATA" "$OUT_DIR" "$ZIP_FILE"
fi

# Ordering matters: APGLaunchHelper must be built before AMD Power Gadget, whose
# CopyFiles phase takes APGLaunchHelper.app from BUILT_PRODUCTS_DIR
build_scheme "AMDRyzenCPUPowerManagement"
build_scheme "APGLaunchHelper"
build_scheme "AMD Power Gadget"
build_scheme "SMCAMDProcessor"

# ------------------------------------------------------------------ cleanup --
log "cleaning intermediate products"
rm -rf "$PRODUCTS_DIR"/*.dSYM "$PRODUCTS_DIR"/*.swiftmodule
# Keep only the helper embedded in AMD Power Gadget.app, drop the standalone copy
rm -rf "$PRODUCTS_DIR/APGLaunchHelper.app"

# ------------------------------------------------------------------ archive --
log "archiving to $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

for product in "AMDRyzenCPUPowerManagement.kext" "SMCAMDProcessor.kext" "AMD Power Gadget.app"; do
    [ -e "$PRODUCTS_DIR/$product" ] || die "missing build product: $PRODUCTS_DIR/$product"
    ditto "$PRODUCTS_DIR/$product" "$OUT_DIR/$product"
done

# Verify the embedded helper
[ -d "$OUT_DIR/AMD Power Gadget.app/Contents/Library/LoginItems/APGLaunchHelper.app" ] \
    || die "APGLaunchHelper.app not found inside AMD Power Gadget.app"

if [ "$DO_ZIP" -eq 1 ]; then
    log "packaging $ZIP_FILE"
    mkdir -p "$(dirname "$ZIP_FILE")"
    rm -f "$ZIP_FILE"
    # Zip from inside the staging directory so the archive root is the products themselves
    ( cd "$OUT_DIR" && zip -r -q -X "$ZIP_FILE" . -x '*.DS_Store' ) \
        || die "packaging failed: $ZIP_FILE"
fi

# ------------------------------------------------------------------ summary --
plist_get() {
    /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || echo "N/A"
}

printf '\n'
log "product summary"
printf '  %s\n' "--------------------------------------------------------------"

for bundle in "AMDRyzenCPUPowerManagement.kext" "SMCAMDProcessor.kext"; do
    path="$OUT_DIR/$bundle"
    plist="$path/Contents/Info.plist"
    bin="$path/Contents/MacOS/${bundle%.kext}"
    printf '  %s\n' "$bundle"
    printf '    CFBundleVersion           : %s\n' "$(plist_get "$plist" CFBundleVersion)"
    printf '    CFBundleShortVersionString: %s\n' "$(plist_get "$plist" CFBundleShortVersionString)"
    printf '    architectures             : %s\n' "$(lipo -archs "$bin" 2>/dev/null || echo 'N/A')"
    printf '    signature                 : %s\n' \
        "$(codesign -dv "$path" 2>&1 | sed -n 's/^Signature=//p' | head -1)"
    printf '    dependencies              : %s\n' \
        "$(plist_get "$plist" OSBundleLibraries | tr '\n' ' ')"
done

for app in "AMD Power Gadget.app"; do
    path="$OUT_DIR/$app"
    plist="$path/Contents/Info.plist"
    printf '  %s\n' "$app"
    printf '    CFBundleVersion           : %s\n' "$(plist_get "$plist" CFBundleVersion)"
    printf '    CFBundleShortVersionString: %s\n' "$(plist_get "$plist" CFBundleShortVersionString)"
    printf '    embedded helper           : %s\n' \
        "$([ -d "$path/Contents/Library/LoginItems/APGLaunchHelper.app" ] && echo 'APGLaunchHelper.app (OK)' || echo 'missing')"
done

printf '  %s\n' "--------------------------------------------------------------"

if [ "$DO_ZIP" -eq 1 ]; then
    printf '  archive: %s (%s)\n' "$ZIP_FILE" "$(du -h "$ZIP_FILE" | cut -f1 | tr -d ' ')"
    printf '  contents:\n'
    unzip -l "$ZIP_FILE" | sed -n '4,$p' | sed 's/^/    /'
fi

printf '\n'
log "done"
