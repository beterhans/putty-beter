#!/bin/sh
#
# make-icns.sh - convert a square PNG into a macOS .icns icon.
#
#     usage: scripts/make-icns.sh <source.png> <output.icns>
#
# Uses only tools that ship with macOS: `sips` to resize and `iconutil` to
# assemble the .iconset into an .icns. The source should be at least 1024x1024
# and ideally have an alpha channel, so that the icon does not end up with
# opaque corners.
#
# Set DEBUG=1 for a trace of what it is doing.

set -e

if [ "$DEBUG" = "1" ]; then
    set -x
fi

SRC="$1"
DST="$2"

log() {
    printf '[make-icns] %s\n' "$*"
}

# Chatty only when DEBUG=1, like the other scripts.
dbg() {
    [ "${DEBUG:-0}" = "1" ] && printf '[make-icns][debug] %s\n' "$*" >&2
    return 0
}

if [ -z "$SRC" ] || [ -z "$DST" ]; then
    echo "usage: $0 <source.png> <output.icns>" >&2
    exit 2
fi

[ -f "$SRC" ] || { log "ERROR: no such file: $SRC"; exit 1; }

for tool in sips iconutil; do
    command -v "$tool" >/dev/null 2>&1 || {
        log "ERROR: $tool not found (it should be part of macOS)"
        exit 1
    }
done

# The source has to be square. Each iconset entry is produced with `sips -z N N`,
# which stretches to the exact size and ignores the aspect ratio, so a
# non-square source would come out distorted - silently. Refuse instead, and say
# what to do about it.
SRC_W="$(sips -g pixelWidth "$SRC" | awk '/pixelWidth/{print $2}')"
SRC_H="$(sips -g pixelHeight "$SRC" | awk '/pixelHeight/{print $2}')"
if [ "$SRC_W" != "$SRC_H" ]; then
    log "ERROR: source is not square: $SRC_W x $SRC_H"
    log "       'sips -z' would stretch it to a square. Crop or pad the PNG"
    log "       (transparent padding, centred) so that it is square, and that"
    log "       its artwork reaches the edges: macOS scales the whole canvas, so"
    log "       a transparent margin makes the finished icon look undersized."
    exit 1
fi
dbg "source: $SRC_W x $SRC_H"

if [ "$SRC_W" -lt 1024 ]; then
    log "WARNING: source is smaller than 1024 x 1024 ($SRC_W x $SRC_H);"
    log "         the largest icon will be upscaled and may look soft"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/make-icns.XXXXXX")"
TMP_DST=""
trap 'rm -rf "$WORK"; if [ -n "${TMP_DST:-}" ]; then rm -f "$TMP_DST"; fi' EXIT

ICONSET="$WORK/icon.iconset"
mkdir -p "$ICONSET"

# The ten images iconutil expects. Each "@2x" entry is the size of the next
# size up, which is why 32, 256 and 512 appear twice.
resize() {                              # size, destination basename
    size="$1"
    name="$2"
    sips -z "$size" "$size" "$SRC" --out "$ICONSET/$name" >/dev/null
    log "  $name ($size x $size)"
}

log "building iconset from $SRC"
resize 16   icon_16x16.png
resize 32   icon_16x16@2x.png
resize 32   icon_32x32.png
resize 64   icon_32x32@2x.png
resize 128  icon_128x128.png
resize 256  icon_128x128@2x.png
resize 256  icon_256x256.png
resize 512  icon_256x256@2x.png
resize 512  icon_512x512.png
resize 1024 icon_512x512@2x.png

log "assembling $DST"
mkdir -p "$(dirname "$DST")"
# Write to a temporary name in the same directory, then rename it into place, so a
# reader sees either the previous icon or the complete new one and never a
# half-written file. Two machines can be bundling at once - bundle-macos.sh
# regenerates this icon on every run, deliberately, so that a stale .icns cannot
# beat it - and an interrupted run would otherwise leave a truncated .icns behind
# for the next build to copy into the app.
#
# The temporary name keeps the .icns extension and is dot-prefixed: iconutil
# refuses any other extension ("The file extension must equal icns"), and a dot
# file cannot be picked up by the icons/*.icns glob if one is ever left behind.
TMP_DST="$(dirname "$DST")/.$(basename "$DST").tmp.$$.icns"
iconutil -c icns "$ICONSET" -o "$TMP_DST"
mv -f "$TMP_DST" "$DST"

log "done: $DST ($(wc -c <"$DST" | tr -d ' ') bytes)"
