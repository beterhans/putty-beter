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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/make-icns.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

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
iconutil -c icns "$ICONSET" -o "$DST"

log "done: $DST ($(wc -c <"$DST" | tr -d ' ') bytes)"
