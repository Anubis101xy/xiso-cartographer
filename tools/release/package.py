#!/usr/bin/env python3
"""Packs a release of the app: the zip that goes on a GitHub release (docs/release.md, ADR 0017).

The zip holds one folder, xiso-cartographer\\, to copy to the console as it is: the XBE and the
files it needs next to it (fonts\\, ui\\), a README, the project's licence, and the licence notice
of every third-party component the XBE contains (licenses\\, THIRD_PARTY.txt). The bundled test
image (samples\\) is left out. The same inputs always give the same bytes: entries are sorted and
dated SOURCE_DATE_EPOCH (or 1980-01-01). Standard library only.

usage: package.py --bin app/bin --out dist [--tag vX.Y.Z] [--root .]
"""

import argparse
import hashlib
import os
import re
import sys
import time
import zipfile

NAME = "xiso-cartographer"

# Every third-party component in the XBE (THIRD_PARTY.md, "Ships in release": Yes), and where its
# licence text comes from: a file in the repo or a submodule, under the root.
LICENSES = [
    ("BearSSL.txt", "BearSSL (TLS, hashes): MIT", "third_party/bearssl/LICENSE.txt"),
    ("miniz.txt", "miniz (Deflate): MIT", "third_party/miniz/LICENSE"),
    ("nxdk.txt", "nxdk (runtime, hal, pbkit, winapi, xboxrt): MIT, the nxdk contributors",
     "third_party/nxdk/LICENSES/MIT.txt"),
    ("PDCLib.txt", "PDCLib (C library): CC0", "third_party/nxdk/lib/pdclib/COPYING.CC0"),
    ("lwIP.txt", "lwIP (network stack): Modified BSD", "third_party/nxdk/lib/net/lwip/COPYING"),
    ("SDL2.txt", "SDL2 (controllers, drawing): zlib", "third_party/nxdk/lib/sdl/SDL2/COPYING.txt"),
    ("SDL_ttf.txt", "SDL_ttf (text): zlib", "third_party/nxdk/lib/sdl/SDL_ttf/COPYING.txt"),
    ("FreeType.txt", "FreeType 2.4.12, inside SDL_ttf: FreeType License (FTL)",
     "third_party/nxdk/lib/sdl/SDL_ttf/external/freetype-2.4.12/docs/FTL.TXT"),
    ("libusbohci.txt", "libusbohci (USB host, for the controllers): Apache-2.0, Nuvoton",
     "third_party/nxdk/lib/usb/libusbohci/LICENSE.txt"),
    ("libusbohci-DISCLAIMER.txt", "libusbohci's disclaimer",
     "third_party/nxdk/lib/usb/libusbohci/DISCLAIMER.md"),
    ("libsmb2.txt", "libsmb2 (NAS shared folders, SMB2/3): LGPL-2.1, Ronnie Sahlberg; its source: "
     "licenses\\libsmb2-source.txt", "third_party/libsmb2/LICENCE-LGPL-2.1.txt"),
    ("LZMA-SDK.txt", "LZMA SDK, 7z decoder (7z archives): public domain, Igor Pavlov",
     "third_party/lzma/7zC.txt"),
    ("stb_image.txt", "stb_image (JPEG and PNG covers): public domain or MIT, Sean Barrett",
     "third_party/stb/LICENSE"),
]

# The fonts' OFL texts ship in fonts\ next to them, as the build copies them.
FONT_NOTICE = "Noto Sans, Michroma, Chakra Petch (fonts\\): SIL Open Font License 1.1, in fonts\\"

FREETYPE_CREDIT = ("Portions of this software are copyright (C) 1996-2013 The FreeType Project "
                   "(www.freetype.org). All rights reserved.")

CA_NOTICE = """Mozilla CA certificates (the built-in TLS trust anchors)

The XBE contains Mozilla's root certificate store as curl publishes it (cacert.pem, Mozilla data
as of the date in the project's third_party/cacert/README.md), unmodified. It is under the Mozilla
Public License 2.0: https://www.mozilla.org/en-US/MPL/2.0/

Its source form is available from the curl project, https://curl.se/docs/caextract.html, and in
the xiso-cartographer repository at third_party/cacert/cacert.pem for this release's tag.
"""

SMB_NOTICE = """libsmb2 (NAS shared folders over SMB2/3)

default.xbe contains libsmb2 by Ronnie Sahlberg, statically linked, under the GNU Lesser General
Public License 2.1 (licenses\\libsmb2.txt), with a small port to the console's network stack.

Its source, with the port, is in the xiso-cartographer repository at third_party/libsmb2/ for
this release's tag (third_party/libsmb2/xcat/README.md says what the port changes, and
nxdk.patch is the port as a diff against upstream commit 727feaa). Upstream:
https://github.com/sahlberg/libsmb2

You may change libsmb2 and build default.xbe again with your version: docs/build-xbox.md in the
repository says how. The rest of xiso-cartographer is under the MIT licence (LICENSE.txt), so its
source is there to relink with too.
"""

README = """xiso-cartographer {version}

An installer for the original Xbox: it lists the games your map file names, and installs the
one you pick to the console's hard disk, over the network, resuming where it stopped.

Install
  Copy this xiso-cartographer folder to the console, for example to E:\\Apps\\xcat\\, and start
  default.xbe from your dashboard. Keep fonts\\ and ui\\ next to it.

What it needs
  A map, E:\\xcat\\map.txt, that lists what to install (it can be added on the console, in
  SETTINGS > Map sources), and for archive.org items a login (SETTINGS > Authentication).
  Games on a NAS or a PC's shared folder: SETTINGS > NAS finds it, logs in and adds the folder.
  The game catalog, catalog.bin, comes with each release as a separate download: put it in
  E:\\Apps\\xcat\\ so games show their names and details.

Nothing is included or downloaded that you don't choose: the map is yours.

More: https://github.com/jmherren726/xiso-cartographer (docs/build-xbox.md: what the app does).
Licence: LICENSE.txt (MIT). Third-party components and their licences: THIRD_PARTY.txt.
"""


def version(root):
    """The version in core/include/xcat/version.h, "MAJOR.MINOR.PATCH"."""
    with open(os.path.join(root, "core", "include", "xcat", "version.h"), encoding="utf-8") as f:
        text = f.read()
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"^#define XC_VERSION_%s (\d+)$" % part, text, re.M)
        if m is None:
            raise ValueError("version.h has no XC_VERSION_%s" % part)
        parts.append(m.group(1))
    return ".".join(parts)


def third_party_txt():
    lines = ["Third-party components in default.xbe, and their licences (in licenses\\).", ""]
    for dest, what, _src in LICENSES:
        lines.append("%s: licenses\\%s" % (what, dest))
    lines.append("Mozilla CA certificates (TLS trust anchors): MPL-2.0: licenses\\Mozilla-CA.txt")
    lines.append(FONT_NOTICE)
    lines += ["", "FreeType's credit, as its licence asks:", FREETYPE_CREDIT, ""]
    return "\r\n".join(lines)


def crlf(text):
    """Text for the console and Windows: CRLF line endings."""
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def files(bin_dir, root, ver):
    """{path in the zip: bytes}."""
    out = {}
    for dirpath, dirnames, names in os.walk(bin_dir):
        rel = os.path.relpath(dirpath, bin_dir)
        if rel.split(os.sep)[0] == "samples":
            continue
        for n in names:
            path = os.path.normpath(os.path.join(rel, n)).replace(os.sep, "/")
            with open(os.path.join(dirpath, n), "rb") as f:
                out[path] = f.read()
    if "default.xbe" not in out:
        raise ValueError("%s has no default.xbe: build the XBE first" % bin_dir)
    if not any(p.startswith("fonts/") for p in out):
        raise ValueError("%s has no fonts\\" % bin_dir)
    out["README.txt"] = crlf(README.format(version=ver))
    with open(os.path.join(root, "LICENSE"), "rb") as f:
        out["LICENSE.txt"] = crlf(f.read().decode("utf-8"))
    out["THIRD_PARTY.txt"] = crlf(third_party_txt())
    for dest, _what, src in LICENSES:
        p = os.path.join(root, src)
        if not os.path.isfile(p):
            raise ValueError("missing licence %s (submodules checked out recursively?)" % src)
        with open(p, "rb") as f:
            out["licenses/" + dest] = crlf(f.read().decode("utf-8", errors="replace"))
    out["licenses/Mozilla-CA.txt"] = crlf(CA_NOTICE)
    out["licenses/libsmb2-source.txt"] = crlf(SMB_NOTICE)
    return {NAME + "/" + k: v for k, v in out.items()}


def write_zip(path, entries, epoch):
    stamp = time.gmtime(max(epoch, 315532800))[:6]  # zip can't date before 1980
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 3
            z.writestr(info, entries[name])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="the XBE's folder, app/bin")
    ap.add_argument("--out", required=True, help="where the zip goes")
    ap.add_argument("--tag", help="the release's tag, which must be v<version.h's version>")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    args = ap.parse_args(argv[1:])
    try:
        ver = version(args.root)
        if args.tag is not None and args.tag != "v" + ver:
            raise ValueError("tag %s doesn't match version.h (%s): bump the version first" % (args.tag, ver))
        entries = files(args.bin, args.root, ver)
    except (OSError, ValueError) as e:
        print("package.py: %s" % e, file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, "%s-%s.zip" % (NAME, ver))
    write_zip(zip_path, entries, int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    with open(zip_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    with open(zip_path + ".sha256", "w", encoding="ascii", newline="\n") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(zip_path)))
    print("%s: %d files, sha256 %s" % (zip_path, len(entries), digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
