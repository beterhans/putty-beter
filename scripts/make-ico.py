#!/usr/bin/env python3
#
# make-ico.py - convert a square PNG into a multi-size Windows .ico icon.
#
#     usage: scripts/make-ico.py <source.png> <output.ico>
#
# This is the Windows counterpart of scripts/make-icns.sh: that one wraps a PNG
# into a macOS .icns with `sips`/`iconutil`, and this one wraps it into an .ico.
# There is no Windows equivalent of those tools, so this script does the whole
# job itself, using nothing but the Python standard library (`zlib` for the
# deflate streams). That keeps the fork's rule that the icon sources are the
# checked-in PNGs and no external image tool is needed to rebuild the icons.
#
# It reads the PNG (8-bit RGB or RGBA), area-averages it down to each size
# Windows wants, and writes each resized image into the .ico as a PNG-compressed
# entry - which Windows has understood since Vista, so it is fine for this
# fork's Windows 7 SP1 target. The .ico is written to a temporary name and
# renamed into place, so a reader never sees a half-written icon.
#
# Set MAKE_ICO_DEBUG=1 for a trace of what it is doing.

import os
import struct
import sys
import zlib

# The sizes Explorer, the taskbar and Alt-Tab ask for. 256 is the "large"
# icon; 16/32/48 are the classic shell sizes; 64 and 128 fill the gaps.
SIZES = [16, 32, 48, 64, 128, 256]

DEBUG = os.environ.get("MAKE_ICO_DEBUG", "") not in ("", "0")


def log(msg):
    print("[make-ico] %s" % msg)


def dbg(msg):
    if DEBUG:
        print("[make-ico][debug] %s" % msg, file=sys.stderr)


# --------------------------------------------------------------------------
# PNG decoding. Just enough for what an icon source is: 8-bit greyscale, RGB,
# palette or RGBA, non-interlaced. Anything else is refused with a message
# rather than silently mis-read.
# --------------------------------------------------------------------------

def read_png(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG file" % path)

    pos = 8
    width = height = None
    bitdepth = colortype = interlace = None
    palette = None
    trns = None
    idat = bytearray()

    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length        # length + type + body + crc

        if ctype == b"IHDR":
            width, height, bitdepth, colortype, _comp, _filt, interlace = \
                struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        # Ancillary chunks (gAMA, pHYs, ...) are ignored.

    if width is None:
        raise ValueError("%s: no IHDR" % path)
    if interlace != 0:
        raise ValueError("%s: interlaced PNG is not supported" % path)
    if bitdepth != 8:
        raise ValueError("%s: only 8-bit PNGs are supported (got %d)"
                         % (path, bitdepth))

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colortype)
    if channels is None:
        raise ValueError("%s: unsupported colour type %d" % (path, colortype))

    raw = zlib.decompress(bytes(idat))
    bpp = channels                      # bytes per pixel (bit depth 8)
    stride = width * bpp
    out = bytearray(height * stride)

    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 0:
            pass
        elif ftype == 1:                       # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:                       # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:                       # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:                       # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[i] = (line[i] + pr) & 0xFF
        else:
            raise ValueError("%s: bad filter type %d on row %d"
                             % (path, ftype, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # Normalise to RGBA.
    rgba = bytearray(width * height * 4)
    if colortype == 6:
        rgba[:] = out
    elif colortype == 2:
        for i in range(width * height):
            rgba[4 * i:4 * i + 3] = out[3 * i:3 * i + 3]
            rgba[4 * i + 3] = 0xFF
    elif colortype == 0:
        for i in range(width * height):
            g = out[i]
            rgba[4 * i:4 * i + 4] = bytes((g, g, g, 0xFF))
    elif colortype == 4:
        for i in range(width * height):
            g = out[2 * i]
            rgba[4 * i:4 * i + 4] = bytes((g, g, g, out[2 * i + 1]))
    elif colortype == 3:
        if palette is None:
            raise ValueError("%s: palette image without PLTE" % path)
        for i in range(width * height):
            idx = out[i]
            r, g, b = palette[3 * idx:3 * idx + 3]
            a = trns[idx] if (trns and idx < len(trns)) else 0xFF
            rgba[4 * i:4 * i + 4] = bytes((r, g, b, a))

    return width, height, rgba


# --------------------------------------------------------------------------
# Area-average downscaling. Averaging happens in premultiplied alpha, so a
# partly transparent edge pixel cannot drag a dark halo into the result.
# --------------------------------------------------------------------------

def downsample(sw, sh, rgba, dw, dh):
    out = bytearray(dw * dh * 4)
    for dy in range(dh):
        y0 = dy * sh // dh
        y1 = max(y0 + 1, (dy + 1) * sh // dh)
        for dx in range(dw):
            x0 = dx * sw // dw
            x1 = max(x0 + 1, (dx + 1) * sw // dw)
            ra = ga = ba = aa = 0
            n = 0
            for y in range(y0, y1):
                row = y * sw
                for x in range(x0, x1):
                    i = (row + x) * 4
                    a = rgba[i + 3]
                    ra += rgba[i] * a
                    ga += rgba[i + 1] * a
                    ba += rgba[i + 2] * a
                    aa += a
                    n += 1
            o = (dy * dw + dx) * 4
            if aa == 0:
                out[o:o + 4] = b"\x00\x00\x00\x00"
            else:
                out[o] = (ra // aa) & 0xFF
                out[o + 1] = (ga // aa) & 0xFF
                out[o + 2] = (ba // aa) & 0xFF
                out[o + 3] = (aa // n) & 0xFF
    return out


# --------------------------------------------------------------------------
# PNG encoding (RGBA8, no interlacing, filter type 0 on every row).
# --------------------------------------------------------------------------

def chunk(ctype, body):
    return (struct.pack(">I", len(body)) + ctype + body +
            struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))


def encode_png(w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# ICO assembly.
# --------------------------------------------------------------------------

def build_ico(images):
    n = len(images)
    header = struct.pack("<HHH", 0, 1, n)
    offset = 6 + 16 * n
    dirs = bytearray()
    blobs = bytearray()
    for (size, png) in images:
        b = 0 if size >= 256 else size          # 256 is stored as 0
        dirs += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(png), offset)
        blobs += png
        offset += len(png)
    return bytes(header + dirs + blobs)


def main(argv):
    if len(argv) != 3:
        print("usage: %s <source.png> <output.ico>" % argv[0], file=sys.stderr)
        return 2

    src, dst = argv[1], argv[2]
    if not os.path.isfile(src):
        log("ERROR: no such file: %s" % src)
        return 1

    w, h, rgba = read_png(src)
    dbg("source: %dx%d" % (w, h))

    if w != h:
        log("ERROR: source is not square: %d x %d" % (w, h))
        log("       Windows scales the whole canvas into each icon size, so a")
        log("       non-square source would come out distorted. Crop or pad it")
        log("       (transparent padding, centred) so that it is square.")
        return 1
    if w < 256:
        log("WARNING: source is smaller than 256 x 256 (%d x %d); the larger"
            % (w, h))
        log("         icons will be upscaled and may look soft")

    images = []
    for size in SIZES:
        img = downsample(w, h, rgba, size, size)
        png = encode_png(size, size, img)
        images.append((size, png))
        log("  %dx%d -> %d bytes" % (size, size, len(png)))

    ico = build_ico(images)

    tmp = "%s.tmp.%d" % (dst, os.getpid())
    with open(tmp, "wb") as f:
        f.write(ico)
    os.replace(tmp, dst)

    log("done: %s (%d bytes)" % (dst, len(ico)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
