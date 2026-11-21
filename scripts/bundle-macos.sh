#!/usr/bin/env bash
#
# bundle-macos.sh — build self-contained macOS .app bundles from an existing
# build dir: PuTTY.app and (PuTTY-beter, PLAN.md section 4m) PuTTYGen.app.
#
# Usage:
#   scripts/bundle-macos.sh [arm64|x86_64]
#
# Environment:
#   DEBUG=1                 verbose (debug) output
#   BREW_PREFIX=...         Homebrew prefix (default /opt/homebrew for arm64)
#   BUNDLER_DIR=...         path to the gtk-mac-bundler checkout
#   PYTHON=...              python interpreter for gtk-mac-bundler (default python3)
#   APPS="..."              override the app list (see APPS below)
#   MCP_APP=<name>          which app carries the MCP bridge, at
#                           Contents/Resources/putty-mcp with a symlink from
#                           Contents/MacOS/putty-mcp (default PuTTY); MCP_APP=
#                           skips installing it. It is NEVER installed on
#                           x86_64: mcp/node_modules contains a symlink, and from
#                           the Intel Mac this tree is on SMB, where symlinks
#                           cannot be read at all (see the note in the main loop).
#                           The arm64 slice carries the bridge, and
#                           make-universal.sh takes arm64 as its structural
#                           authority, so the merged app still has it.
#   MCP_NODE_MODULES=0      do not bundle the bridge's node_modules with it. The
#                           app then needs its packages installed (see SETUP.txt
#                           in the bundle); make-universal.sh puts them into the
#                           universal app from the shared mcp/node_modules if a
#                           slice left them out. This is what keeps an Intel
#                           copy-back to ~275 files instead of ~3700.
#   BUILD_DIR=...           build tree to take the binaries from (default
#                           build-<arch>); STAGE=... where the bundles are made
#   PUTTY_BUNDLE_LOCAL_STAGE=<dir>
#                           where to stage when the stage would be on a network
#                           mount (default ~/Library/Caches/putty-beter/
#                           bundle-stage-<arch>)
#   BUNDLE_FULL=1           ignore any bundle already in the stage and run
#                           gtk-mac-bundler for everything, from scratch
#   BUNDLE_SLOW_WARN=<secs> warn when a bundler run takes longer than this
#                           (default 300); points at the local-stage workaround
#   BUNDLE_PROGRESS=0       no progress lines while the bundler runs or while the
#                           finished apps are copied back to a network stage
#   BUNDLE_PROGRESS_INTERVAL=<secs>
#                           how often to report progress (default 15)
#   BUNDLE_STALL_WARN=<secs>
#                           warn when neither the file count nor the bundler's
#                           log has moved for this long (default 180)
#
# While the bundler runs, progress is reported every BUNDLE_PROGRESS_INTERVAL
# seconds: files of the tree so far (against the total a previous run recorded),
# size, elapsed time, install-name changes so far, and the file in hand - plus a
# warning if nothing moves for BUNDLE_STALL_WARN seconds, because a run that has
# stopped looks exactly like a run that is slow.
#
# Reuse: the bundler copies the whole GTK closure (~275 files, 44 MB) per app and
# never checks whether a file is already there, so a rebuild used to cost the
# same as the first build (~1 min locally, 20+ min on a slow network mount). If
# the GTK inputs have not changed (a signature over the dylibs/plugins/icon-theme
# mtimes is stored in the stage) the tree already in a bundle is reused, or
# cloned from the sibling app, and only the binaries/plist/icon are refreshed —
# seconds instead of minutes. BUNDLE_FULL=1, or a `brew upgrade`, forces a
# complete run.
#
# What it does, per app:
#   1. makes sure the build + icon exist
#   2. stages unix/<app>.bundle + <app>.plist + osxlaunch + <main binary> + <icon>
#   3. runs gtk-mac-bundler to produce <App>.app
#   4. normalises Homebrew's split "opt/" vs "Cellar/<f>/<ver>/" library paths so
#      only one copy of each library ends up in the bundle (otherwise GLib/GTK
#      get loaded twice and Objective-C classes collide)
#   5. ad-hoc re-signs every Mach-O (install_name_tool invalidates signatures)
#   6. checks for dangling @executable_path references
#
# gtk-mac-bundler accepts exactly one bundle file per run, so each app is a
# separate bundler invocation, each copying the whole GTK tree into its own
# .app. See PLAN.md Phase 1 and section 4m.
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
        -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
        *)            echo "bundle-macos.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------------------------------------------------------------------
# The apps to build. Each entry is
#     <app name>:<bundle file>:<main binary>:<icon file>
# The app name must match CFBundleName/CFBundleExecutable in the plist - it is
# what gtk-mac-bundler names the .app after.
#
# PuTTYGen.app is the GTK PuTTYgen front end (unix/puttygen.c); it needs no new
# launcher, because osxlaunch execs "<own path>-bin" and so serves both apps.
# ---------------------------------------------------------------------------
APPS="${APPS:-PuTTY:putty.bundle:puttyapp:PuTTY.icns PuTTYGen:puttygen.bundle:puttygenapp:PuTTYgen.icns}"

# The MCP bridge (mcp/bin/putty-mcp.mjs) is a Node program that talks to a
# running PuTTY over a Unix socket; it is copied into this app's
# Contents/Resources/putty-mcp on every bundle run, with a symlink from
# Contents/MacOS/putty-mcp (see install_mcp_bridge). Only the app with the MCP
# server compiled in needs it, and PuTTYGen has none - so it goes in PuTTY.app.
# `-` and not `:-`: an *unset* MCP_APP means PuTTY, while MCP_APP= (empty) means
# do not install it at all.
MCP_APP="${MCP_APP-PuTTY}"

# ---------------------------------------------------------------------------
# gtk-mac-bundler  (REQUIRED - not part of this project)
#
# This script needs the `gtk-mac-bundler` tool to assemble the .app bundles.
# That tool is what copies GTK and all its dependencies into the .app and
# rewrites their install names to point inside the bundle. It is a separate
# project from GNOME and is licensed GPL-2.0, so it is deliberately NOT
# vendored into this MIT-licensed source tree - you have to provide a checkout
# of it.
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
    arm64)  BREW_PREFIX="${BREW_PREFIX:-/opt/homebrew}"; : "${BUILD_DIR:=$SRC_DIR/build-arm64}" ;;
    x86_64) BREW_PREFIX="${BREW_PREFIX:-/usr/local}";    : "${BUILD_DIR:=$SRC_DIR/build-x86_64}" ;;
esac

STAGE_GIVEN=0
[ -n "${STAGE:-}" ] && STAGE_GIVEN=1
STAGE="${STAGE:-$BUILD_DIR/bundle-stage}"

# ---------------------------------------------------------------------------
# Filesystem type of the volume holding a path, from `mount`.
#
# MOUNT_OUTPUT is a test seam: point it at a file holding `mount`-style output to
# exercise the parsing below without such a mount being present.
fs_type_of() {                          # fs_type_of <path>
    local path="$1" line mp fstype best="" besttype="" mounts
    if [ -n "${MOUNT_OUTPUT:-}" ]; then
        mounts="$(cat "$MOUNT_OUTPUT")"
    else
        mounts="$(mount)"
    fi
    while IFS= read -r line; do
        case "$line" in
            *" on "*" ("*) ;;
            *) continue ;;
        esac
        mp="${line#* on }";   mp="${mp%% (*}"
        fstype="${line##* (}"; fstype="${fstype%%,*}"; fstype="${fstype%)*}"
        case "$path" in
            "$mp"|"$mp"/*)
                if [ "${#mp}" -gt "${#best}" ]; then
                    best="$mp"
                    besttype="$fstype"
                fi
                ;;
        esac
    done <<EOF
$mounts
EOF
    printf '%s' "${besttype:-unknown}"
}

is_network_fs() {                       # is_network_fs <fstype>
    case "$1" in
        smbfs|cifs|nfs|afpfs|webdav|ftp) return 0 ;;
        *) return 1 ;;
    esac
}

# Do not bundle onto a network mount unless asked. The bundler writes ~275 files
# and then rewrites every library's install names in place (~2400
# install_name_tool runs). Locally that is about a minute; over SMB the same work
# measured 85 files in 75 minutes on the Intel Mac, i.e. hours in total, because
# each of those small read-modify-writes is a network round trip. Stage on local
# disk and copy the finished .app back instead - that is a handful of big
# sequential writes.
COPY_BACK=0
FS_TYPE_STAGE="$(fs_type_of "$STAGE")"
if [ "$STAGE_GIVEN" = 0 ] && is_network_fs "$FS_TYPE_STAGE"; then
    LOCAL_STAGE="${PUTTY_BUNDLE_LOCAL_STAGE:-$HOME/Library/Caches/putty-beter/bundle-stage-$ARCH}"
    log "NOTE: $STAGE is on a network mount ($FS_TYPE_STAGE), where bundling takes"
    log "      hours instead of about a minute: every library it copies is then"
    log "      rewritten in place ~a dozen times over the network."
    log "      Staging on local disk instead:"
    log "        $LOCAL_STAGE"
    log "      and the finished .app(s) will be copied back to $STAGE/unix/."
    log "      (STAGE=<dir> overrides this choice; STAGE=$STAGE keeps it here.)"
    STAGE="$LOCAL_STAGE"
    COPY_BACK=1
fi

# The same environment build-macos.sh sets up for Intel: gtk-mac-bundler expands
# "${pkg:gtk+-3.0:gtk_binary_version}" and the like by running pkg-config from
# PATH, and the gdk-pixbuf version is queried the same way below. On an Intel Mac
# the host pkg-config already is the x86_64 one, but when x86_64 is built from an
# Apple Silicon machine it is not, and the bundle would be assembled from the
# arm64 Homebrew's paths. Put the target prefix's bin first so both cases work.
if [ "$ARCH" = "x86_64" ]; then
    export PATH="$BREW_PREFIX/bin:$PATH"
    export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:$BREW_PREFIX/opt/gtk+3/lib/pkgconfig"
    log "using Intel toolchain: PATH/PKG_CONFIG_PATH point at $BREW_PREFIX"
fi
RES="Contents/Resources"

log "arch       : $ARCH"
log "build dir  : $BUILD_DIR"
log "brew prefix: $BREW_PREFIX"
log "bundler dir: $BUNDLER_DIR"
log "apps       : $APPS"
# Identify the file that is actually running. On a network mount a stale read is
# otherwise invisible - a run has already used the *previous* script and quietly
# done the old thing, costing a ten-minute copy-back - so this line is the first
# thing to compare against the server: `shasum -a 256 scripts/bundle-macos.sh`. A
# mismatch means the mount served cached bytes; remounting the share is the fix.
log "script     : $(shasum -a 256 "$0" 2>/dev/null | cut -c1-12)  $(wc -c < "$0" | tr -d ' ') bytes  $(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$0" 2>/dev/null)"

# ------------------------------------------------------------------- preflight
for f in "$BUILD_DIR/osxlaunch"; do
    [ -x "$f" ] || { log "ERROR: missing binary: $f (run scripts/build-macos.sh first)"; exit 1; }
done
for app in $APPS; do
    IFS=: read -r name bundle mainbin icns <<<"$app"
    [ -x "$BUILD_DIR/$mainbin" ] || {
        log "ERROR: missing binary: $BUILD_DIR/$mainbin (run scripts/build-macos.sh first)"
        exit 1
    }
    [ -f "$SRC_DIR/unix/$bundle" ] || { log "ERROR: missing $SRC_DIR/unix/$bundle"; exit 1; }
    [ -f "$SRC_DIR/icons/$icns" ] || { log "ERROR: missing $SRC_DIR/icons/$icns"; exit 1; }
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
# PuTTY-beter: both app icons are square PNGs converted to .icns here with
# scripts/make-icns.sh (sips + iconutil):
#   icons/putty-beter-icon.png -> icons/PuTTY.icns     (PuTTY.app)
#   icons/puttygen-icon.png    -> icons/PuTTYgen.icns  (PuTTYGen.app)
#
# The upstream icon set in icons/ - mkicon.py + macicon.py, i.e.
# `make -C icons PuTTY.icns` / `PuTTYgen.icns` - is kept and used as a fallback
# when the matching PNG is missing. Its output is always overwritten when the
# PNG is there, so that a stale .icns from that route cannot end up in a bundle.
ICON_PNG="$SRC_DIR/icons/putty-beter-icon.png"
ICON_ICNS="$SRC_DIR/icons/PuTTY.icns"
KEYGEN_ICON_PNG="$SRC_DIR/icons/puttygen-icon.png"
KEYGEN_ICON_ICNS="$SRC_DIR/icons/PuTTYgen.icns"

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

if [ -f "$KEYGEN_ICON_PNG" ]; then
    # Always rebuild this one: an icons/PuTTYgen.icns left behind by the
    # mkicon.py route would otherwise look newer than the PNG and win.
    log "converting icons/puttygen-icon.png -> icons/PuTTYgen.icns"
    "$SRC_DIR/scripts/make-icns.sh" "$KEYGEN_ICON_PNG" "$KEYGEN_ICON_ICNS"
elif [ ! -f "$KEYGEN_ICON_ICNS" ]; then
    log "generating icons/PuTTYgen.icns from the upstream icon set"
    make -C "$SRC_DIR/icons" PuTTYgen.icns >/dev/null
fi

# ---------------------------------------------------------------------- stage
#
# Bundling is the expensive part (it copies ~275 files / 44 MB into each .app),
# so the stage is NOT wiped on every run: the GTK tree that is already in a
# bundle is reused, and only this app's own files (binary, launcher, plist, icon)
# are refreshed. See "reuse" below. BUNDLE_FULL=1 throws everything away.
if [ "${BUNDLE_FULL:-0}" = "1" ]; then
    log "BUNDLE_FULL=1: removing $STAGE and bundling from scratch"
    rm -rf "$STAGE"
fi
log "staging in $STAGE"
mkdir -p "$STAGE/unix" "$STAGE/icons"
for app in $APPS; do
    IFS=: read -r name bundle mainbin icns <<<"$app"
    cp "$SRC_DIR/unix/$bundle" "$STAGE/unix/"
    cp "$SRC_DIR/unix/${bundle%.bundle}.plist" "$STAGE/unix/" 2>/dev/null || true
    cp "$BUILD_DIR/$mainbin" "$STAGE/"
    cp "$SRC_DIR/icons/$icns" "$STAGE/icons/"
done
cp "$BUILD_DIR/osxlaunch" "$STAGE/"

# ===========================================================================
# Per-app work
# ===========================================================================

# Run one phase and log how long it took. The bundler and the repair passes are
# all dominated by process spawns and file I/O (a few thousand of them), so on a
# network mount the timings are the only way to see where the time goes.
TIMED_SECS=0                            # how long the last timed phase took
timed() {                               # timed <label> <command...>
    local label="$1"; shift
    local t0 rc
    t0="$(date +%s)"
    "$@"
    rc=$?
    TIMED_SECS=$(( $(date +%s) - t0 ))
    log "$label took ${TIMED_SECS}s"
    return $rc
}

# ------------------------------------------------------------------- reuse
#
# gtk-mac-bundler copies the whole GTK closure every time and has no notion of
# "that file is already there" - it walks its input list and copies, then runs
# install_name_tool over every library it touched. That makes a rebuild cost the
# same as the first build: ~1 minute on a local disk, and tens of minutes on a
# slow network mount (measured: 20+ minutes on the Intel Mac).
#
# Both shortcuts below keep that cost to the first build, keyed on a signature of
# the GTK inputs so that a Homebrew upgrade still forces a complete run:
#
#   * the app already has the tree  -> reuse it in place and refresh only this
#                                     app's binary, launcher, plist and icon
#   * the app has no tree, but a sibling app in the stage does -> clone the
#                                     sibling's Contents and swap in this app's
#                                     own files
#
# The repair passes are idempotent, so they run either way (only the ones that
# are properties of the GTK tree - Cellar->opt, dangling refs, loaders.cache -
# are skipped for a reused tree, which we did not touch).

# Newest mtime among the files the bundler copies: the dylibs, the gdk-pixbuf
# loaders, the immodules, the print backends and the icon theme. A few hundred
# stats, and enough to notice a `brew upgrade`.
gtk_signature() {
    local most
    # One `stat` for the whole set, not one *forked* stat per file: this used to
    # spawn /usr/bin/stat for every dylib in the prefix, which is ~3 s on an
    # M-series Mac but measured ~174 s on the Intel Mac (a slower disk, and
    # thousands of files under /usr/local). Same file set, same answer.
    # -L because opt/<formula> is a symlink into Cellar, which find would
    # otherwise not descend into at all.
    most="$(
        {
            find -L "$BREW_PREFIX/opt" -mindepth 3 -maxdepth 3 \
                 -name '*.dylib' -path '*/lib/*' 2>/dev/null || true
            find -L "$BREW_PREFIX/lib" -maxdepth 1 -name '*.dylib' 2>/dev/null \
                 || true
            find -L "$BREW_PREFIX/lib/gdk-pixbuf-2.0" -path '*/loaders/*.so' \
                 2>/dev/null || true
            find -L "$BREW_PREFIX/lib/gtk-3.0" \
                 \( -path '*/immodules/*.so' -o -path '*/printbackends/*.so' \) \
                 2>/dev/null || true
            printf '%s\n' "$BREW_PREFIX/share/icons/Adwaita/index.theme"
        } | tr '\n' '\0' | xargs -0 stat -f %m 2>/dev/null | sort -n | tail -1
    )" || true
    printf '%s|%s|%s' "$ARCH" "$BREW_PREFIX" "${most:-0}"
}

# Entries in the .app root other than Contents. There should never be any: a
# .app holds exactly one thing. A stray entry is outside the code-signature seal,
# so codesign says "unsealed contents present in the bundle root" and macOS
# refuses to open the app - see clear_app for how one gets there. Prints the
# offending basenames, one per line; empty output means the root is clean.
stray_root_entries() {                  # stray_root_entries <app path>
    find "$1" -mindepth 1 -maxdepth 1 ! -name Contents -exec basename {} \; \
        2>/dev/null
}

# Is this .app a complete bundle, i.e. did a previous run get all the way
# through? (A run killed part-way through leaves a directory behind.)
#
# Used to decide whether an existing tree can be reused, cloned or copied back,
# so it has to be choosy about what counts as "there": a bundle with a stray
# entry in its root looks identical under Contents, but is one macOS will not
# open, and treating it as reusable is how such a bundle survives a rebuild.
app_complete() {                        # app_complete <app path>
    local app="$1" exe n
    [ -d "$app" ] || return 1
    exe="$(plutil -extract CFBundleExecutable raw \
                   "$app/Contents/Info.plist" 2>/dev/null || true)"
    [ -n "$exe" ] || return 1
    [ -x "$app/Contents/MacOS/$exe" ] || return 1
    [ -x "$app/Contents/MacOS/$exe-bin" ] || return 1
    [ -z "$(stray_root_entries "$app")" ] || return 1
    [ -f "$app/Contents/Resources/share/glib-2.0/schemas/gschemas.compiled" ] \
        || return 1
    # The closure is 75 Mach-O files; the whole bundle is ~273 files.
    n="$(find "$app/Contents" -type f 2>/dev/null | wc -l | tr -d ' ')"
    [ "${n:-0}" -ge 250 ] || return 1
    return 0
}

# Remove a previous app before rebuilding it, and insist that it is gone.
#
# This is not paranoia: `cp -R "$tpl" "$APP"` and the bundler's
# `shutil.move(".PuTTY.app", "PuTTY.app")` both *nest* into an existing
# directory instead of replacing it, giving PuTTY.app/.PuTTY.app. The result
# still looks like an app and still signs, but the extra entry sits outside the
# seal, so codesign says "unsealed contents present in the bundle root" and
# macOS refuses to open it. A network mount is where a removal quietly fails,
# so check the result rather than assuming it.
clear_app() {                           # clear_app <name> <app path>
    rm -rf "$2"
    [ ! -e "$2" ] && return 0
    log "[$1] ERROR: could not remove $2"
    return 1
}

# Another app in the stage that we can clone the GTK tree from.
find_usable_template() {                # find_usable_template <app name>
    local skip="$1.app" d
    for d in "$STAGE/unix"/*.app; do
        [ -d "$d" ] || continue
        case "$(basename "$d")" in
            "$skip") continue ;;
        esac
        if app_complete "$d"; then
            printf '%s' "$d"
            return 0
        fi
    done
    return 1
}

bundle_one() {                          # bundle_one <name> <bundle> <mainbin> <icns> <sig>
    local name="$1" bundle="$2" mainbin="$3" icns="$4" sig="${5:-}"
    local APP="$STAGE/unix/$name.app"
    local macho="$STAGE/macho-list-$name.txt"
    local plist="$SRC_DIR/unix/${bundle%.bundle}.plist"
    local exe stored tpl tpl_exe tpl_icns reused=0
    local refbin=""                 # a binary the bundler already rewrote

    exe="$(plutil -extract CFBundleExecutable raw "$plist" 2>/dev/null || true)"
    [ -n "$exe" ] || { log "[$name] ERROR: no CFBundleExecutable in $plist"; return 1; }

    # <sig> is handed in because the caller computes it once for the whole run;
    # fall back to computing it here if this is ever called without one.
    [ -n "$sig" ] || sig="$(gtk_signature)"
    stored=""
    [ -f "$STAGE/.gtk-signature" ] && stored="$(cat "$STAGE/.gtk-signature")"

    if [ "${BUNDLE_FULL:-0}" != "1" ] && [ "$sig" = "$stored" ]; then
        if app_complete "$APP"; then
            reused=1
            refbin="$APP/Contents/MacOS/$exe-bin"
            log "[$name] reusing the GTK tree already in $APP"
            log "[$name]   (a Homebrew upgrade, or BUNDLE_FULL=1, forces a full bundle)"
        elif tpl="$(find_usable_template "$name")"; then
            reused=1
            log "[$name] cloning the GTK tree from $(basename "$tpl")"
            tpl_exe="$(plutil -extract CFBundleExecutable raw "$tpl/Contents/Info.plist")"
            tpl_icns="$(plutil -extract CFBundleIconFile raw "$tpl/Contents/Info.plist" 2>/dev/null || true)"
            refbin="$tpl/Contents/MacOS/$tpl_exe-bin"
            clear_app "$name" "$APP" || return 1
            cp -R "$tpl" "$APP"
            # The launcher is the same osxlaunch binary under a different name,
            # so renaming it is all that a clone needs; the other app's -bin and
            # icon are replaced by ours below.
            if [ "$tpl_exe" != "$exe" ]; then
                mv "$APP/Contents/MacOS/$tpl_exe" "$APP/Contents/MacOS/$exe"
                rm -f "$APP/Contents/MacOS/$tpl_exe-bin"
            fi
            if [ -n "$tpl_icns" ] && [ "$tpl_icns" != "$icns" ]; then
                rm -f "$APP/Contents/Resources/$tpl_icns"
            fi
        fi
    fi

    if [ "$reused" = 0 ]; then
        log "[$name] running gtk-mac-bundler"
        # -u so that $STAGE/bundler-$name.log fills in as it goes: this phase is
        # the long one, and Python's block buffering otherwise makes the log look
        # idle until it finishes. `tail -f` that file to watch progress.
        clear_app "$name" "$APP" || return 1
        timed "[$name] gtk-mac-bundler" run_bundler "$name" "$bundle" || return 1
        printf '%s' "$sig" > "$STAGE/.gtk-signature"
        # Nothing else to do: the bundler has already put this app's launcher,
        # main binary (with its install names rewritten), plist and icon in place.
        # Overwriting the binary here would *undo* the rewrite.
        dbg "[$name] bundle size: $(du -sh "$APP" | cut -f1)"
        printf '%s %s' 0 "${TIMED_SECS:-0}"
        return 0
    fi

    # Reused or cloned tree: refresh this app's own files, and give the binary the
    # install-name rewrite the bundler would have done. The mapping is taken from
    # a binary the bundler already rewrote - this app's previous -bin (reused) or
    # the sibling app's (cloned) - rather than re-derived (see
    # rewrite_main_binary_refs).
    local refdeps="$STAGE/refdeps-$name.txt"
    if [ -f "$refbin" ]; then
        otool -L "$refbin" 2>/dev/null | tail -n +2 | awk '{print $1}' \
            > "$refdeps"
    fi

    cp "$BUILD_DIR/$mainbin" "$APP/Contents/MacOS/$exe-bin"
    cp "$plist" "$APP/Contents/Info.plist"
    cp "$SRC_DIR/icons/$icns" "$APP/Contents/Resources/$icns"
    if [ ! -x "$APP/Contents/MacOS/$exe" ]; then
        cp "$BUILD_DIR/osxlaunch" "$APP/Contents/MacOS/$exe"
    fi
    rewrite_main_binary_refs "$APP/Contents/MacOS/$exe-bin" "$refdeps"

    dbg "[$name] bundle size: $(du -sh "$APP" | cut -f1)"
    printf '%s %s' 1 0
    return 0
}

# How many files the finished app should have, when we know: the count recorded
# by a previous full run, or the count in a complete sibling app.
expected_files() {                      # expected_files <name>
    local name="$1" d n
    if [ -f "$STAGE/.expected-files-$name" ]; then
        cat "$STAGE/.expected-files-$name"
        return 0
    fi
    for d in "$STAGE/unix"/*.app; do
        [ -d "$d" ] || continue
        case "$(basename "$d")" in
            .*) continue ;;            # gtk-mac-bundler's own work in progress
        esac
        n="$(find "$d/Contents" -type f 2>/dev/null | wc -l | tr -d ' ')"
        if [ "${n:-0}" -ge 250 ]; then
            printf '%s' "$n"
            return 0
        fi
    done
    return 0
}

# The bundler itself, with progress reporting.
#
# gtk-mac-bundler prints nothing while it copies, and a run can take a minute
# locally or the best part of an hour on a slow mount, so it looks like a hang.
# While it runs, report what we can see: how many files of the tree are in place
# (against the expected total, when a previous run recorded it), how big it is,
# how long it has been, how many install names it has rewritten, and the file in
# hand. If neither the file count nor the log has moved for a while, say so once:
# that is a stall, not slowness.
run_bundler() {                         # run_bundler <name> <bundle file>
    local name="$1" bundle="$2"
    local app="$STAGE/unix/$name.app" log="$STAGE/bundler-$name.log"
    local interval="${BUNDLE_PROGRESS_INTERVAL:-15}"
    local stall="${BUNDLE_STALL_WARN:-180}"
    local pid t0 rc=0 n prev_n=-1 loglines prev_loglines=-1 expected sz work
    local last_change last_change_seen=1 now idle last changes
    local peak=0 last_shown="" el eltxt

    t0="$(date +%s)"
    : > "$log"
    ( cd "$STAGE/unix" && \
      PUTTY_GTK_PREFIX_FROM_MAKEFILE="$BREW_PREFIX" \
      "$PYTHON" -u -c "import sys; sys.path.insert(0, '$BUNDLER_DIR'); import bundler.main; bundler.main.main(['$bundle'])" \
    ) >"$log" 2>&1 &
    pid=$!

    expected="$(expected_files "$name")"
    last_change="$t0"
    log "[$name] bundling$([ -n "$expected" ] && printf ' (0/%s files)' "$expected")..."
    log "[$name]   progress every ${interval}s; full detail: tail -f $log"

    while kill -0 "$pid" 2>/dev/null; do
        [ "${BUNDLE_PROGRESS:-1}" = "0" ] && { sleep 5; continue; }
        sleep "$interval"
        kill -0 "$pid" 2>/dev/null || break

        # gtk-mac-bundler assembles into a dot-prefixed temporary directory and
        # renames it when it is done, so count whichever exists.
        work="$app"
        [ -d "$STAGE/unix/.$name.app" ] && work="$STAGE/unix/.$name.app"
        n="$(find "$work" -type f 2>/dev/null | wc -l | tr -d ' ')"
        loglines="$(wc -l < "$log" 2>/dev/null | tr -d ' ')"
        sz="$(du -sh "$work" 2>/dev/null | cut -f1)"
        changes="$(grep -c install_name_tool "$log" 2>/dev/null || true)"
        now="$(date +%s)"

        # Activity is either new files or new log lines: during the
        # install_name_tool phase the count does not grow, but the log does.
        if [ "${n:-0}" != "$prev_n" ] || [ "${loglines:-0}" != "$prev_loglines" ]; then
            prev_n="${n:-0}"; prev_loglines="${loglines:-0}"
            last_change="$now"
            last_change_seen=1
        fi
        idle=$(( now - last_change ))
        # (an `if`, not `[ ... ] && x=y`: under set -e a failed test as the last
        # command of a list would end the script)
        if [ "${n:-0}" -gt "${peak:-0}" ]; then
            peak="${n:-0}"
        fi

        # Elapsed as H:MM:SS, because these numbers get big.
        el=$(( now - t0 ))
        eltxt="$(( el / 3600 )):$(printf '%02d' $(( (el % 3600) / 60 ))):$(printf '%02d' $(( el % 60 )))"

        if [ -n "$expected" ]; then
            log "[$name]   ${eltxt} | files ${n:-0}/${expected} (peak ${peak:-0}) | ${sz:-?} | rewrites ${changes:-0} | idle ${idle}s"
        else
            log "[$name]   ${eltxt} | files ${n:-0} (peak ${peak:-0}) | ${sz:-?} | rewrites ${changes:-0} | idle ${idle}s"
        fi

        # What it is working on, as a path relative to the .app (the absolute one
        # is long enough that it tells you nothing but the stage directory), and
        # only when it changes - otherwise it is a wall of identical lines.
        last="$(sed -n 's/.*in: //p' "$log" 2>/dev/null | tail -1)"
        case "$last" in
            *"/Contents/"*)
                last="${last#*/Contents/}"
                ;;
            *)
                if [ -n "$last" ]; then
                    last="$(basename "$last")"
                fi
                ;;
        esac
        last="$(printf '%s' "$last" | cut -c1-76)"
        if [ -n "$last" ] && [ "$last" != "${last_shown:-}" ]; then
            last_shown="$last"
            log "[$name]     now: $last"
        fi

        if [ "$idle" -gt "$stall" ] && [ "$last_change_seen" = 1 ]; then
            last_change_seen=0
            log "[$name]   WARNING: nothing has changed for ${idle}s - this looks stalled."
            log "[$name]     Check the mount (a dropped SMB session blocks in I/O),"
            log "[$name]     and prefer staging on local disk next time:"
            log "[$name]       STAGE=\$HOME/tmp/stage scripts/bundle-macos.sh $ARCH"
        fi
    done

    wait "$pid" || rc=$?
    if [ "$rc" != 0 ]; then
        log "[$name] ERROR: bundler failed (exit $rc), see $log"
        tail -20 "$log" >&2
        return 1
    fi

    [ -d "$app" ] || {
        log "[$name] ERROR: bundler produced no app at $app"
        return 1
    }
    n="$(find "$app/Contents" -type f 2>/dev/null | wc -l | tr -d ' ')"
    printf '%s' "$n" > "$STAGE/.expected-files-$name"
    log "[$name] bundler finished in $(( $(date +%s) - t0 ))s: $n files, $(du -sh "$app" | cut -f1)"
    return 0
}

# ------------------------------------------------- normalise opt/ vs Cellar/
# Homebrew's libraries reference each other through two different paths:
# /opt/homebrew/opt/<formula>/...  and  /opt/homebrew/Cellar/<formula>/<ver>/...
# gtk-mac-bundler copies both, so two copies of the same dylib get loaded at
# runtime, which produces "Class X is implemented in both ..." warnings and
# breaks GObject/GtkApplication. Rewrite every Cellar reference to its opt
# equivalent, then drop the duplicate Cellar tree.
#
# The list is every Mach-O in the bundle, not only those under Resources: the
# executables in Contents/MacOS must be re-signed as well. On arm64 the linker
# ad-hoc signs them by itself, so leaving them out went unnoticed; the x86_64
# linker does not, and an unsigned nested binary makes the bundle-level codesign
# in the re-sign pass fail - which the "|| true" there hides, leaving the .app
# unsigned on Intel.
normalise_cellar() {                    # normalise_cellar <name> <app> <macho-list>
    local name="$1" APP="$2" macho="$3"

    log "[$name] normalising Cellar -> opt references"
    ( cd "$APP" &&
      find Contents -type f > "$macho" &&
      while IFS= read -r f; do
          file "$f" | grep -q 'Mach-O' || continue
          dbg "[$name] normalising: $f"
          otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep 'Cellar' | while IFS= read -r old; do
              new="$(printf '%s' "$old" | sed -E 's#/Cellar/([^/]+)/[^/]+/#/opt/\1/#')"
              dbg "[$name]   change: $old -> $new"
              install_name_tool -change "$old" "$new" "$f" 2>/dev/null || true
          done || true
          own="$(otool -D "$f" 2>/dev/null | tail -n +2 | head -1)"
          case "$own" in
              *Cellar*)
                  newown="$(printf '%s' "$own" | sed -E 's#/Cellar/([^/]+)/[^/]+/#/opt/\1/#')"
                  dbg "[$name]   id: $own -> $newown"
                  install_name_tool -id "$newown" "$f" 2>/dev/null || true
                  ;;
          esac
      done < "$macho"
      rm -rf "$RES/Cellar" )
    log "[$name] duplicate Cellar tree removed"
}

# ------------------------------------------ resolve remaining dangling refs
# Some Homebrew libraries reference each other through the "flat" symlink
# directory (/opt/homebrew/lib/x.dylib) while others use
# /opt/homebrew/opt/<formula>/lib/x.dylib. gtk-mac-bundler copies the file only
# once, at whichever path it discovered first, so a reference can end up
# pointing at a location that does not exist in the bundle. (This is what
# prevented gdk-pixbuf's loaders.cache from being generated: the loaders could
# not dlopen libgdk_pixbuf.) Point any such reference at the copy that does
# exist, matching by basename and only when the match is unambiguous.
resolve_dangling() {                    # resolve_dangling <name> <app> <macho-list>
    local name="$1" APP="$2" macho="$3"

    log "[$name] resolving dangling @executable_path references"
    ( cd "$APP" &&
      find "$RES" -name '*.dylib' | sed 's#^Contents/##' > "$STAGE/dylib-index-$name.txt" &&
      while IFS= read -r f; do
          file "$f" | grep -q 'Mach-O' || continue
          otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@executable_path' | while IFS= read -r ref; do
              rel="${ref#@executable_path/}"
              [ -e "Contents/MacOS/$rel" ] && continue
              base="$(basename "$ref")"
              matches="$(awk -v b="$base" 'substr($0, length($0)-length(b)+1) == b' "$STAGE/dylib-index-$name.txt")"
              n="$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l | tr -d ' ')"
              if [ "$n" -eq 1 ]; then
                  dbg "[$name]   fix: $ref -> @executable_path/../$matches"
                  install_name_tool -change "$ref" "@executable_path/../$matches" "$f" 2>/dev/null || true
              elif [ "$n" -eq 0 ]; then
                  log "[$name]   WARNING: unresolved reference $ref (in $f)"
              else
                  log "[$name]   WARNING: ambiguous reference $ref ($n candidates, in $f)"
              fi
          done || true
      done < "$macho" )
    log "[$name] dangling reference pass complete"
}

# ------------------------------------- regenerate the gdk-pixbuf loaders cache
# gtk-mac-bundler writes loaders.cache while it is bundling, but at that point
# the loader modules still referenced a library path that did not yet exist in
# the bundle, so the file came out empty and gdk-pixbuf could not load any image
# format (SVG theme icons failed with "could not load a pixbuf"). Now that the
# references are fixed, re-run the query tool from inside the bundle so that
# @executable_path resolves to the bundle, then rewrite the reported paths to
# bundle-relative form (mirroring what gtk-mac-bundler does).
fix_loaders_cache() {                   # fix_loaders_cache <name> <app>
    local name="$1" APP="$2"
    local GDK_PIXBUF_VER LOADERS_DIR LOADERS_CACHE QUERY_LOADERS

    GDK_PIXBUF_VER="$(pkg-config --variable=gdk_pixbuf_binary_version gdk-pixbuf-2.0 2>/dev/null || true)"
    [ -n "$GDK_PIXBUF_VER" ] || GDK_PIXBUF_VER=2.10.0
    LOADERS_DIR="Contents/Resources/lib/gdk-pixbuf-2.0/$GDK_PIXBUF_VER/loaders"
    LOADERS_CACHE="Contents/Resources/lib/gdk-pixbuf-2.0/$GDK_PIXBUF_VER/loaders.cache"
    QUERY_LOADERS="$BREW_PREFIX/bin/gdk-pixbuf-query-loaders"

    ( cd "$APP" || exit 1
      if [ -d "$LOADERS_DIR" ] && [ -x "$QUERY_LOADERS" ]; then
          log "[$name] regenerating gdk-pixbuf loaders.cache"
          cp "$QUERY_LOADERS" "Contents/MacOS/gdk-pixbuf-query-loaders"
          chmod u+w "Contents/MacOS/gdk-pixbuf-query-loaders"
          GDK_PIXBUF_MODULEDIR="$PWD/$LOADERS_DIR" \
              "Contents/MacOS/gdk-pixbuf-query-loaders" \
              > "$STAGE/loaders-$name.raw" 2> "$STAGE/loaders-$name.err" || true
          rm -f "Contents/MacOS/gdk-pixbuf-query-loaders"
          if [ -s "$STAGE/loaders-$name.raw" ]; then
              sed "s#\"$PWD/Contents/Resources#\"@executable_path/../Resources#g" \
                  "$STAGE/loaders-$name.raw" > "$LOADERS_CACHE"
              dbg "[$name] loaders.cache now has $(grep -c . "$LOADERS_CACHE") lines"
          else
              log "[$name] WARNING: gdk-pixbuf-query-loaders produced no output (see $STAGE/loaders-$name.err)"
          fi
      fi )
}

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
# Browse button in the configuration box, the ZMODEM upload chooser and
# PuTTYgen's Open Key / Save Key dialogs. The launcher already sets
# XDG_DATA_DIRS to Contents/Resources/share, so putting the compiled schemas
# there is all that is needed.
install_schemas() {                     # install_schemas <name> <app>
    local name="$1" APP="$2"
    local SCHEMA_DIR="Contents/Resources/share/glib-2.0/schemas"
    local SCHEMA_SRC="$BREW_PREFIX/share/glib-2.0/schemas"
    local COMPILE_SCHEMAS="$BREW_PREFIX/bin/glib-compile-schemas"

    ( cd "$APP" || exit 1
      if [ -d "$SCHEMA_SRC" ] && [ -x "$COMPILE_SCHEMAS" ]; then
          log "[$name] installing GSettings schemas (GTK's file chooser needs them)"
          mkdir -p "$SCHEMA_DIR"
          cp "$SCHEMA_SRC"/*.gschema.xml "$SCHEMA_DIR/" 2>/dev/null || true
          if "$COMPILE_SCHEMAS" "$SCHEMA_DIR" 2> "$STAGE/schemas-$name.err"; then
              dbg "[$name] schemas: $(ls "$SCHEMA_DIR"/*.gschema.xml | wc -l | tr -d ' ') source files compiled"
          else
              log "[$name] WARNING: glib-compile-schemas failed (see $STAGE/schemas-$name.err)"
              cat "$STAGE/schemas-$name.err" >&2
          fi
      else
          log "[$name] WARNING: no GSettings schemas installed; the file chooser will crash"
      fi )
}

# The MCP bridge (PLAN.md section 7): the Node program an agent runs to speak MCP
# over stdio to a *live* PuTTY window.
#
# It is installed at Contents/Resources/putty-mcp, with a SYMLINK to it from
# Contents/MacOS/putty-mcp - so the path a human configures is the obvious
# ".../Contents/MacOS/putty-mcp/bin/putty-mcp.mjs". The symlink is not a
# decoration: codesign treats everything under Contents/MacOS as *code*, so a
# bundle with unsigned non-code files there does not seal ("code object is not
# signed at all", measured), while the files under Resources are covered by the
# bundle seal and need no individual signature. A symlink in MacOS is ignored by
# codesign and survives ditto and the merge.
#
# The dependencies are bundled with it (mcp/node_modules), so the app needs
# nothing installed: `import '@modelcontextprotocol/sdk'` then resolves by the
# ordinary walk-up into this same folder - no absolute paths, no global packages,
# nothing to set up. They are pure JavaScript (measured: 0 .node files, 0 Mach-O
# anywhere in the tree, no os/cpu-constrained packages), so the arm64 and x86_64
# copies are byte-identical and the merge copies them through like any other text
# file. MCP_NODE_MODULES=0 leaves them out instead, and the bundle's SETUP.txt
# then has to tell the user to install them.
#
# This runs on every bundle run, including a reused tree, so the folder tracks
# the source: a file deleted from mcp/ disappears from the bundle too.
install_mcp_bridge() {                  # install_mcp_bridge <name> <app>
    local name="$1" APP="$2"
    local SRC="$SRC_DIR/mcp"
    local DEST="Contents/Resources/putty-mcp"       # the real files
    local LINK="Contents/MacOS/putty-mcp"           # the path to configure

    if [ ! -f "$SRC/bin/putty-mcp.mjs" ]; then
        log "[$name] WARNING: no MCP bridge at $SRC/bin/putty-mcp.mjs; skipping it"
        return 0
    fi
    if [ ! -f "$SRC/SETUP.txt" ]; then
        log "[$name] WARNING: $SRC/SETUP.txt is missing; the bundled bridge will"
        log "[$name]          have no note on how to configure it"
    fi

    log "[$name] installing the MCP bridge (${LINK#Contents/})"
    # rm -rf/rm -f first, so this is the source tree's contents and not a merge
    # of the two, and so a real directory left by an older layout is gone before
    # ln -s (which would otherwise create the link *inside* it). ${APP:?} so a
    # bug cannot reduce this to "rm -rf /".
    rm -rf "${APP:?}/$DEST" "${APP:?}/$LINK"
    mkdir -p "$APP/$DEST/bin" "$APP/$DEST/src"
    cp "$SRC/bin/putty-mcp.mjs" "$APP/$DEST/bin/"
    cp "$SRC"/src/*.mjs "$APP/$DEST/src/"
    # package.json/package-lock.json document what the deps are and pin them;
    # USER-MANUAL.md is read at runtime (src/manual.mjs, via ../USER-MANUAL.md),
    # so it must sit next to src/, not deeper.
    cp "$SRC/package.json" "$SRC/package-lock.json" "$APP/$DEST/"
    cp "$SRC/USER-MANUAL.md" "$SRC/README.md" "$APP/$DEST/"
    cp "$SRC/SETUP.txt" "$APP/$DEST/SETUP.txt"
    # The dependencies, so the app needs nothing installed (see the note above).
    # ditto, not cp: it keeps the symlinks in node_modules/.bin as symlinks.
    if [ "${MCP_NODE_MODULES:-1}" = "1" ]; then
        if [ -d "$SRC/node_modules" ]; then
            log "[$name]   + its dependencies ($(find "$SRC/node_modules" \
                -type f | wc -l | tr -d ' ') files)"
            if ! ditto "$SRC/node_modules" "$APP/$DEST/node_modules"; then
                log "[$name] ERROR: could not copy $SRC/node_modules"
                # Usually a symlink: ditto has to read every link's target to
                # reproduce it, and this tree is reached over SMB from the Intel
                # Mac, where readlink fails with EINVAL. Name the links, so the
                # failure is not a bare ditto message. (x86_64 does not get here:
                # the bridge is not installed on that arch - see the main loop.)
                find "$SRC/node_modules" -type l 2>/dev/null \
                    | sed "s/^/[$name]   symlink: /"
                return 1
            fi
        else
            log "[$name] WARNING: $SRC/node_modules is missing, so the bundled"
            log "[$name]          bridge will need its packages installed"
        fi
    fi
    chmod +x "$APP/$DEST/bin/putty-mcp.mjs"
    ( cd "$APP" && ln -s "../Resources/putty-mcp" "$LINK" )
    dbg "[$name] MCP bridge: $(find "$APP/$DEST" -type f | wc -l | tr -d ' ') files + ${LINK#Contents/} -> ../Resources/putty-mcp"
}

# Only one app carries the bridge (MCP_APP). This is not a formality: when an app
# has no GTK tree of its own, bundle_one CLONES its sibling's - PuTTYGen.app is
# cloned from PuTTY.app - and the clone brings the whole payload with it, bridge
# and all. Left alone, PuTTYGen.app would ship a second copy of the bridge, and the
# merge would then add 23 MB of dependencies to go with it. So the payload step is
# authoritative in both directions: install it for MCP_APP, remove it for the rest.
remove_mcp_bridge() {                   # remove_mcp_bridge <name> <app>
    local name="$1" APP="$2"
    local DEST="Contents/Resources/putty-mcp"
    local LINK="Contents/MacOS/putty-mcp"

    if [ ! -e "${APP:?}/$DEST" ] && [ ! -L "${APP:?}/$LINK" ]; then
        return 0
    fi
    log "[$name] removing the MCP bridge (it belongs to ${MCP_APP:-no app})"
    rm -rf "${APP:?}/$DEST" "${APP:?}/$LINK"
}

# ------------------------------------------------------------------ re-sign
resign() {                              # resign <name> <app> <macho-list>
    local name="$1" APP="$2" macho="$3"

    # A build product must not carry com.apple.quarantine. It arrives on the
    # *sources* - this repository's own files have it, from a browser download -
    # and `cp` propagates it into Contents/Info.plist. Measured: it does not stop
    # the app launching (a valid seal plus a quarantined Info.plist opens
    # normally), so it is not a cause of "cannot be opened" - but it is a
    # download marker with no place in something we ship.
    if ! xattr -cr "$APP" 2>/dev/null; then
        log "[$name] note: could not clear extended attributes on $APP"
    fi

    log "[$name] ad-hoc re-signing Mach-O files (install_name_tool invalidated signatures)"
    # Everything inside the .app, including Contents/MacOS and the .so files
    # that are not dylibs - the list came from `find Contents -type f`.
    ( cd "$APP" &&
      while IFS= read -r f; do
          file "$f" | grep -q 'Mach-O' || continue
          codesign --force --sign - "$f" >/dev/null 2>&1 || true
      done < "$macho"
      codesign --force --sign - "$APP" >/dev/null 2>&1 || true )
}

# ------------------------------------------------------------------- verify
verify() {                              # verify <name> <app> <macho-list>
    local name="$1" APP="$2" macho="$3"
    local strays sig_out

    # A .app directory contains exactly one thing: Contents. Anything else in the
    # root is outside the code-signature seal, so codesign reports "unsealed
    # contents present in the bundle root" and macOS refuses to open the app -
    # the failure looks like "the application cannot be opened" to the user, with
    # nothing in the build log to explain it. This is worth its own check because
    # the usual cause is a rebuild that did not replace the previous app but
    # nested into it (see clear_app).
    strays="$(stray_root_entries "$APP")"
    if [ -n "$strays" ]; then
        log "[$name] ERROR: stray entries in the bundle root:"
        printf '%s\n' "$strays" | sed "s/^/[$name]   /"
        log "[$name]   a .app may contain only Contents; such an entry sits outside"
        log "[$name]   the signature seal and macOS will refuse to open the app."
        log "[$name]   Remove it, then re-bundle (BUNDLE_FULL=1 forces a clean stage)."
        return 1
    fi

    log "[$name] checking for dangling @executable_path references"
    ( cd "$APP" &&
      while IFS= read -r f; do
          file "$f" | grep -q 'Mach-O' || continue
          otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@executable_path' | while IFS= read -r ref; do
              p="Contents/MacOS/$(printf '%s' "$ref" | sed 's#^@executable_path/##')"
              [ -e "$p" ] || echo "MISSING: $ref (from $f)"
          done || true
      done < "$macho" ) | sort -u | tee "$STAGE/dangling-$name.txt"
    if [ -s "$STAGE/dangling-$name.txt" ]; then
        log "[$name] WARNING: dangling references found (see $STAGE/dangling-$name.txt)"
    else
        log "[$name] no dangling references"
    fi

    log "[$name] signature check"
    # This must fail the build, not warn: an app that codesign rejects is one
    # macOS will not open. Run it once and keep the output for the error path.
    if sig_out="$(codesign --verify --deep --strict "$APP" 2>&1)"; then
        log "[$name] codesign --verify --deep --strict: OK"
    else
        log "[$name] ERROR: codesign --verify --deep --strict failed:"
        printf '%s\n' "$sig_out" | sed "s/^/[$name]   /"
        return 1
    fi
}

# gtk-mac-bundler rewrites the main binary's library references to point inside
# the bundle (that is what makes the app self-contained). A binary we drop in
# ourselves has to get the same treatment: without it the app loads the *system*
# GTK and, worse, the system gdk-pixbuf - whose loader list is empty - so GTK
# aborts the moment it needs an SVG icon ("Failed to load .../image-missing.svg").
#
# Rather than re-deriving the bundler's mapping rules, take them from a binary the
# bundler already rewrote: this app's previous -bin (a reused tree) or the sibling
# app's (a cloned tree). Anything with no precedent falls back to the rule the
# bundler uses, which is "strip the Homebrew prefix and prepend
# @executable_path/../Resources/", plus Cellar/<f>/<v> -> opt/<f>.
rewrite_main_binary_refs() {            # <new binary> <file of reference deps>
    local f="$1" refdeps="$2" old base mapped
    [ -f "$refdeps" ] || return 0

    for old in $(otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}'); do
        case "$old" in
            @executable_path*|/usr/lib/*|/System/*) continue ;;
        esac
        base="$(basename "$old")"
        mapped="$(awk -v b="/$base" \
                  'substr($0, length($0)-length(b)+1) == b { print; exit }' \
                  "$refdeps")"
        if [ -z "$mapped" ]; then
            mapped="@executable_path/../Resources/${old#"$BREW_PREFIX"/}"
            mapped="$(printf '%s' "$mapped" | sed -E 's#/Cellar/([^/]+)/[^/]+/#/opt/\1/#')"
        fi
        dbg "  rewrite $old -> $mapped"
        install_name_tool -change "$old" "$mapped" "$f" 2>/dev/null || true
    done
    return 0
}

# ===========================================================================
# The GTK-input signature is the same for every app in this run, and computing it
# walks the whole Homebrew prefix, so do it once here rather than once per app
# (it used to be per app, which on the Intel Mac was minutes of the run).
sig_t0="$(date +%s)"
bundle_sig="$(gtk_signature)"
sig_secs=$(( $(date +%s) - sig_t0 ))
if [ "$sig_secs" -ge 5 ]; then
    log "GTK-input signature (a walk of $BREW_PREFIX) took ${sig_secs}s"
fi

for app in $APPS; do
    IFS=: read -r name bundle mainbin icns <<<"$app"
    APP="$STAGE/unix/$name.app"
    macho="$STAGE/macho-list-$name.txt"
    app_t0="$(date +%s)"

    # bundle_one reports back "<reused> <seconds the bundler itself took>", so the
    # note below quotes the bundler's time and not, as it did at first, the time
    # of the whole of bundle_one (which reported 315 s for a 141 s run).
    out="$(bundle_one "$name" "$bundle" "$mainbin" "$icns" "$bundle_sig")" || exit 1
    reused="${out%% *}"
    bundle_secs="${out##* }"

    # A bundler run that takes minutes is almost always a network mount: it writes
    # ~275 files and rewrites ~2400 library references, and each of those is a
    # round trip. Say so once, with the workaround, rather than letting anyone
    # wait an hour twice. (Measured: ~60 s locally, hours over SMB.)
    # (The time reported is the bundler's own, from `timed`; see where bundle_one
    # is called.)
    if [ "$reused" = 0 ] && [ "$bundle_secs" -gt "${BUNDLE_SLOW_WARN:-300}" ]; then
        log "[$name] NOTE: that bundler run took ${bundle_secs}s."
        if [ "$COPY_BACK" = 0 ] && is_network_fs "$FS_TYPE_STAGE"; then
            log "[$name]       The stage is on a network mount, so stage local:"
            log "[$name]         STAGE=\$HOME/tmp/stage scripts/bundle-macos.sh $ARCH"
            log "[$name]       (the finished .app is then copied back by hand; see README)"
        else
            log "[$name]       That is slow for a local disk: this machine is busy, or"
            log "[$name]       its disk is slow. See README for what to expect."
        fi
    fi

    # The list of Mach-O files to re-sign and verify. normalise_cellar() writes
    # the same list, but a reused tree skips that pass, so build it here.
    ( cd "$APP" && find Contents -type f > "$macho" )

    if [ "$reused" = "1" ]; then
        # Properties of the GTK tree, which a previous verified run already
        # fixed and this run has not touched.
        log "[$name] reusing the tree: skipping Cellar->opt, dangling-refs and loaders.cache"
    else
        timed "[$name] Cellar->opt"   normalise_cellar "$name" "$APP" "$macho"
        timed "[$name] dangling refs" resolve_dangling "$name" "$APP" "$macho"
        timed "[$name] loaders.cache" fix_loaders_cache "$name" "$APP"
    fi
    timed "[$name] schemas" install_schemas "$name" "$APP"
    # MCP is not bundled on x86_64. install_mcp_bridge copies mcp/node_modules,
    # and node_modules contains a symlink (npm's .bin/node-which shim). The Intel
    # Mac reaches this tree over SMB, where a symlink cannot be read at all -
    # readlink, stat -L, open and cp -L all fail with EINVAL; only lstat works -
    # so no copy tool can reproduce one, and ditto aborts the whole run there.
    # Rather than special-case that one link, the Intel build leaves the bridge
    # out entirely: the arm64 slice carries it, and make-universal.sh takes the
    # arm64 bundle as its structural authority, so the merged app still has it.
    mcp_installed_here=0
    if [ -n "$MCP_APP" ] && [ "$name" = "$MCP_APP" ] && [ "$ARCH" != "x86_64" ]; then
        mcp_installed_here=1
    fi
    # Before re-sign, so the bridge files are sealed by the bundle-level codesign
    # and not added outside it. See install_mcp_bridge / remove_mcp_bridge. The
    # remove branch is authoritative: it takes the bridge back out of a tree that
    # a previous run (or the other arch) left it in.
    if [ "$mcp_installed_here" = 1 ]; then
        timed "[$name] MCP bridge" install_mcp_bridge "$name" "$APP"
    else
        if [ "$name" = "$MCP_APP" ] && [ "$ARCH" = "x86_64" ]; then
            log "[$name] MCP bridge: not bundled on x86_64 (see the note above)"
        fi
        timed "[$name] MCP bridge" remove_mcp_bridge "$name" "$APP"
    fi
    timed "[$name] re-sign" resign "$name" "$APP" "$macho"
    timed "[$name] verify"  verify "$name" "$APP" "$macho" || exit 1

    log "[$name] done in $(( $(date +%s) - app_t0 ))s. app bundle: $APP"
done

# With an automatic local stage, put the finished apps where they are expected to
# be (the network stage), so everything downstream - make-universal.sh, the
# README's paths, a human looking in build-<arch>/bundle-stage - finds them, and
# the local stage is only a cache.
if [ "$COPY_BACK" = 1 ]; then
    DEST="$BUILD_DIR/bundle-stage/unix"
    log "copying the finished bundles back to $DEST"
    mkdir -p "$DEST"
    copy_t0="$(date +%s)"
    for app in $APPS; do
        IFS=: read -r name bundle mainbin icns <<<"$app"
        src="$STAGE/unix/$name.app"
        want="$(find "$src" -type f | wc -l | tr -d ' ')"
        exe="$(plutil -extract CFBundleExecutable raw \
                    "$src/Contents/Info.plist" 2>/dev/null || true)"

        # The two small control files are cheap; keep them in step always.
        cp "$STAGE/unix/$bundle" "$STAGE/unix/${bundle%.bundle}.plist" "$DEST/" \
            2>/dev/null || true

        # Is it already there and up to date? The marker says which GTK inputs the
        # copy in $DEST was made from; the file count catches a copy that is
        # missing part of the app (the MCP bridge, say); and this app's own files
        # are compared byte for byte. That is a few reads, against ~280 files of
        # round trips for a full recopy - which over SMB is minutes.
        # (Cheapest first: the marker test usually settles it and skips the rest,
        # and the count is checked before the byte compares because those read the
        # whole binary over the network.)
        fresh=1
        if [ "$(cat "$DEST/.gtk-signature" 2>/dev/null || true)" = "$bundle_sig" ] \
           && app_complete "$DEST/$name.app" \
           && [ "$(find "$DEST/$name.app" -type f 2>/dev/null | wc -l | tr -d ' ')" \
                = "$want" ] \
           && cmp -s "$src/Contents/Info.plist" \
                     "$DEST/$name.app/Contents/Info.plist" \
           && cmp -s "$src/Contents/MacOS/$exe-bin" \
                     "$DEST/$name.app/Contents/MacOS/$exe-bin"; then
            fresh=0
        fi
        if [ "$fresh" = 0 ]; then
            log "[$name] already in the stage and up to date; not copying again"
            continue
        fi

        log "[$name] copying $want files there (over a network mount this is many"
        log "[$name]   small round trips, not one big write)"
        cp_t0="$(date +%s)"
        clear_app "$name" "$DEST/$name.app" || exit 1
        cp -R "$src" "$DEST/" &
        cp_pid=$!
        while kill -0 "$cp_pid" 2>/dev/null; do
            if [ "${BUNDLE_PROGRESS:-1}" = "0" ]; then
                sleep 5
                continue
            fi
            sleep "${BUNDLE_PROGRESS_INTERVAL:-15}"
            kill -0 "$cp_pid" 2>/dev/null || break
            n="$(find "$DEST/$name.app" -type f 2>/dev/null | wc -l | tr -d ' ')"
            el=$(( $(date +%s) - cp_t0 ))
            log "[$name]   copy back: ${n:-0}/${want} files, $(( el / 60 ))m$(( el % 60 ))s elapsed"
        done
        if ! wait "$cp_pid"; then
            log "[$name] ERROR: copying $name.app back failed"
            exit 1
        fi
        got="$(find "$DEST/$name.app" -type f 2>/dev/null | wc -l | tr -d ' ')"
        el=$(( $(date +%s) - cp_t0 ))
        # The signature was checked in the local stage just now; checking it again
        # *here* would hash all 275 files over the network for no new information.
        # So count the files, and confirm that what landed is still a bundle worth
        # shipping: app_complete also rejects a stray entry in the root, which is
        # what a copy that nested into an existing app leaves behind.
        if [ "${got:-0}" = "${want:-0}" ] && app_complete "$DEST/$name.app"; then
            printf '%s' "$bundle_sig" > "$DEST/.gtk-signature"
            log "[$name] copied back in ${el}s: $got files, $(du -sh "$src" | cut -f1), signature checked in the stage"
        else
            log "[$name] ERROR: the copy back is not a usable bundle:"
            log "[$name]   ${got:-0} of ${want:-0} files; stray entries in the root:"
            stray_root_entries "$DEST/$name.app" | sed "s/^/[$name]     /"
            exit 1
        fi
    done
    log "copy back took $(( $(date +%s) - copy_t0 ))s"
else
    log "done. bundles in: $STAGE/unix"
fi
