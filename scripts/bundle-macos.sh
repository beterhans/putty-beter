#!/usr/bin/env bash
#
# bundle-macos.sh — build a self-contained PuTTY.app from an existing build dir.
#
# Usage:
#   scripts/bundle-macos.sh [arm64|x86_64]
#
# Environment:
#   DEBUG=1                 verbose (debug) output
#   BREW_PREFIX=...         Homebrew prefix (default /opt/homebrew for arm64)
#   BUNDLER_DIR=...         path to the gtk-mac-bundler checkout
#   PYTHON=...              python interpreter for gtk-mac-bundler (default python3)
#
# What it does:
#   1. makes sure the build + icon exist
#   2. stages unix/putty.bundle + putty.plist + osxlaunch + puttyapp + PuTTY.icns
#   3. runs gtk-mac-bundler to produce PuTTY.app
#   4. normalises Homebrew's split "opt/" vs "Cellar/<f>/<ver>/" library paths so
#      only one copy of each library ends up in the bundle (otherwise GLib/GTK
#      get loaded twice and Objective-C classes collide)
#   5. ad-hoc re-signs every Mach-O (install_name_tool invalidates signatures)
#   6. checks for dangling @executable_path references
#
# See PLAN.md Phase 1.
#
set -euo pipefail

# ---------------------------------------------------------------- debug switch
DEBUG="${DEBUG:-0}"
log() { printf '[bundle-macos] %s\n' "$*" >&2; }
dbg() { [ "$DEBUG" = "1" ] && printf '[bundle-macos][debug] %s\n' "$*" >&2; return 0; }

# ---------------------------------------------------------------------- args
ARCH="arm64"
for arg in "$@"; do
    case "$arg" in
        arm64|x86_64) ARCH="$arg" ;;
        -h|--help)    sed -n '2,25p' "$0"; exit 0 ;;
        *)            echo "bundle-macos.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------------------------------------------------------------------
# gtk-mac-bundler  (REQUIRED - not part of this project)
#
# This script needs the `gtk-mac-bundler` tool to assemble PuTTY.app. That tool
# is what copies GTK and all its dependencies into the .app and rewrites their
# install names to point inside the bundle. It is a separate project from GNOME
# and is licensed GPL-2.0, so it is deliberately NOT vendored into this
# MIT-licensed source tree - you have to provide a checkout of it.
#
# It is looked for in this order:
#   1. $BUNDLER_DIR                       - explicit override
#   2. $SRC_DIR/tools/gtk-mac-bundler     - in-project checkout (preferred)
#   3. $HOME/gtk-mac-bundler              - global/fallback location
#
# To fetch it into the project:
#   git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git \
#       tools/gtk-mac-bundler
# ---------------------------------------------------------------------------
BUNDLER_DIR="${BUNDLER_DIR:-}"
if [ -z "$BUNDLER_DIR" ]; then
    for _candidate in "$SRC_DIR/tools/gtk-mac-bundler" "$HOME/gtk-mac-bundler"; do
        if [ -f "$_candidate/bundler/main.py" ]; then
            BUNDLER_DIR="$_candidate"
            break
        fi
    done
fi
# Default to the in-project path, so the error message below names the
# location we would prefer you to use.
BUNDLER_DIR="${BUNDLER_DIR:-$SRC_DIR/tools/gtk-mac-bundler}"
PYTHON="${PYTHON:-python3}"

case "$ARCH" in
    arm64)  BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"; BUILD_DIR="$SRC_DIR/build-arm64" ;;
    x86_64) BREW_PREFIX="${BREW_PREFIX:-/usr/local}";    BUILD_DIR="$SRC_DIR/build-x86_64" ;;
esac

STAGE="$BUILD_DIR/bundle-stage"
APP="$STAGE/unix/PuTTY.app"
RES="Contents/Resources"

log "arch       : $ARCH"
log "build dir  : $BUILD_DIR"
log "brew prefix: $BREW_PREFIX"
log "bundler dir: $BUNDLER_DIR"

# ------------------------------------------------------------------- preflight
for f in "$BUILD_DIR/puttyapp" "$BUILD_DIR/osxlaunch"; do
    [ -x "$f" ] || { log "ERROR: missing binary: $f (run scripts/build-macos.sh first)"; exit 1; }
done
[ -f "$BUNDLER_DIR/bundler/main.py" ] || {
    log "ERROR: gtk-mac-bundler not found."
    log "       Looked in:"
    log "         \$BUNDLER_DIR (unset or invalid)"
    log "         $SRC_DIR/tools/gtk-mac-bundler"
    log "         \$HOME/gtk-mac-bundler"
    log
    log "       gtk-mac-bundler is required to assemble the .app. Fetch it with:"
    log "         git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git"
    log "                   $SRC_DIR/tools/gtk-mac-bundler"
    log "       or set BUNDLER_DIR to an existing checkout."
    exit 1
}
[ -f "$BREW_PREFIX/share/icons/Adwaita/index.theme" ] || {
    log "ERROR: Adwaita icon theme missing. Install it with:"
    log "         brew install adwaita-icon-theme"
    exit 1
}

# ----------------------------------------------------------------------- icon
#
# PuTTY-beter: the app icon is icons/putty-beter-icon.png, converted to .icns
# here with scripts/make-icns.sh (sips + iconutil). The upstream icon set in
# icons/ - mkicon.py and `make -C icons PuTTY.icns` - is kept for reference and
# is still used as a fallback if the PNG is missing.
ICON_PNG="$SRC_DIR/icons/putty-beter-icon.png"
ICON_ICNS="$SRC_DIR/icons/PuTTY.icns"

if [ -f "$ICON_PNG" ]; then
    if [ ! -f "$ICON_ICNS" ] || [ "$ICON_PNG" -nt "$ICON_ICNS" ]; then
        log "converting icons/putty-beter-icon.png -> icons/PuTTY.icns"
        "$SRC_DIR/scripts/make-icns.sh" "$ICON_PNG" "$ICON_ICNS"
    else
        log "icons/PuTTY.icns is up to date"
    fi
elif [ ! -f "$ICON_ICNS" ]; then
    log "generating icons/PuTTY.icns from the upstream icon set"
    make -C "$SRC_DIR/icons" PuTTY.icns >/dev/null
fi

# ---------------------------------------------------------------------- stage
log "staging in $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/unix" "$STAGE/icons"
cp "$SRC_DIR/unix/putty.bundle" "$SRC_DIR/unix/putty.plist" "$STAGE/unix/"
cp "$BUILD_DIR/puttyapp" "$BUILD_DIR/osxlaunch" "$STAGE/"
cp "$SRC_DIR/icons/PuTTY.icns" "$STAGE/icons/"

# -------------------------------------------------------------------- bundler
log "running gtk-mac-bundler"
( cd "$STAGE/unix" && \
  PUTTY_GTK_PREFIX_FROM_MAKEFILE="$BREW_PREFIX" \
  "$PYTHON" -c "import sys; sys.path.insert(0, '$BUNDLER_DIR'); import bundler.main; bundler.main.main(['putty.bundle'])" \
) >"$STAGE/bundler.log" 2>&1 || { log "ERROR: bundler failed, see $STAGE/bundler.log"; tail -20 "$STAGE/bundler.log" >&2; exit 1; }

[ -d "$APP" ] || { log "ERROR: bundler produced no app at $APP"; exit 1; }
dbg "bundle size: $(du -sh "$APP" | cut -f1)"

# ------------------------------------------------- normalise opt/ vs Cellar/
# Homebrew's libraries reference each other through two different paths:
# /opt/homebrew/opt/<formula>/...  and  /opt/homebrew/Cellar/<formula>/<ver>/...
# gtk-mac-bundler copies both, so two copies of the same dylib get loaded at
# runtime, which produces "Class X is implemented in both ..." warnings and
# breaks GObject/GtkApplication. Rewrite every Cellar reference to its opt
# equivalent, then drop the duplicate Cellar tree.
log "normalising Cellar -> opt references"
cd "$APP"
find "$RES" -type f > "$STAGE/macho-list.txt"
while IFS= read -r f; do
    file "$f" | grep -q 'Mach-O' || continue
    dbg "normalising: $f"
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep 'Cellar' | while IFS= read -r old; do
        new="$(printf '%s' "$old" | sed -E 's#/Cellar/([^/]+)/[^/]+/#/opt/\1/#')"
        dbg "  change: $old -> $new"
        install_name_tool -change "$old" "$new" "$f" 2>/dev/null || true
    done || true
    own="$(otool -D "$f" 2>/dev/null | tail -n +2 | head -1)"
    case "$own" in
        *Cellar*)
            newown="$(printf '%s' "$own" | sed -E 's#/Cellar/([^/]+)/[^/]+/#/opt/\1/#')"
            dbg "  id: $own -> $newown"
            install_name_tool -id "$newown" "$f" 2>/dev/null || true
            ;;
    esac
done < "$STAGE/macho-list.txt"
rm -rf "$RES/Cellar"
log "duplicate Cellar tree removed"

# ------------------------------------------ resolve remaining dangling refs
# Some Homebrew libraries reference each other through the "flat" symlink
# directory (/opt/homebrew/lib/x.dylib) while others use
# /opt/homebrew/opt/<formula>/lib/x.dylib. gtk-mac-bundler copies the file only
# once, at whichever path it discovered first, so a reference can end up
# pointing at a location that does not exist in the bundle. (This is what
# prevented gdk-pixbuf's loaders.cache from being generated: the loaders could
# not dlopen libgdk_pixbuf.) Point any such reference at the copy that does
# exist, matching by basename and only when the match is unambiguous.
log "resolving dangling @executable_path references"
find "$RES" -name '*.dylib' | sed 's#^Contents/##' > "$STAGE/dylib-index.txt"
fixed=0
while IFS= read -r f; do
    file "$f" | grep -q 'Mach-O' || continue
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@executable_path' | while IFS= read -r ref; do
        rel="${ref#@executable_path/}"
        [ -e "Contents/MacOS/$rel" ] && continue
        base="$(basename "$ref")"
        matches="$(awk -v b="$base" 'substr($0, length($0)-length(b)+1) == b' "$STAGE/dylib-index.txt")"
        n="$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l | tr -d ' ')"
        if [ "$n" -eq 1 ]; then
            dbg "  fix: $ref -> @executable_path/../$matches"
            install_name_tool -change "$ref" "@executable_path/../$matches" "$f" 2>/dev/null || true
        elif [ "$n" -eq 0 ]; then
            log "  WARNING: unresolved reference $ref (in $f)"
        else
            log "  WARNING: ambiguous reference $ref ($n candidates, in $f)"
        fi
    done || true
done < "$STAGE/macho-list.txt"
log "dangling reference pass complete"

# ------------------------------------- regenerate the gdk-pixbuf loaders cache
# gtk-mac-bundler writes loaders.cache while it is bundling, but at that point
# the loader modules still referenced a library path that did not yet exist in
# the bundle, so the file came out empty and gdk-pixbuf could not load any image
# format (SVG theme icons failed with "could not load a pixbuf"). Now that the
# references are fixed, re-run the query tool from inside the bundle so that
# @executable_path resolves to the bundle, then rewrite the reported paths to
# bundle-relative form (mirroring what gtk-mac-bundler does).
GDK_PIXBUF_VER="$(pkg-config --variable=gdk_pixbuf_binary_version gdk-pixbuf-2.0 2>/dev/null || true)"
[ -n "$GDK_PIXBUF_VER" ] || GDK_PIXBUF_VER=2.10.0
LOADERS_DIR="Contents/Resources/lib/gdk-pixbuf-2.0/$GDK_PIXBUF_VER/loaders"
LOADERS_CACHE="Contents/Resources/lib/gdk-pixbuf-2.0/$GDK_PIXBUF_VER/loaders.cache"
QUERY_LOADERS="$BREW_PREFIX/bin/gdk-pixbuf-query-loaders"
if [ -d "$LOADERS_DIR" ] && [ -x "$QUERY_LOADERS" ]; then
    log "regenerating gdk-pixbuf loaders.cache"
    cp "$QUERY_LOADERS" "Contents/MacOS/gdk-pixbuf-query-loaders"
    chmod u+w "Contents/MacOS/gdk-pixbuf-query-loaders"
    GDK_PIXBUF_MODULEDIR="$PWD/$LOADERS_DIR" \
        "Contents/MacOS/gdk-pixbuf-query-loaders" \
        > "$STAGE/loaders.raw" 2> "$STAGE/loaders.err" || true
    rm -f "Contents/MacOS/gdk-pixbuf-query-loaders"
    if [ -s "$STAGE/loaders.raw" ]; then
        sed "s#\"$PWD/Contents/Resources#\"@executable_path/../Resources#g" \
            "$STAGE/loaders.raw" > "$LOADERS_CACHE"
        dbg "loaders.cache now has $(grep -c . "$LOADERS_CACHE") lines"
    else
        log "WARNING: gdk-pixbuf-query-loaders produced no output (see $STAGE/loaders.err)"
    fi
fi

# ------------------------------------------------------------------ schemas
# GTK's file chooser calls g_settings_new("org.gtk.Settings.FileChooser"), and
# GLib aborts the whole process when that schema is not installed:
#
#   g_settings_set_property -> g_log_abort  (EXC_BREAKPOINT / SIGTRAP)
#     <- _gtk_file_chooser_get_settings_for_widget
#     <- gtk_file_chooser_widget_get_default_size
#
# gtk-mac-bundler copies libraries but not schemas, so without this every
# GtkFileChooserDialog takes the application down with it - which means the
# Browse button in the configuration box and the ZMODEM upload chooser.
# The launcher already sets XDG_DATA_DIRS to Contents/Resources/share, so
# putting the compiled schemas there is all that is needed.
SCHEMA_DIR="Contents/Resources/share/glib-2.0/schemas"
SCHEMA_SRC="$BREW_PREFIX/share/glib-2.0/schemas"
COMPILE_SCHEMAS="$BREW_PREFIX/bin/glib-compile-schemas"
if [ -d "$SCHEMA_SRC" ] && [ -x "$COMPILE_SCHEMAS" ]; then
    log "installing GSettings schemas (GTK's file chooser needs them)"
    mkdir -p "$SCHEMA_DIR"
    cp "$SCHEMA_SRC"/*.gschema.xml "$SCHEMA_DIR/" 2>/dev/null || true
    if "$COMPILE_SCHEMAS" "$SCHEMA_DIR" 2> "$STAGE/schemas.err"; then
        dbg "schemas: $(ls "$SCHEMA_DIR"/*.gschema.xml | wc -l | tr -d ' ') source files compiled"
    else
        log "WARNING: glib-compile-schemas failed (see $STAGE/schemas.err)"
        cat "$STAGE/schemas.err" >&2
    fi
else
    log "WARNING: no GSettings schemas installed; the file chooser will crash"
    log "         (looked for $SCHEMA_SRC and $COMPILE_SCHEMAS)"
fi

# ------------------------------------------------------------------ re-sign
log "ad-hoc re-signing Mach-O files (install_name_tool invalidated signatures)"
while IFS= read -r f; do
    file "$f" | grep -q 'Mach-O' || continue
    codesign --force --sign - "$f" >/dev/null 2>&1 || true
done < "$STAGE/macho-list.txt"
codesign --force --sign - "$APP" >/dev/null 2>&1 || true

# ------------------------------------------------------------------- verify
log "checking for dangling @executable_path references"
dangling=0
while IFS= read -r f; do
    file "$f" | grep -q 'Mach-O' || continue
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@executable_path' | while IFS= read -r ref; do
        p="Contents/MacOS/$(printf '%s' "$ref" | sed 's#^@executable_path/##')"
        [ -e "$p" ] || echo "MISSING: $ref (from $f)"
    done || true
done < "$STAGE/macho-list.txt" | sort -u | tee "$STAGE/dangling.txt"
if [ -s "$STAGE/dangling.txt" ]; then
    log "WARNING: dangling references found (see $STAGE/dangling.txt)"
else
    log "no dangling references"
fi

log "done. app bundle: $APP"
