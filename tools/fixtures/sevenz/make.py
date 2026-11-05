#!/usr/bin/env python3
"""Makes the 7z test archives in core/tests/data/7z/ (docs/formats/sevenz.md).

The files inside are synthetic: byte patterns test_sevenz.c computes too (pattern() below),
never game data. The archives are made by 7-Zip's console tool, which CI doesn't have, so
they're committed; run this to remake them after changing it:

    python3 tools/fixtures/sevenz/make.py /path/to/7zz core/tests/data/7z

7-Zip 25.01 (https://github.com/ip7z/7zip, tag 25.01) made the committed ones.
"""
import os
import shutil
import subprocess
import sys
import tempfile


def pattern(k, n, random=False):
    """File k's n bytes: a pattern LZMA finds matches in, or an LCG it can't compress."""
    if random:
        out = bytearray(n)
        x = 0x12345678 + k
        for i in range(n):
            x = (x * 1103515245 + 12345) & 0xFFFFFFFF
            out[i] = (x >> 16) & 0xFF
        return bytes(out)
    return bytes(((k * 31 + i * 7 + (i >> 11)) & 0xFF) for i in range(n))


# (path, file number, size, random) - the game folder; the file number seeds its bytes.
GAME = [
    ("Game/default.xbe", 1, 70000, False),
    ("Game/media/intro.xmv", 2, 600000, False),
    ("Game/media/noise.bin", 3, 40000, True),
    ("Game/media/empty.bin", 4, 0, False),
    ("Game/data/a.txt", 5, 2049, False),
    ("Game/data/b.dat", 6, 1, False),
]


def write_tree(root, files):
    for path, k, n, rnd in files:
        p = os.path.join(root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(pattern(k, n, rnd))
    os.makedirs(os.path.join(root, "Game", "empty-dir"), exist_ok=True)


def main(argv):
    tool, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp()
    try:
        write_tree(work, GAME)
        def seven(name, *opts, files=("Game",)):
            dest = os.path.abspath(os.path.join(out, name))
            if os.path.exists(dest):
                os.remove(dest)
            subprocess.run([tool, "a", "-bso0", "-bsp0", *opts, dest, *files], cwd=work,
                           check=True)
        seven("game-lzma2-solid.7z", "-ms=on")
        seven("game-lzma2-flat.7z", "-ms=off")
        seven("game-lzma.7z", "-m0=LZMA")
        seven("game-copy.7z", "-mx0", files=("Game/default.xbe", "Game/data", "Game/empty-dir"))
        seven("game-bcj.7z", "-mf=BCJ")
        seven("game-ppmd.7z", "-m0=PPMd")
        seven("game-password.7z", "-pSecret")
        # One "image" (any bytes: the reader doesn't look inside) for xc_7z_pick_image.
        with open(os.path.join(work, "Disc.iso"), "wb") as f:
            f.write(pattern(7, 300000))
        seven("image-lzma2.7z", files=("Disc.iso",))
        seven("two-images.7z", files=("Disc.iso", "Game/default.xbe"))
        shutil.copy(os.path.join(work, "Disc.iso"), os.path.join(work, "Disc2.iso"))
        seven("several-images.7z", files=("Disc.iso", "Disc2.iso"))
    finally:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
