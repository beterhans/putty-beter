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
        BUILD_DIR="$SRC_DIR/build-arm64"
        ;;
    x86_64)
        BREW_PREFIX="${BREW_PREFIX:-/usr/local}"
        BUILD_DIR="$SRC_DIR/build-x86_64"
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
for b in putty puttyapp plink pscp psftp psusan puttygen pterm pageant osxlaunch; do
    if [ -x "$BUILD_DIR/$b" ]; then
        dbg "$b -> $(lipo -archs "$BUILD_DIR/$b" 2>/dev/null || echo '?')"
    else
        log "WARNING: expected binary missing: $b"
    fi
done

log "done. binaries in: $BUILD_DIR"
