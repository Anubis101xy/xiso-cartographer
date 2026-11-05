# xiso-cartographer

[![CI](https://github.com/jmherren726/xiso-cartographer/actions/workflows/ci.yml/badge.svg)](https://github.com/jmherren726/xiso-cartographer/actions/workflows/ci.yml)

Homebrew game catalog and installer for the modded original Xbox. Browse the full library, then download and install your own game backups directly on the console, with streaming xiso extraction, HTTPS and resumable installs. Built with [nxdk](https://github.com/XboxDev/nxdk).

## Status

**Beta: [0.9.0](https://github.com/jmherren726/xiso-cartographer/releases), the first release.** The installer and its screens are done and have been tested in xemu and on a real console. The beta's last tests on hardware (other console revisions, hard disks, 64/128 MB and dashboards, and a login on a console that has never had one) are under way ([roadmap](docs/HANDOFF.md#41-roadmap), M9). Bug reports are welcome in [issues](https://github.com/jmherren726/xiso-cartographer/issues).

## What it does

- **Runs on a soft- or hard-modded Xbox**, started from a dashboard such as XBMC4Gamers or UnleashX. The XBE carries the app's logo for dashboards to show.
- **Lists your games with names and details.** You write a **map** that says where your disc images are: an archive.org item you have access to, a NAS's shared folder, a plain HTTP(S) server on your LAN, or images already on the hard disk. A built-in catalog of about 1,070 games and 2,200 releases (from Wikidata and Redump, text only) names them and shows their year, region, maker and genres. Search by name, year, region, maker or origin on the on-screen keyboard.
- **You choose:** the map is a catalog. Nothing installs until you pick a game and press A.
- **Installs on the console, no PC:** files are stream-extracted straight into `E:\Games\<title>` (or a folder on F: or G:, chosen in SETTINGS), with no temporary copy, so full-disc images of 7+ GB install too. Trimmed xisos, full-disc `.iso` dumps, and `.zip` files holding one image (stored or Deflate, Zip64) all work.
- **HTTPS and archive.org logins:** TLS 1.2 with certificate checks against built-in roots, the clock set from NTP. Log in to archive.org on the Xbox with the on-screen keyboard: only the session is saved, never the password. `@archiveorg <item>` in the map lists every game the item holds, with its published size and hashes.
- **Archives and apps:** `.7z` and `.zip` archives install straight from where they are, whether they hold an image or a game or app folder; apps go to `E:\Apps`. Unfinished or unwanted installs can be deleted from the console.
- **Games from your NAS:** SETTINGS > NAS finds file sharing servers on your network, logs in (only a hash of the password is kept) and adds a folder. Its images and extracted game folders install over SMB2/3, with nothing to set up on a PC ([setup guide](docs/nas-setup.md)).
- **Pictures and details from your games' own files:** a cover kept beside a game on the NAS (XBMC4Gamers' `_resources` too, with its synopsis), else the title image inside its `default.xbe`, with its real title, region and title ID. The list is in alphabetical order, long names slide into view, and the top of the screen shows the console's and the NAS's addresses and the free space ([art.md](docs/formats/art.md)).
- **PLAY:** start an installed game from the app.
- **Resumable and verified:** a pulled cable is retried, a power cut resumes where it stopped, and every file is read back and checked against its CRC-32 (and trimmed xisos against their MD5 and SHA-1).
- **Clear messages:** every error says what went wrong and what to do, with RETRY, or LOG IN when a session has expired.

## Installing

Download `xiso-cartographer-X.Y.Z.zip` and `catalog.bin` from the [latest release](https://github.com/jmherren726/xiso-cartographer/releases/latest). Copy what's in the zip's `xiso-cartographer` folder to `E:\Apps\xcat\` on the console, put `catalog.bin` in the same folder, and start `default.xbe` from your dashboard. The zip's `README.txt` has the details; [What the app does](docs/build-xbox.md#what-the-app-does) explains each screen and file. Check the download against its `.sha256` file.

## Legal stance

xiso-cartographer is a tool for installing **backups you are entitled to use**.

- It ships with **no game images, download links, ROMs, BIOS/MCPX dumps or Microsoft XDK material**, and none will be accepted into this repository, its issues or its CI.
- Every source is supplied by the user through their own map file.
- It will never include code that gets past captchas, Cloudflare or any other bot or access protection, or that violates a source's terms.
- It is built only with the open-source nxdk toolchain.

## Building

There are two builds: the portable core library with its unit tests (CMake, on a PC), and the Xbox app (nxdk, in Docker). CI builds both on every pull request, and its `xiso-cartographer-xbe-<commit>` artifact holds the app.

```sh
git clone --recursive https://github.com/jmherren726/xiso-cartographer.git
cd xiso-cartographer
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
```

You need CMake 3.25+, Ninja, Python 3.10+ and a C11 compiler: gcc or clang on Linux/macOS, or MSVC/clang-cl on Windows. See [CONTRIBUTING.md](CONTRIBUTING.md#setup) for details.

The host build also produces **`xcat-cli`**, which runs the core on a PC. It works on both trimmed xisos and full-disc `.iso` dumps:

```sh
build/host-debug/tools/xcat-cli/xcat-cli list <image>               # layout + every file, in disc order
build/host-debug/tools/xcat-cli/xcat-cli extract <image> <out-dir>  # same output as extract-xiso -x
build/host-debug/tools/xcat-cli/xcat-cli check <image> [title]      # can the Xbox HDD store it?
build/host-debug/tools/xcat-cli/xcat-cli layout <image>             # can it be installed in one pass?
build/host-debug/tools/xcat-cli/xcat-cli get <url> <file>           # download over HTTP(S) with Range and resume
build/host-debug/tools/xcat-cli/xcat-cli map <map-file>             # check a map file and list its entries
build/host-debug/tools/xcat-cli/xcat-cli install <url|image> <games-dir> [title]   # install like the console: resumable, verified
```

`get`, `map` and `install` speak `http://` and `https://` (TLS 1.2, certificates always checked against the built-in Mozilla roots, or the PEM file given with `--ca-file`), and report retries and MB/s on the way. `--credentials <file>` adds logins from a [credentials file](docs/formats/credentials.md), sent only to the hosts each one is scoped to. [tools/testserver](tools/testserver/README.md) has a local Range server, with optional faults, to try it against.

**Xbox app** (needs Docker):

```sh
git submodule update --init --recursive
make docker-xbe     # → app/bin/default.xbe, app/xiso-cartographer.iso
```

See [docs/build-xbox.md](docs/build-xbox.md) for building without `make`, the Windows line-ending fix, and running the XBE in xemu or on a console.

## Documentation

| Doc | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, interfaces, install flow, memory budget |
| [docs/formats/map.md](docs/formats/map.md) | The map file format (where your images are) |
| [docs/formats/archiveorg.md](docs/formats/archiveorg.md) | `@archiveorg`: turning an archive.org item's listing into map entries |
| [docs/formats/credentials.md](docs/formats/credentials.md) | Logins for sources that need them: host-scoped tokens, kept out of maps |
| [docs/nas-setup.md](docs/nas-setup.md), [docs/formats/smb.md](docs/formats/smb.md), [folder-image.md](docs/formats/folder-image.md), [sevenz.md](docs/formats/sevenz.md) | Games from a NAS: setting it up, the SMB rules, how extracted game folders and archives install, and 7z |
| [docs/formats/art.md](docs/formats/art.md), [xpr.md](docs/formats/xpr.md), [synopsis.md](docs/formats/synopsis.md) | A game's picture and details: where they're looked for, XBE title images, XBMC4Gamers synopses |
| [docs/formats/zip.md](docs/formats/zip.md), [stream-install.md](docs/formats/stream-install.md) | Installing from zips, and from Deflate streams that can only be read front to back |
| [docs/formats/http.md](docs/formats/http.md) | The HTTP client: Range requests, redirects, retries and resume |
| [docs/formats/fatx-names.md](docs/formats/fatx-names.md) | What the Xbox hard disk can store, measured on the console, and when an install is refused |
| [docs/formats/journal.md](docs/formats/journal.md) | The install journal that lets an interrupted install resume |
| [docs/HANDOFF.md](docs/HANDOFF.md) | Goals, design decisions, roadmap and standards |
| [docs/build-xbox.md](docs/build-xbox.md) | Building the XBE and running it in xemu or on hardware |
| [docs/testing.md](docs/testing.md) | Test ladder, hardware bench and results log |
| [docs/adr/](docs/adr/) | Architecture decision records |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Setup, MR rules and review checklist |
| [CLAUDE.md](CLAUDE.md) | Guidance for AI coding agents |

## Licence

[MIT](LICENSE). See [ADR 0002](docs/adr/0002-license.md) for the reasoning and dependency licences.
