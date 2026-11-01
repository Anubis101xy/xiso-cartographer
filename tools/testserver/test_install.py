"""End-to-end tests for `xcat-cli install`: streamed, resumable, verified installs.

    python tools/testserver/test_install.py --cli <xcat-cli> --fixtures <dir> --work <dir> SCENARIO
    python tools/testserver/test_install.py --list

Each scenario installs fixture images (tools/fixtures) from rangeserver.py, or
from a local file, into a scratch games folder, and checks the result against
the fixture's manifest: every file's size and SHA-256, every directory, nothing
extra but the `.xcat-installed` marker, and no journal left behind. The resume
scenarios kill the installer mid-way (as a power cut would stop it) and run it
again. Used by CTest (install_<scenario> tests).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fixtures"))

import check_extracted  # noqa: E402
import rangeserver  # noqa: E402

JOURNAL = ".xcat-journal"
MARKER = ".xcat-installed"


class Env:
    def __init__(self, cli: str, fixtures: str, work: str, name: str):
        self.cli = cli
        self.fixtures = fixtures
        self.scratch = os.path.join(work, name)
        shutil.rmtree(self.scratch, ignore_errors=True)
        self.served = os.path.join(self.scratch, "served")
        self.games = os.path.join(self.scratch, "Games")
        os.makedirs(self.served)
        self.srv = rangeserver.RangeServer(self.served)
        self.srv.start()
        self.problems: list[str] = []

    def close(self) -> None:
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.scratch, ignore_errors=True)

    def serve(self, fixture: str, as_name: str = "game.iso") -> None:
        """Serves a fixture under `as_name`: a hard link where possible, since the
        full-disc fixtures are 7-9 GB sparse files."""
        src = os.path.join(self.fixtures, f"{fixture}.iso")
        dst = os.path.join(self.served, as_name)
        for _ in range(100):  # Windows: a server thread may still be finishing a killed transfer
            try:
                if os.path.exists(dst):
                    os.remove(dst)
                break
            except PermissionError:
                time.sleep(0.05)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copyfile(src, dst)

    def url(self, query: str = "", name: str = "game.iso") -> str:
        return f"http://127.0.0.1:{self.srv.port}/{name}" + (f"?{query}" if query else "")

    def args(self, image: str, title: str = "Game", extra: list[str] | None = None) -> list[str]:
        return [self.cli, "install", "--timeout", "2000", *(extra or []), image, self.games, title]

    def run(self, image: str, title: str = "Game",
            extra: list[str] | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(self.args(image, title, extra), capture_output=True, text=True,
                                timeout=300, check=False)
        print(f"$ xcat-cli install {image} {title}\n{result.stdout}{result.stderr}", end="")
        return result

    def expect(self, cond: bool, what: str) -> None:
        if not cond:
            self.problems.append(what)

    def check_installed(self, fixture: str, folder: str = "Game") -> None:
        root = os.path.join(self.games, folder)
        self.expect(os.path.isfile(os.path.join(root, MARKER)), f"{folder}: no {MARKER}")
        self.expect(not os.path.exists(os.path.join(root, JOURNAL)), f"{folder}: journal left behind")
        if os.path.exists(os.path.join(root, MARKER)):
            os.remove(os.path.join(root, MARKER))  # the manifest check allows no extra files
        status = check_extracted.main([os.path.join(self.fixtures, f"{fixture}.json"), root])
        self.expect(status == 0, f"{folder}: differs from the {fixture} manifest")

    def kill_after_done_lines(self, image: str, lines: int, title: str = "Game") -> None:
        """Starts an install and kills it once the journal has `lines` done records."""
        self.kill_when(image, lambda js: sum(1 for line in js if line.startswith("done ")) >= lines,
                       title)

    def kill_when(self, image: str, ready, title: str = "Game",
                  extra: list[str] | None = None) -> None:
        """Starts an install and kills it once ready(journal lines) is true."""
        journal = os.path.join(self.games, title, JOURNAL)
        proc = subprocess.Popen(self.args(image, title, extra), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with open(journal, encoding="ascii", errors="replace") as f:
                    if ready(f.read().splitlines()):
                        break
            except OSError:
                pass
            time.sleep(0.02)
        finished = proc.poll() is not None
        proc.kill()
        out, _ = proc.communicate()
        print(f"$ xcat-cli install {image} (killed)\n{out}", end="")
        self.expect(not finished, "the install finished before it could be interrupted")


def image_hashes(path: str) -> tuple[str, str]:
    """The image file's MD5 and SHA-1, as xcat-cli prints them (journal.md section 7)."""
    md5, sha1 = hashlib.md5(), hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            md5.update(chunk)
            sha1.update(chunk)
    return md5.hexdigest(), sha1.hexdigest()


def expect_hash(e: Env, out: str, fixture: str, restarted: bool = False) -> None:
    md5, sha1 = image_hashes(os.path.join(e.fixtures, f"{fixture}.iso"))
    e.expect(f"image md5 {md5} sha1 {sha1}" in out, f"{fixture}: wrong or missing image hashes")
    e.expect(("hashed from the start again" in out) == restarted,
             f"{fixture}: hashing {'should' if restarted else 'should not'} have started over")


def done_files(journal: str) -> list[str]:
    with open(journal, encoding="ascii") as f:
        return [line.split(" ")[3] for line in f if line.startswith("done ")]


# --- scenarios ------------------------------------------------------------------------------


def s_fixtures(e: Env) -> None:
    """Every installable fixture over HTTP, including the full-disc layouts."""
    for fixture in ["small", "redump-xgd1", "redump-xgd2", "redump-xgd3"]:
        e.serve(fixture, f"{fixture}.iso")
        r = e.run(e.url(name=f"{fixture}.iso"), fixture)
        e.expect(r.returncode == 0, f"{fixture}: exit {r.returncode}")
        e.expect("verified" in r.stdout, f"{fixture}: not verified")
        if fixture == "small":
            expect_hash(e, r.stdout, fixture)
        else:  # full-disc layouts: sizes and CRC-32 only (ADR 0004 section 5)
            e.expect("image not hashed: full-disc layout" in r.stdout, f"{fixture}: hashed?")
        e.check_installed(fixture, fixture)
    data = [r for r in e.srv.requests if r.get("range") not in (None, "bytes=0-0")]
    print(f"{len(e.srv.requests)} requests, {len(data)} with data")


def s_local_image(e: Env) -> None:
    r = e.run(os.path.join(e.fixtures, "small.iso"))
    e.expect(r.returncode == 0, f"exit {r.returncode}")
    e.check_installed("small")


def s_title_from_url(e: Env) -> None:
    e.serve("small", "My Game (Europe).xiso.iso")
    r = subprocess.run([e.cli, "install", e.url(name="My%20Game%20(Europe).xiso.iso"), e.games],
                       capture_output=True, text=True, timeout=120, check=False)
    print(r.stdout + r.stderr, end="")
    e.expect(r.returncode == 0, f"exit {r.returncode}")
    e.check_installed("small", "My Game (Europe)")


def s_drops(e: Env) -> None:
    e.serve("small")
    r = e.run(e.url("drop=100000&times=3"))
    e.expect(r.returncode == 0, f"exit {r.returncode}")
    e.expect("retrying" in r.stderr, "no retries reported")
    e.check_installed("small")


def s_not_installable(e: Env) -> None:
    e.serve("deep")
    r = e.run(e.url())
    e.expect(r.returncode == 3, f"exit {r.returncode}, expected 3")
    e.expect("not installable" in r.stdout, "no 'not installable' message")
    e.expect(not os.path.exists(os.path.join(e.games, "Game")), "a folder was created")


def s_twice(e: Env) -> None:
    e.serve("small")
    e.expect(e.run(e.url()).returncode == 0, "first install failed")
    r = e.run(e.url())
    e.expect(r.returncode == 0, "second install failed")
    e.expect("already installed there" in r.stderr, "the existing folder isn't mentioned")
    e.check_installed("small", "Game")
    e.check_installed("small", "Game (2)")


def s_resume_after_kill(e: Env) -> None:
    e.serve("small")
    e.kill_after_done_lines(e.url("rate=60000"), 2)
    journal = os.path.join(e.games, "Game", JOURNAL)
    e.expect(os.path.exists(journal), "no journal after the interruption")
    before = done_files(journal) if os.path.exists(journal) else []
    r = e.run(e.url())
    e.expect(r.returncode == 0, f"resume: exit {r.returncode}")
    m = re.search(r"(\d+) kept from an earlier attempt, resumed", r.stdout)
    e.expect(m is not None and int(m.group(1)) >= len(before) >= 2,
             f"didn't resume with the {len(before)} finished files")
    e.expect(not os.path.exists(os.path.join(e.games, "Game (2)")), "a second folder was made")
    # No hash line yet (the default 64 MiB between them): hashing starts over (journal.md 6.7).
    expect_hash(e, r.stdout, "small", restarted=True)
    e.check_installed("small")


def s_resume_other_image(e: Env) -> None:
    e.serve("small")
    e.kill_after_done_lines(e.url("rate=60000"), 1)
    e.serve("redump-xgd2")  # same tree, different image: the old journal no longer matches
    r = e.run(e.url())
    e.expect(r.returncode == 0, f"exit {r.returncode}")
    e.expect("different image" in r.stderr, "the reason for a new folder isn't given")
    e.check_installed("redump-xgd2", "Game (2)")
    e.expect(os.path.exists(os.path.join(e.games, "Game", JOURNAL)),
             "the other image's unfinished install was touched")


def s_verify_catches_damage(e: Env) -> None:
    e.serve("small")
    e.kill_after_done_lines(e.url("rate=60000"), 2)
    journal = os.path.join(e.games, "Game", JOURNAL)
    done = [p for p in done_files(journal) if p != ""]
    # Damage a finished file without changing its size: only the read-back can tell.
    victim = None
    for p in done:
        path = os.path.join(e.games, "Game", *urllib.parse.unquote(p).split("/"))
        if os.path.getsize(path) > 0:
            victim = path
            break
    e.expect(victim is not None, "no non-empty finished file to damage")
    if victim is None:
        return
    with open(victim, "r+b") as f:
        b = f.read(1)
        f.seek(0)
        f.write(bytes([b[0] ^ 0xFF]))
    r = e.run(e.url())
    e.expect(r.returncode == 1, f"damaged install: exit {r.returncode}, expected 1")
    e.expect("doesn't match" in r.stderr, "the damaged file isn't reported")
    r = e.run(e.url())
    e.expect(r.returncode == 0, f"retry after damage: exit {r.returncode}")
    e.check_installed("small")


def s_resume_inside_file(e: Env) -> None:
    """ADR 0011: a kill after a checkpoint inside a file continues that file, not from 0."""
    e.serve("small")
    cp = ["--checkpoint", "16384"]
    e.kill_when(e.url("rate=60000"),
                lambda js: any(j.startswith("part ") and " media/intro.xmv " in j for j in js)
                and any(j.startswith("hash ") for j in js),
                extra=cp)
    r = e.run(e.url(), extra=cp)
    e.expect(r.returncode == 0, f"resume: exit {r.returncode}")
    m = re.search(r"(\d+) files? continued from a checkpoint \((\d+) bytes kept\)", r.stdout)
    e.expect(m is not None and int(m.group(2)) >= 16384, "no file was continued from a checkpoint")
    # The hash state saved with the checkpoint carried over (journal.md 6.7).
    expect_hash(e, r.stdout, "small", restarted=False)
    e.check_installed("small")


def s_hash_expected(e: Env) -> None:
    """journal.md section 7: expected hashes that match pass; a wrong one fails the install."""
    e.serve("small")
    md5, sha1 = image_hashes(os.path.join(e.fixtures, "small.iso"))
    r = e.run(e.url(), "Good", ["--md5", md5.upper(), "--sha1", sha1])
    e.expect(r.returncode == 0, f"exit {r.returncode}")
    e.expect("matches what was expected" in r.stdout, "the match isn't reported")
    e.check_installed("small", "Good")
    wrong = ("0" if md5[0] != "0" else "1") + md5[1:]
    r = e.run(e.url(), "Bad", ["--md5", wrong])
    e.expect(r.returncode == 1, f"wrong md5: exit {r.returncode}, expected 1")
    e.expect("doesn't match its published checksum" in r.stderr, "no checksum message")
    e.expect(f"image md5 {md5} " in r.stdout, "the computed hashes aren't shown")
    bad = os.path.join(e.games, "Bad")
    e.expect(not os.path.exists(os.path.join(bad, MARKER)), "a marker was written")
    e.expect(os.path.exists(os.path.join(bad, JOURNAL)), "the journal wasn't kept")
    r = subprocess.run([e.cli, "install", "--md5", "xyz", e.url(), e.games], capture_output=True,
                       text=True, timeout=60, check=False)
    e.expect(r.returncode == 2, f"bad --md5 value: exit {r.returncode}, expected 2")


TLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures", "tls")


def s_https(e: Env) -> None:
    """Installs over HTTPS (ADR 0012) from a TLS server with the test PKI."""
    tls = rangeserver.tls_context(os.path.join(TLS_DIR, "server-chain.pem"),
                                  os.path.join(TLS_DIR, "server.key.der"))
    srv = rangeserver.RangeServer(e.served, tls=tls)
    srv.start()
    try:
        for fixture in ["small", "redump-xgd2"]:
            e.serve(fixture, f"{fixture}.iso")
            r = e.run(f"https://localhost:{srv.port}/{fixture}.iso", fixture,
                      ["--ca-file", os.path.join(TLS_DIR, "ca.pem")])
            e.expect(r.returncode == 0, f"{fixture}: exit {r.returncode}")
            e.expect("verified" in r.stdout, f"{fixture}: not verified")
            e.check_installed(fixture, fixture)
    finally:
        srv.shutdown()
        srv.server_close()


def serve_file(e: Env, name: str, as_name: str | None = None) -> str:
    """Serves fixtures/<name> (any file) and returns its URL."""
    dst = os.path.join(e.served, as_name or name)
    shutil.copyfile(os.path.join(e.fixtures, name), dst)
    return e.url(name=urllib.parse.quote(as_name or name))


ZIPS = [  # (zip, the fixture inside it)
    ("small-stored.zip", "small"),
    ("small-deflate.zip", "small"),
    ("late-stored.zip", "late"),
    ("late-deflate.zip", "late"),
    ("late-deflate0.zip", "late"),
]


def s_zip_fixtures(e: Env) -> None:
    """Zips over HTTP (docs/formats/zip.md): stored members with Range like a bare image,
    Deflate members from a stream, late tables through the spool (stream-install.md)."""
    for name, fixture in ZIPS:
        title = name[:-4]
        r = e.run(serve_file(e, name), title)
        e.expect(r.returncode == 0, f"{name}: exit {r.returncode}")
        e.expect(f"from the zip: {fixture}.iso, " in r.stdout, f"{name}: no zip summary")
        if "deflate" in name:
            e.expect("checked by the member's CRC-32" in r.stdout, f"{name}: CRC-32 not mentioned")
        if name.startswith("late-deflate"):
            e.expect(re.search(r"[1-9]\d* files? from a", r.stdout) is not None,
                     f"{name}: no files came from the spool")
        e.check_installed(fixture, title)
        leftovers = [f for f in os.listdir(os.path.join(e.games, title)) if f.startswith(".xcat-spool")]
        e.expect(not leftovers, f"{name}: spool files left behind: {leftovers}")


def s_zip_https(e: Env) -> None:
    """A Deflate zip with late tables over HTTPS."""
    tls = rangeserver.tls_context(os.path.join(TLS_DIR, "server-chain.pem"),
                                  os.path.join(TLS_DIR, "server.key.der"))
    srv = rangeserver.RangeServer(e.served, tls=tls)
    srv.start()
    try:
        serve_file(e, "late-deflate.zip")
        r = e.run(f"https://localhost:{srv.port}/late-deflate.zip", "Late",
                  ["--ca-file", os.path.join(TLS_DIR, "ca.pem")])
        e.expect(r.returncode == 0, f"exit {r.returncode}")
        e.check_installed("late", "Late")
    finally:
        srv.shutdown()
        srv.server_close()


def s_zip_resume(e: Env) -> None:
    """journal.md 6.8: an interrupted Deflate zip install keeps its finished files."""
    url = serve_file(e, "small-deflate0.zip")
    e.kill_after_done_lines(url + "?rate=60000", 2)
    journal = os.path.join(e.games, "Game", JOURNAL)
    e.expect(os.path.exists(journal), "no journal after the interruption")
    if os.path.exists(journal):
        with open(journal, encoding="ascii") as f:
            e.expect(re.search(r"^zip 786432 [0-9A-F]{8} \d+ \*", f.read(), re.M) is not None,
                     "the journal has no zip identity line")
    r = e.run(url)
    e.expect(r.returncode == 0, f"resume: exit {r.returncode}")
    m = re.search(r"(\d+) kept from an earlier attempt, resumed", r.stdout)
    e.expect(m is not None and int(m.group(1)) >= 2, "didn't keep the finished files")
    e.check_installed("small")


def s_zip_resume_mid_stream(e: Env) -> None:
    """stream-install.md 7: a kill after a saved state continues the stream there, not from 0."""
    cp = ["--checkpoint", "65536"]
    for zip_name, fixture, states in (("small-deflate.zip", "small", 3),
                                      ("late-deflate.zip", "late", 6)):
        url = serve_file(e, zip_name)
        title = fixture.capitalize()
        e.kill_when(url + "?rate=60000",
                    lambda js, n=states: sum(1 for j in js if j.startswith("state ")) >= n,
                    title, cp)
        folder = os.path.join(e.games, title)
        e.expect(any(n.startswith(".xcat-state-") for n in os.listdir(folder)),
                 f"{zip_name}: no state file after the interruption")
        r = e.run(url, title, cp)
        e.expect(r.returncode == 0, f"{zip_name}: resume: exit {r.returncode}")
        m = re.search(r"continued ([0-9.]+) MiB into the image", r.stdout)
        e.expect(m is not None and float(m.group(1)) > 0,
                 f"{zip_name}: the stream started again from the start")
        left = [n for n in os.listdir(folder) if n.startswith((".xcat-state-", ".xcat-spool-",
                                                               ".xcat-zip-"))]
        e.expect(not left, f"{zip_name}: files left behind: {left}")
        e.check_installed(fixture, title)


def s_zip_refused(e: Env) -> None:
    """Zips the installer can't use are refused with a reason (zip.md 2, 5, 6)."""
    import zipfile

    two = os.path.join(e.served, "two.zip")
    with zipfile.ZipFile(two, "w") as z:
        z.writestr("a.iso", b"a")
        z.writestr("b.iso", b"b")
    r = e.run(e.url(name="two.zip"), "Two")
    e.expect(r.returncode == 1, f"two images: exit {r.returncode}")
    e.expect("one .iso or .xiso image" in r.stderr, "two images: no reason given")

    lzma = os.path.join(e.served, "lzma.zip")
    with zipfile.ZipFile(lzma, "w") as z:
        z.writestr("game.iso", b"not really LZMA")
    data = bytearray(open(lzma, "rb").read())
    cd = data.rfind(b"PK\x01\x02")
    data[8:10] = (14).to_bytes(2, "little")  # local header method
    data[cd + 10:cd + 12] = (14).to_bytes(2, "little")  # central directory method
    open(lzma, "wb").write(bytes(data))
    r = e.run(e.url(name="lzma.zip"), "Lzma")
    e.expect(r.returncode == 1, f"LZMA: exit {r.returncode}")
    e.expect("game.iso in the zip is LZMA (method 14)" in r.stderr, "LZMA: method not named")

    r = e.run(serve_file(e, "small.iso", "fake.zip"), "Fake")
    e.expect(r.returncode == 1, f"not a zip: exit {r.returncode}")
    e.expect("Not a zip file" in r.stderr, "not a zip: no reason given")
    for title in ("Two", "Lzma", "Fake"):
        e.expect(not os.path.exists(os.path.join(e.games, title, MARKER)), f"{title}: marked done")


def s_zip_damaged(e: Env) -> None:
    """A changed byte in a Deflate member is caught (zip.md 9.3), and nothing is marked done."""
    dst = os.path.join(e.served, "damaged.zip")
    shutil.copyfile(os.path.join(e.fixtures, "small-deflate.zip"), dst)
    data = bytearray(open(dst, "rb").read())
    data[len(data) // 2] ^= 0x10
    open(dst, "wb").write(bytes(data))
    r = e.run(e.url(name="damaged.zip"))
    e.expect(r.returncode == 1, f"exit {r.returncode}")
    e.expect("CRC-32" in r.stderr or "violates its format" in r.stderr or "corrupt" in r.stderr.lower(),
             "the damage isn't reported")
    e.expect(not os.path.exists(os.path.join(e.games, "Game", MARKER)), "marked done")


SCENARIOS = {
    "fixtures": s_fixtures,
    "zip_fixtures": s_zip_fixtures,
    "zip_https": s_zip_https,
    "zip_resume": s_zip_resume,
    "zip_resume_mid_stream": s_zip_resume_mid_stream,
    "zip_refused": s_zip_refused,
    "zip_damaged": s_zip_damaged,
    "https": s_https,
    "local_image": s_local_image,
    "title_from_url": s_title_from_url,
    "drops": s_drops,
    "not_installable": s_not_installable,
    "twice": s_twice,
    "resume_after_kill": s_resume_after_kill,
    "resume_other_image": s_resume_other_image,
    "verify_catches_damage": s_verify_catches_damage,
    "resume_inside_file": s_resume_inside_file,
    "hash_expected": s_hash_expected,
}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli")
    ap.add_argument("--fixtures")
    ap.add_argument("--work")
    ap.add_argument("--list", action="store_true", help="print the scenario names (for CMake)")
    ap.add_argument("scenario", nargs="?", choices=sorted(SCENARIOS))
    args = ap.parse_args(argv)
    if args.list:
        print(";".join(SCENARIOS))
        return 0
    if not (args.cli and args.fixtures and args.work and args.scenario):
        ap.error("--cli, --fixtures, --work and a scenario are required")
    e = Env(args.cli, args.fixtures, args.work, args.scenario)
    try:
        SCENARIOS[args.scenario](e)
    finally:
        e.close()
    for p in e.problems:
        print(f"FAIL {args.scenario}: {p}")
    return 1 if e.problems else 0


if __name__ == "__main__":
    sys.exit(main())
