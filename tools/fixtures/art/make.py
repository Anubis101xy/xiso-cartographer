#!/usr/bin/env python3
"""Makes the art fixtures in core/tests/data/art/ (docs/formats/art.md, xpr.md, xbe.md).

- title.xbe: a minimal XBE (headers, a certificate with a region, disc and version, one empty
  section), given a $$XTIMAGE section by tools/ui/xbe_title_image.py, the same code that gives
  the app's own XBE its title image; title.png is the 128 x 128 picture in it. test_picture
  checks that decoding the section gives the picture, within DXT1's error.
- cover.png: 3 x 2 RGBA, every pixel known exactly. cover.jpg, progressive.jpg: 16 x 16, four
  flat quadrants (Pillow's encoder; checked within JPEG's error). huge.png: a PNG header that
  says 4000 x 4000, for the size limit.

It also writes the seeds of fuzz_picture, fuzz_xpr and fuzz_synopsis (core/tests/fuzz-seeds/),
and the XBE as a seed of fuzz_xbe.

The PNGs and the XBE come from the standard library; the JPEGs need Pillow. Run it from the
repository's root; it overwrites the files.
"""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools", "ui"))
import xbe_title_image  # noqa: E402

OUT = os.path.join(ROOT, "core", "tests", "data", "art")
SEEDS = os.path.join(ROOT, "core", "tests", "fuzz-seeds")

SYNOPSIS = (b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<synopsis>\r\n"
            b"<title>Seed Game &amp; Friends</title>\r\n<developer>A Studio</developer>\r\n"
            b"<publisher><![CDATA[A <Publisher>]]></publisher>\r\n<genre>Action</genre>\r\n"
            b"<year>2004</year>\r\n<esrb>T</esrb>\r\n<rating>8.0</rating>\r\n"
            b"<features_general>Players 1-4</features_general>\r\n"
            b"<titleid>58430003</titleid>\r\n<overview>Two lines\r\n of text &#233;.</overview>\r\n"
            b"</synopsis>\r\n")


def xpr(colour, lw, lh, size, data):
    """An XPR0 texture: the header (0x40 bytes) and its data."""
    head = bytearray(0x40)
    head[0:4] = b"XPR0"
    fmt = 0x29 | colour << 8 | 1 << 16 | lw << 20 | lh << 24
    struct.pack_into("<7I", head, 4, 0x40 + len(data), 0x40, 0x00040001, 0, 0, fmt, size)
    struct.pack_into("<I", head, 0x20, 0xFFFFFFFF)
    return bytes(head) + data


def seeds(xbe, files):
    def write(kind, name, data):
        os.makedirs(os.path.join(SEEDS, kind), exist_ok=True)
        with open(os.path.join(SEEDS, kind, name + ".seed"), "wb") as f:
            f.write(data)

    for name in ("cover.png", "cover.jpg", "progressive.jpg", "huge.png"):
        write("picture", name.replace(".", "-"), files[name])
    base, = struct.unpack_from("<I", xbe, 0x104)
    count, at = struct.unpack_from("<2I", xbe, 0x11C)
    last = xbe_title_image.SECTION_HEADER.unpack_from(xbe, at - base + (count - 1) * 56)
    write("xpr", "title-dxt1", xbe[last[3]:last[3] + last[4]])
    write("xpr", "dxt3", xpr(0x0E, 3, 2, 0, bytes(range(256)) * 2))
    write("xpr", "dxt5", xpr(0x0F, 2, 3, 0, bytes(range(255, -1, -1)) * 2))
    write("xpr", "argb-swizzled", xpr(0x06, 3, 2, 0, bytes(range(128)) * 2))
    write("xpr", "rgb565-swizzled", xpr(0x05, 2, 2, 0, bytes(range(32))))
    write("xpr", "lin-argb", xpr(0x12, 0, 0, 2 | 1 << 12, bytes(64 + 12)))
    write("synopsis", "full", SYNOPSIS)
    write("synopsis", "short", b"<synopsis><title>x</title></synopsis>")
    write("xbe", "title-image", xbe)


def png(width, height, rows, alpha):
    """rows: lists of (r, g, b[, a]) tuples."""
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def minimal_xbe():
    base = 0x10000
    headers = 0x600  # up to the section's name; the section starts at 0x1000, as cxbe's do
    xbe = bytearray(0x2000)
    xbe[0:4] = b"XBEH"
    struct.pack_into("<3I", xbe, 0x104, base, headers, 0x2000)
    struct.pack_into("<I", xbe, 0x118, base + 0x180)        # the certificate
    struct.pack_into("<2I", xbe, 0x11C, 1, base + 0x400)    # one section, its header
    cert = 0x180
    struct.pack_into("<I", xbe, cert, 0x1D0)
    struct.pack_into("<I", xbe, cert + 8, 0x58430001)        # "XC", 1
    for i, ch in enumerate("Art Fixture"):
        struct.pack_into("<H", xbe, cert + 0x0C + 2 * i, ord(ch))
    struct.pack_into("<I", xbe, cert + 0xA0, 0x4)            # the rest of the world
    struct.pack_into("<I", xbe, cert + 0xA8, 1)              # the second disc
    struct.pack_into("<I", xbe, cert + 0xAC, 0x102)
    # .text: flags, vaddr, vsize, raw, raw size, name, refs, head, tail, digest
    xbe_title_image.SECTION_HEADER.pack_into(xbe, 0x400, 0x6, base + 0x1000, 0x1000, 0x1000,
                                             0x1000, base + 0x500, 0, base + 0x508,
                                             base + 0x50A, bytes(20))
    xbe[0x500:0x506] = b".text\0"
    return bytes(xbe)


def title_png():
    rows = [[(x * 2, y * 2, 255 - x - y, 255) for x in range(128)] for y in range(128)]
    return png(128, 128, rows, True)


def main():
    os.makedirs(OUT, exist_ok=True)
    picture = title_png()
    xbe = xbe_title_image.add_section(minimal_xbe(), xbe_title_image.SECTION_NAME,
                                      xbe_title_image.title_image(picture),
                                      xbe_title_image.FLAG_INSERTED_FILE)
    files = {
        "title.xbe": xbe,
        "title.png": picture,
        "cover.png": png(3, 2, [[(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 0)],
                                [(10, 20, 30, 255), (200, 100, 50, 255), (1, 2, 3, 4)]], True),
    }
    huge = bytearray(png(1, 1, [[(0, 0, 0)]], False))
    struct.pack_into(">II", huge, 16, 4000, 4000)  # IHDR's size, its CRC fixed below
    struct.pack_into(">I", huge, 29, zlib.crc32(bytes(huge[12:29])))
    files["huge.png"] = bytes(huge)
    for name, data in files.items():
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)

    from PIL import Image  # only for the JPEGs
    im = Image.new("RGB", (16, 16))
    colours = [(200, 30, 30), (30, 200, 30), (30, 30, 200), (230, 230, 230)]
    for y in range(16):
        for x in range(16):
            im.putpixel((x, y), colours[(y >= 8) * 2 + (x >= 8)])
    im.save(os.path.join(OUT, "cover.jpg"), quality=95, subsampling=0)
    im.save(os.path.join(OUT, "progressive.jpg"), quality=95, subsampling=0, progressive=True)
    for name in ("cover.jpg", "progressive.jpg"):
        with open(os.path.join(OUT, name), "rb") as f:
            files[name] = f.read()
    seeds(xbe, files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
