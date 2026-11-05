#!/usr/bin/env python3
"""Makes core/tests/data/zip/app-folder.zip: an app folder in a zip (folder-image.md §7), with
Deflate and stored members, a '\\' name, a folder entry, macOS clutter and a file outside the
app's folder. Synthetic bytes (make.py's pattern()), never game data. Standard library only.

    python3 tools/fixtures/sevenz/make_zip.py core/tests/data/zip
"""
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make import pattern  # noqa: E402

FIXED = (1980, 1, 1, 0, 0, 0)


def add(z, name, data, method):
    info = zipfile.ZipInfo(name, FIXED)
    info.compress_type = method
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def main(argv):
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(os.path.join(out, "app-folder.zip"), "w") as z:
        add(z, "readme.txt", b"outside the app's folder", zipfile.ZIP_STORED)
        add(z, "My App/", b"", zipfile.ZIP_STORED)
        add(z, "My App/default.xbe", pattern(1, 70000), zipfile.ZIP_DEFLATED)
        add(z, "My App\\skins\\big.bin", pattern(2, 300000), zipfile.ZIP_DEFLATED)
        add(z, "My App/skins/noise.bin", pattern(3, 20000, True), zipfile.ZIP_STORED)
        add(z, "My App/empty.txt", b"", zipfile.ZIP_STORED)
        add(z, "My App/.DS_Store", b"clutter", zipfile.ZIP_STORED)
        add(z, "__MACOSX/My App/._default.xbe", b"clutter", zipfile.ZIP_STORED)
        add(z, "My App/tools/default.xbe", pattern(4, 100), zipfile.ZIP_DEFLATED)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
