#!/usr/bin/env bash
#
# build-macos.sh — configure and build PuTTY for macOS.
#
# Usage:
#   scripts/build-macos.sh [arm64|x86_64] [--clean]
#
# Environment:
#   DEBUG=1         verbose (debug) output
#   BUILD_TYPE=...  CMake build type (default: Release)
#   BREW_PREFIX=... Homebrew prefix to use for GTK/cmake/ninja/pkg-config
#   BUILD_DIR=...   override the build directory (default build-<arch>, in the
#                   repo; useful to build on a local disk when the repo lives on
#                   a slow network share)
#
# Notes:
#   * arm64  uses the native Homebrew prefix (default /opt/homebrew).
#   * x86_64 uses a Rosetta Homebrew prefix (default /usr/local) and runs the
#     whole build under `arch -x86_64`. See PLAN.md Phase 1.
#   * Cross-arch GTK is NOT available from a single Homebrew, so each arch is
#     configured/built separately; merging happens later (Phase 1c, lipo).
#
set -euo pipefail

# ---------------------------------------------------------------- debug switch
DEBUG="${DEBUG:-0}"
log() { printf '[build-macos] %s\n' "$*" >&2; }
dbg() { [ "$DEBUG" = "1" ] && printf '[build-macos][debug] %s\n' "$*" >&2; return 0; }

# ---------------------------------------------------------------------- args
ARCH="arm64"
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        arm64|x86_64) ARCH="$arg" ;;
        --clean)      CLEAN=1 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *)            echo "build-macos.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

BUILD_TYPE="${BUILD_TYPE:-Release}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$ARCH" in
    arm64)
        BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"
        : "${BUILD_DIR:=$SRC_DIR/build-arm64}"
        ;;
    x86_64)
        BREW_PREFIX="${BREW_PREFIX:-/usr/local}"
        : "${BUILD_DIR:=$SRC_DIR/build-x86_64}"
        ;;
esac

log "source     : $SRC_DIR"
log "arch       : $ARCH"
log "build type : $BUILD_TYPE"
log "brew prefix: $BREW_PREFIX"
log "build dir  : $BUILD_DIR"

# ------------------------------------------------- cross-arch execution wrapper
# On Apple Silicon, building x86_64 needs the whole toolchain run under Rosetta
# so that the x86_64 Homebrew headers/libs are the ones that get used.
WRAP=()
if [ "$ARCH" = "x86_64" ] && [ "$(uname -m)" = "arm64" ]; then
    log "host is arm64 -> running x86_64 build under 'arch -x86_64'"
    WRAP=(arch -x86_64)
fi

if [ "$ARCH" = "x86_64" ]; then
    export PATH="$BREW_PREFIX/bin:$PATH"
    export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:$BREW_PREFIX/opt/gtk+3/lib/pkgconfig"
fi

# ------------------------------------------------- Intel toolchain preflight
# Cross-building x86_64 on Apple Silicon needs Rosetta *and* an x86_64 toolchain
# at $BREW_PREFIX/bin. Without them cmake dies with the very unhelpful
#   "arch: posix_spawnp: cmake: Bad CPU type in executable"
# because PATH just found the arm64 cmake. Say what is actually missing.
if [ "$ARCH" = "x86_64" ] && [ "$(uname -m)" = "arm64" ]; then
    if ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        log "ERROR: Rosetta 2 is not installed, so x86_64 tools cannot run here."
        log "       Install it with:"
        log "         softwareupdate --install-rosetta --agree-to-license"
        log "       (or run this on an Intel Mac, which needs no Rosetta)"
        exit 1
    fi
    if ! arch -x86_64 "$BREW_PREFIX/bin/cmake" --version >/dev/null 2>&1; then
        log "ERROR: no working x86_64 cmake at $BREW_PREFIX/bin/cmake."
        if [ "$BREW_PREFIX" = "/opt/homebrew" ]; then
            log "       /opt/homebrew is this Mac's own (arm64) Homebrew; a"
            log "       cross-build needs the Intel one, which conventionally"
            log "       lives at /usr/local. Run this on an Intel Mac instead,"
            log "       or point BREW_PREFIX at an Intel Homebrew."
        else
            log "       '$BREW_PREFIX' looks like a directory, but it is not an"
            log "       Intel Homebrew. Either install the Intel toolchain:"
            log "         arch -x86_64 /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
            log "         arch -x86_64 $BREW_PREFIX/bin/brew install gtk+3 cmake ninja pkgconf"
            log "       or do the Intel build on an Intel Mac, where it is native."
        fi
        exit 1
    fi
    GTK_VER_X86="$(arch -x86_64 pkg-config --modversion gtk+-3.0 2>/dev/null || true)"
    if [ -z "$GTK_VER_X86" ]; then
        log "ERROR: gtk+-3.0 not found by the x86_64 pkg-config (PKG_CONFIG_PATH=$PKG_CONFIG_PATH)"
        log "       arch -x86_64 $BREW_PREFIX/bin/brew install gtk+3"
        exit 1
    fi
    log "x86_64 gtk : $GTK_VER_X86"
fi

# ------------------------------------------------------------------- preflight
for tool in cmake ninja pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        log "ERROR: '$tool' not found on PATH (prefix: $BREW_PREFIX)"
        exit 1
    fi
    dbg "$tool -> $(command -v "$tool")"
done

if [ "$ARCH" = "x86_64" ] && ! [ -d "$BREW_PREFIX" ]; then
    log "ERROR: x86_64 Homebrew prefix '$BREW_PREFIX' does not exist."
    log "       Install it with:"
    log "         arch -x86_64 /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    log "         arch -x86_64 $BREW_PREFIX/bin/brew install gtk+3 cmake ninja pkgconf"
    exit 1
fi

GTK_VER="$(pkg-config --modversion gtk+-3.0 2>/dev/null || true)"
if [ -z "$GTK_VER" ]; then
    log "ERROR: gtk+-3.0 not found via pkg-config (PKG_CONFIG_PATH=$PKG_CONFIG_PATH)"
    exit 1
fi
log "gtk+-3.0   : $GTK_VER"

# --------------------------------------------------------------------- clean
if [ "$CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    log "removing build dir (--clean): $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# --------------------------------------------------- stale build-dir check
# A CMake build tree records the absolute path of the source directory it was
# configured from, so a build dir copied or synced from another machine (or from
# another clone) cannot be reused: cmake refuses with
#   "The current CMakeCache.txt directory ... is different than the directory
#    ... where CMakeCache.txt was created."
# which does not say what to do about it. Say so plainly instead.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cached_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
                  "$BUILD_DIR/CMakeCache.txt" | head -1)"
    if [ -n "$cached_src" ] && [ "$cached_src" != "$SRC_DIR" ]; then
        log "ERROR: $BUILD_DIR was configured from a different source tree:"
        log "         configured for: $cached_src"
        log "         this checkout : $SRC_DIR"
        log "       cmake will refuse to reuse it (this happens when the repo or"
        log "       its build dir is copied between machines)."
        log "       Rebuild with: scripts/build-macos.sh $ARCH --clean"
        exit 1
    fi
fi

# ----------------------------------------------------------------- configure
log "configuring..."
"${WRAP[@]+"${WRAP[@]}"}" cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH"

# --------------------------------------------------------------------- build
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
log "building with $JOBS jobs..."
"${WRAP[@]+"${WRAP[@]}"}" cmake --build "$BUILD_DIR" -j"$JOBS"

# ------------------------------------------------------------------- verify
log "verifying binaries..."
for b in putty puttyapp plink pscp psftp psusan puttygen puttygenapp pterm pageant osxlaunch; do
    if [ -x "$BUILD_DIR/$b" ]; then
        dbg "$b -> $(lipo -archs "$BUILD_DIR/$b" 2>/dev/null || echo '?')"
    else
        log "WARNING: expected binary missing: $b"
    fi
done

log "done. binaries in: $BUILD_DIR"
