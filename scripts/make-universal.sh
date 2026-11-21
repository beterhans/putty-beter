#!/usr/bin/env bash
#
# make-universal.sh — merge the per-architecture .app bundles into universal ones.
#
# Each slice is built and bundled on its own machine: arm64 on the arm Mac
# (build-arm64/), x86_64 on the Intel Mac (build-x86_64/). Both must come from the
# same git revision and have the same relative layout; the contract, the measured
# differences between the two slices, and the traps are in intel_arm_merge.md.
#
# This does the pairing with `lipo`: a Mach-O in both slices becomes one universal
# Mach-O, non-Mach-O files are copied through, and a Mach-O that exists in only
# one slice is a hard error (it cannot be made universal).
#
# Usage:
#   scripts/make-universal.sh
#       Merge every app in $APPS from the two stage directories, then package
#       the result as a drag-to-Applications disk image.
#
#   scripts/make-universal.sh <arm64.app> <x86_64.app> [<out.app>]
#       One app, explicit paths. <out.app> defaults to
#       $OUT_DIR/<basename of arm64.app>; the disk image then holds that app
#       alone.
#
# The disk image has the merged .app(s) and a symlink to /Applications at the
# root of its volume, so both apps are dragged into /Applications in one
# gesture. It is a plain compressed (UDZO) image - no background picture and no
# icon layout, because those need a mounted, Finder-arranged volume and buy
# nothing for a two-app install.
#
# Options:
#   -h, --help              this text.
#
# Environment:
#   APPS=<names>            apps to merge (default: "PuTTY PuTTYGen").
#   ARM_STAGE=<dir>         arm64 bundles   (default: build-arm64/bundle-stage/unix).
#   X86_STAGE=<dir>         x86_64 bundles  (default: build-x86_64/bundle-stage/unix).
#   OUT_DIR=<dir>           output          (default: build-universal).
#   MAKE_DMG=0              merge only; do not build the disk image.
#   DMG_NAME=<file>         disk image name (default: PuTTY-universal.dmg, or
#                           <app>-universal.dmg in the single-app form).
#   DMG_VOLNAME=<name>      volume name     (default: PuTTY).
#   DEBUG=1                 verbose per-file progress.
#
# Relative defaults are anchored at the repository root, so this works from any
# working directory. Exits non-zero if any slice is missing, already universal,
# unmergeable, or if the merged bundle fails its final verification, or if the
# disk image cannot be created or does not verify.
#
# See intel_arm_merge.md section 6 for the design and section 7 for the checks
# this script performs at the end.
#
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DEBUG="${DEBUG:-0}"
log() { printf '[make-universal] %s\n' "$*" >&2; }
dbg() { if [ "$DEBUG" = 1 ]; then printf '[make-universal:debug] %s\n' "$*" >&2; fi; }

usage() { sed -n '3,/^set -euo pipefail/p' "$0" | sed '$d' | sed 's/^#\{1,2\} \{0,1\}//'; }

# ---------------------------------------------------------------------- helpers

is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }

# Entries in the .app root other than Contents. There should never be any, and a
# bundle that has one is not something to merge: the entry sits outside the
# signature seal, so the result would not open even if the merge succeeded.
strays_of() {                           # strays_of <app>
    find "$1" -mindepth 1 -maxdepth 1 ! -name Contents -exec basename {} \; \
        2>/dev/null
}

# The name of the executable in a bundle's Contents/MacOS, per its Info.plist.
exe_of() {                              # exe_of <app>
    plutil -extract CFBundleExecutable raw "$1/Contents/Info.plist" 2>/dev/null \
        || true
}

# ------------------------------------------------------------------ disk image

# Create a compressed, read-only image (UDZO) from a folder. macOS 27 deprecates
# `hdiutil create` in favour of `diskutil image create from`, so prefer that and
# fall back to `hdiutil` where it is absent (older macOS) or fails.
dmg_create() {                          # dmg_create <srcfolder> <dest.dmg> <volname>
    local src="$1" dest="$2" vol="$3"

    if diskutil image create from --help >/dev/null 2>&1; then
        if diskutil image create from "$src" "$dest" --format UDZO \
                --volumeName "$vol" >/dev/null 2>&1; then
            return 0
        fi
        log "[dmg] note: diskutil image create from failed; trying hdiutil"
        rm -f "$dest"                   # do not leave a half-written image behind
    fi
    hdiutil create -volname "$vol" -srcfolder "$src" -ov -format UDZO "$dest"
}

# A drag-to-Applications installer: the .app(s) and a symlink to /Applications
# at the root of the volume. Staged in a temporary directory (local disk), not
# in OUT_DIR - those ~40 MB bundles would otherwise be copied twice, and on a
# network mount the second copy is minutes.
make_dmg() {                            # make_dmg <dmg> <volname> <app>...
    local dmg="$1" vol="$2"; shift 2
    local stage a names=""

    stage="$(mktemp -d "${TMPDIR:-/tmp}/putty-dmg.XXXXXX")" || return 1
    for a in "$@"; do
        if [ ! -d "$a" ]; then
            log "[dmg] ERROR: not a bundle: $a"
            rm -rf "$stage"
            return 1
        fi
        ditto "$a" "$stage/$(basename "$a")"    # ditto, not cp: .app-aware
        names="$names $(basename "$a")"
    done
    ln -s /Applications "$stage/Applications"

    rm -f "$dmg"
    log "[dmg] creating $(basename "$dmg") (volume '$vol')"
    if ! dmg_create "$stage" "$dmg" "$vol"; then
        log "[dmg] ERROR: could not create $dmg"
        rm -rf "$stage"
        return 1
    fi
    rm -rf "$stage"

    # The apps were verified by merge_one; this only confirms the image itself
    # was written whole.
    if ! hdiutil verify "$dmg" >/dev/null 2>&1; then
        log "[dmg] ERROR: $dmg failed its checksum verification"
        return 1
    fi
    log "[dmg] OK:$names + Applications/"
    log "[dmg]   $dmg ($(du -h "$dmg" | cut -f1))"
}

# --------------------------------------------------------------------- merge one

merge_one() {                           # merge_one <name> <arm64.app> <x86_64.app> <out.app>
    local name="$1" ARM="$2" X86="$3" OUT="$4"
    local exe arm_bin x86_bin a_archs x_archs list rel src_a src_x dst
    local n_files=0 n_macho=0 n_uni=0 archs stray

    for d in "$ARM" "$X86"; do
        [ -d "$d" ] || { log "[$name] ERROR: not a bundle: $d"; return 1; }
        while IFS= read -r stray; do
            [ -n "$stray" ] || continue
            log "[$name] ERROR: stray entry in the root of $d: $stray"
            log "[$name]   a .app may contain only Contents; re-bundle that slice"
            return 1
        done < <(strays_of "$d")
    done

    exe="$(exe_of "$ARM")"
    [ -n "$exe" ] || { log "[$name] ERROR: no CFBundleExecutable in $ARM"; return 1; }
    arm_bin="$ARM/Contents/MacOS/$exe-bin"
    x86_bin="$X86/Contents/MacOS/$exe-bin"
    if [ ! -f "$arm_bin" ] || [ ! -f "$x86_bin" ]; then
        log "[$name] ERROR: $exe-bin missing in one of the slices"
        log "[$name]   arm64:  $arm_bin"
        log "[$name]   x86_64: $x86_bin"
        return 1
    fi

    # Exactly one architecture each, and different ones: merging a bundle with
    # itself, or with an already-universal one, is a mistake worth catching here.
    a_archs="$(lipo -archs "$arm_bin")"
    x_archs="$(lipo -archs "$x86_bin")"
    log "[$name] slices: $a_archs + $x_archs"
    for archs in "$a_archs" "$x_archs"; do
        case "$archs" in
            *" "*) log "[$name] ERROR: a slice is already universal ($archs)"
                   return 1 ;;
        esac
    done
    if [ "$a_archs" = "$x_archs" ]; then
        log "[$name] ERROR: both slices are $a_archs - the same arch twice?"
        return 1
    fi

    log "[$name] merging into $OUT"
    rm -rf "$OUT"
    mkdir -p "$OUT"

    # --- 1. the arm64 bundle is the structural authority ---------------------
    # Its file list defines the output. Here-strings, not pipes: an `exit 1` in a
    # pipeline's subshell would not stop the script.
    list="$( cd "$ARM" && find . \( -type f -o -type l \) -print )"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in ./*) ;; *) continue ;; esac      # skip find's own noise
        src_a="$ARM/${rel#./}"; src_x="$X86/${rel#./}"; dst="$OUT/${rel#./}"
        mkdir -p "$(dirname "$dst")"

        if [ -L "$src_a" ]; then                        # symlink: recreate as-is
            cp -P "$src_a" "$dst"
            dbg "symlink       $rel"
        elif [ ! -e "$src_x" ] && [ ! -L "$src_x" ]; then   # arm64 only
            cp -P "$src_a" "$dst"
            dbg "arm64 only    $rel"
        elif is_macho "$src_a"; then                    # the point of the exercise
            if ! is_macho "$src_x"; then
                log "[$name] ERROR: $rel is Mach-O in arm64 but not in x86_64"
                return 1
            fi
            lipo -create "$src_a" "$src_x" -output "$dst"
            dbg "lipo          $rel"
        else                                            # common, not Mach-O
            cp -P "$src_a" "$dst"
            if ! cmp -s "$src_a" "$src_x"; then
                log "[$name] note: $rel differs between the slices; kept the arm64 copy"
            fi
        fi
    done <<<"$list"

    # --- 2. entries that exist only in the x86_64 bundle --------------------
    # Non-Mach-O extras are harmless and get copied; a Mach-O with no counterpart
    # cannot be made universal, and that is the "fail loudly" case.
    list="$( cd "$X86" && find . \( -type f -o -type l \) -print )"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in ./*) ;; *) continue ;; esac
        if [ -e "$ARM/${rel#./}" ] || [ -L "$ARM/${rel#./}" ]; then
            continue                    # already handled above
        fi
        src_x="$X86/${rel#./}"; dst="$OUT/${rel#./}"
        if is_macho "$src_x"; then
            log "[$name] ERROR: $rel is Mach-O but exists only in x86_64, so it"
            log "[$name]   cannot be made universal. Rebuild both slices from the"
            log "[$name]   same revision and the same GTK version."
            return 1
        fi
        log "[$name] note: $rel exists only in x86_64; copied through"
        mkdir -p "$(dirname "$dst")"
        cp -P "$src_x" "$dst"
    done <<<"$list"

    # --- 2b. the MCP bridge's dependencies ---------------------------------
    # The bridge in the merged app has to be able to run, and it will not without
    # its packages beside it. Either slice may have left them out - the Intel side
    # is expected to, because pushing 3400 small files across an SMB mount takes
    # minutes where one local copy takes a second - and the arm64 authority above
    # copies them through when only it has them. This is the case where neither
    # did: put them in from the one shared tree, so the merged app is never
    # shipped with a bridge that cannot load. (The packages are pure JavaScript,
    # so one copy is right for both architectures.)
    if [ -f "$OUT/Contents/Resources/putty-mcp/bin/putty-mcp.mjs" ] \
       && [ ! -d "$OUT/Contents/Resources/putty-mcp/node_modules" ] \
       && [ -d "$SRC_DIR/mcp/node_modules" ]; then
        log "[$name] neither slice carried the bridge's dependencies; adding them"
        if ! ditto "$SRC_DIR/mcp/node_modules" \
                "$OUT/Contents/Resources/putty-mcp/node_modules"; then
            log "[$name] ERROR: could not copy $SRC_DIR/mcp/node_modules"
            return 1
        fi
    fi
    if [ -f "$OUT/Contents/Resources/putty-mcp/bin/putty-mcp.mjs" ] \
       && [ ! -d "$OUT/Contents/Resources/putty-mcp/node_modules" ]; then
        log "[$name] WARNING: the bridge has no node_modules and $SRC_DIR/mcp"
        log "[$name]          has none either, so the bundled bridge will need"
        log "[$name]          its packages installed (see SETUP.txt)"
    fi

    # --- 3. re-sign, inside-out --------------------------------------------
    # lipo invalidated every signature it touched, and the Mach-O files in
    # Contents/MacOS are the trap this project has already fallen into once: they
    # are not under Contents/Resources, and on x86_64 the linker does not ad-hoc
    # sign them by itself. This walks the whole bundle, so no directory is missed,
    # and a signing failure is fatal rather than `|| true`-hidden.
    log "[$name] re-signing every Mach-O, then the bundle"
    # Same reason as bundle-macos.sh's re-sign pass: this repository's files carry
    # com.apple.quarantine from a browser download, and cp propagates it. It does
    # not stop an app launching, but it should not be in what we ship.
    if ! xattr -cr "$OUT" 2>/dev/null; then
        log "[$name] note: could not clear extended attributes on $OUT"
    fi
    list="$( cd "$OUT" && find . -type f -print )"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in ./*) ;; *) continue ;; esac
        f="$OUT/${rel#./}"
        if is_macho "$f"; then
            if ! codesign --force --sign - "$f" >/dev/null 2>&1; then
                log "[$name] ERROR: cannot sign $f"
                return 1
            fi
        fi
    done <<<"$list"
    if ! codesign --force --sign - "$OUT" >/dev/null 2>&1; then
        log "[$name] ERROR: cannot sign the bundle $OUT"
        return 1
    fi

    # --- 4. verify what was just produced ----------------------------------
    list="$( cd "$OUT" && find . \( -type f -o -type l \) -print )"
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in ./*) ;; *) continue ;; esac
        f="$OUT/${rel#./}"
        n_files=$(( n_files + 1 ))
        is_macho "$f" || continue
        n_macho=$(( n_macho + 1 ))
        archs="$(lipo -archs "$f" 2>/dev/null || true)"
        case "$archs" in
            *" "*) n_uni=$(( n_uni + 1 )) ;;
            *) log "[$name] ERROR: not universal: $rel ($archs)" ;;
        esac
    done <<<"$list"

    if [ "$n_macho" != "$n_uni" ]; then
        log "[$name] ERROR: $(( n_macho - n_uni )) of $n_macho Mach-O files have one slice"
        return 1
    fi
    if ! codesign --verify --deep --strict "$OUT"; then
        log "[$name] ERROR: codesign --verify --deep --strict failed on $OUT"
        return 1
    fi
    # A Homebrew path in the main binary means the bundle stopped being
    # self-contained: it would load the *host's* GTK, not the bundled one.
    if otool -L "$OUT/Contents/MacOS/$exe-bin" | grep -qE '/usr/local|/opt/homebrew'; then
        log "[$name] ERROR: an absolute Homebrew path leaked into $exe-bin:"
        otool -L "$OUT/Contents/MacOS/$exe-bin" | grep -E '/usr/local|/opt/homebrew' \
            | sed "s/^/[$name]   /"
        return 1
    fi

    log "[$name] OK: $n_files files, $n_macho Mach-O all $a_archs+$x_archs, signed and verified"
    log "[$name]   $OUT"
}

# ------------------------------------------------------------------------ main

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

APPS="${APPS:-PuTTY PuTTYGen}"
ARM_STAGE="${ARM_STAGE:-$SRC_DIR/build-arm64/bundle-stage/unix}"
X86_STAGE="${X86_STAGE:-$SRC_DIR/build-x86_64/bundle-stage/unix}"
OUT_DIR="${OUT_DIR:-$SRC_DIR/build-universal}"
MAKE_DMG="${MAKE_DMG:-1}"
DMG_NAME="${DMG_NAME:-}"                # default depends on what is merged
DMG_VOLNAME="${DMG_VOLNAME:-PuTTY}"

merged=()                               # the output .app paths, in order
if [ "$#" -ge 2 ]; then
    arm="$1"; x86="$2"
    out="${3:-$OUT_DIR/$(basename "$arm")}"
    name="$(basename "$arm" .app)"
    merge_one "$name" "$arm" "$x86" "$out" || exit 1
    merged+=("$out")
else
    [ "$#" -eq 0 ] || { log "ERROR: expected 0 or 2+ arguments"; usage; exit 2; }
    for name in $APPS; do
        out="$OUT_DIR/$name.app"
        merge_one "$name" "$ARM_STAGE/$name.app" "$X86_STAGE/$name.app" "$out" \
            || exit 1
        merged+=("$out")
    done
fi

merged_names=""
for a in "${merged[@]}"; do merged_names="$merged_names $(basename "$a")"; done
log "merged:$merged_names"

if [ "$MAKE_DMG" = 1 ]; then
    # Naming: a bare run packages the suite under one name; the single-app form
    # (explicit paths) is named after that app, so it cannot overwrite the
    # suite image in the same OUT_DIR.
    if [ -z "$DMG_NAME" ]; then
        if [ "${#merged[@]}" = 1 ]; then
            DMG_NAME="$(basename "${merged[0]%.app}")-universal.dmg"
        else
            DMG_NAME="PuTTY-universal.dmg"
        fi
    fi
    make_dmg "$OUT_DIR/$DMG_NAME" "$DMG_VOLNAME" "${merged[@]}" || exit 1
else
    log "MAKE_DMG=0: no disk image built"
fi

log "now launch each of these on the arm Mac natively, and on the Intel Mac"
log "natively - not under Rosetta, and not with arch -x86_64 forcing a slice."
