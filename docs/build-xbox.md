# Building and running the XBE

The Xbox app is built with [nxdk](https://github.com/XboxDev/nxdk), pinned as the submodule `third_party/nxdk`. The supported way to build it is inside a Docker image made from that submodule, so every machine and CI use the same toolchain.

## Prerequisites

- Docker (on Windows: Docker Desktop with WSL 2).
- The nxdk submodule and its own submodules:

  ```sh
  git submodule update --init --recursive
  ```

### Windows: line endings

If git converts line endings on checkout (`core.autocrlf=true`, the Git for Windows default), nxdk's shell scripts get CRLF endings and the Linux build fails with `env: can't execute 'sh'`. Our own files are protected by `.gitattributes`, but nxdk and its submodules aren't. Run this once after cloning, and again after any submodule update that adds new submodules:

```sh
git submodule foreach --recursive "git config core.autocrlf false && git config core.eol lf && git rm -q --cached -r . && git reset -q --hard"
```

This rewrites the submodules' working files only. It's safe as long as you haven't edited anything inside `third_party/`.

## Build

With `make` (Linux, macOS, WSL):

```sh
make docker-xbe
```

Without `make` (e.g. Git Bash or PowerShell on Windows):

```sh
docker build -f docker/nxdk.Dockerfile -t xcat-nxdk --build-context xcat-lwip=platform/nxdk/lwip third_party/nxdk
docker run --rm -v "$PWD:/usr/src/app" xcat-nxdk make -C app
```

On Git Bash, prefix the `docker run` command with `MSYS_NO_PATHCONV=1`, so the container path isn't rewritten.

The first image build compiles nxdk's tools and libraries. Docker caches it after that, until the nxdk submodule, our lwIP options or the Dockerfile change. nxdk compiles without optimisation; the image builds SDL, SDL_ttf and FreeType a second time at `-O2`, since the UI draws every pixel of every frame through them ([ui-style.md](ui-style.md#cost)).

**Network (lwIP) options:** nxdk's network library is built into the image with our [`platform/nxdk/lwip/lwipopts.h`](../platform/nxdk/lwip/lwipopts.h) ahead of nxdk's own: a larger TCP receive window with window scaling, more receive buffers in the network driver, and SACK. Without them one connection to archive.org ran at 0.3 MB/s (testing.md, M5). `app/lwip.mk` puts the same folder first for our own sources, so both sides agree. The file reaches the image build as the named context `xcat-lwip` (the `--build-context` above); after changing it, rebuild the image (`make docker-xbe` does).

**Output:**

| File | Use |
|---|---|
| `app/bin/default.xbe` | The program. Copy all of `app/bin/` to the console: the XBE needs `fonts\` and `ui\` next to it. It carries the app's logo as its title image, which dashboards show ([xbe-title-image.md](formats/xbe-title-image.md)). |
| `app/bin/samples/small.iso` | Bundled synthetic test image (the `small` fixture: dummy data, never a game). The app installs it when `E:\xcat\isos\` is empty. |
| `app/xiso-cartographer.iso` | Disc image containing `app/bin/` (the XBE, `fonts\`, `ui\` and `samples\`). Boots directly in xemu. |
| `app/xiso-cartographer-walkthrough.iso` | The same disc with [`tools/ui/walkthrough.txt`](../tools/ui/walkthrough.txt) next to the XBE as `ui-script.txt`: the app plays it, the M8 exit test's scripted walkthrough ([ui-script.md](formats/ui-script.md), [testing.md](testing.md#m8-scripted-walkthrough)). For testing only; never shipped. |
| `app/fatx-probe/bin/default.xbe`, `app/fatx-probe/xcat-fatx-probe.iso` | The FATX probe: a test tool that measures what file names and paths the console's FATX driver accepts. See [testing.md](testing.md#fatx-probe-m2). |
| `app/net-test/bin/default.xbe`, `app/net-test/xcat-net-test.iso` | The network test: downloads a file over HTTP or HTTPS and reports MB/s, retries and the CRC-32; for HTTPS also the CPU's cipher speed and the negotiated TLS suite. See [testing.md](testing.md#m3-network-download-test) and [M5](testing.md#m5-tls-speed-on-the-console). |
| `app/ui-test/bin/` (`default.xbe`, `fonts\` and `ui\`), `app/ui-test/xcat-ui-test.iso` | The UI test (M8): one screen in the UI's look ([ui-style.md](ui-style.md)), with the panning background, the bundled fonts, the text helpers and the widgets (list, progress bar, a dialog on A, the on-screen keyboard on Y and, as a password field, on X), drawn through SDL; shows the time to draw a frame and frames per second. Copy the whole `bin\` folder: the XBE needs `fonts\` and `ui\` next to it. See [testing.md](testing.md#m8-ui-test-rendering). |

XBEs with a UI include [`app/ui/ui.mk`](../app/ui/ui.mk) and [`app/ui/ui-assets.mk`](../app/ui/ui-assets.mk) ([ADR 0015](adr/0015-ui-sdl2-own-widgets.md)): the UI sources, nxdk's SDL2 and SDL_ttf, Noto Sans from [`third_party/fonts/`](../third_party/fonts/README.md), checked against its `SHA256SUMS` and copied to `bin/fonts/` (the build stops if a font file doesn't match), and the images from [`app/ui/assets/`](../app/ui/assets/README.md), converted from PNG to BMP in `bin/ui/` by `tools/ui/png2bmp.py` (needs `python3`, which the image has).

The app's XBE gets its title image after `cxbe` makes it: `tools/ui/xbe_title_image.py` turns [`app/assets/title-image.png`](../app/assets/README.md) into a 128 × 128 DXT1 texture and adds it as the section `$$XTIMAGE` ([xbe-title-image.md](formats/xbe-title-image.md)), since `cxbe` can't. It needs `python3`, which the image has. A dashboard may keep the image it saw before (UnleashX and XBMC cache them), so a new one can take a cache reset to show.

XBEs with HTTPS include [`app/tls.mk`](../app/tls.mk): BearSSL, `core/src/tls.c`, `core/src/sntp.c`, `core/src/hash.c`, the CA bundle (embedded at build time, so the build needs `python3`, which the image has) and `platform/nxdk/src/nx_tls.c`. nxdk compiles without optimisation by default; `tls.mk` builds BearSSL and `tls.c` with `-O2`, since unoptimised crypto would understate the TLS speed several times over. BearSSL's object files land in its submodule folder, which git is told to ignore (`ignore = untracked` in `.gitmodules`).

The app also includes [`app/smb.mk`](../app/smb.mk): libsmb2 from [`third_party/libsmb2/`](../third_party/libsmb2/xcat/README.md) (LGPL-2.1, vendored with a small nxdk port), built at `-O2` with its warnings off like BearSSL, and the SMB client and NAS scan from `platform/smb/` ([ADR 0018](adr/0018-smb-sources.md)). The NAS scan opens 16 connections at a time, so `lwipopts.h` also raises lwIP's socket table (`MEMP_NUM_NETCONN` 32) and ARP table (`ARP_TABLE_SIZE` 64); rebuild the image after pulling this change.

Every CI run also uploads all of these files as the `xiso-cartographer-xbe-<sha>` artifact (`xbe` job).

## Run in xemu

1. Set up xemu with your own BIOS, MCPX boot ROM and HDD image. These are never part of this repo; see the [xemu docs](https://xemu.app/docs/required-files/).
2. **Machine → Load Disc…** and choose `app/xiso-cartographer.iso`. Then reset the machine (**Machine → Reset**) if it doesn't boot on its own.

## What the app does

The map is a **catalog to pick from**: nothing installs until you choose it. At start the STARTING screen shows the app loading the credentials, the catalog and the map (or, if it can't go on, why, and how to return to the dashboard), then the library takes over (M8, [ui-style.md](ui-style.md#library)):

- **The library** lists everything it can install, with the highlighted game's details beside the list: its year, region and size, who made it, the release's Redump name and title ID, and where it comes from. **D-pad up/down** moves, **LT/RT** page, **Left** goes to the sections (GAMES and INSTALLED work so far), **A** asks "Install …?" and installs on INSTALL, **SETTINGS** chooses where games install (`E:\Games` by default, `F:\Games` or `G:\Games` on drives with those partitions, or any folder typed on the keyboard; kept in `E:\Apps\xcat\settings`, [settings.md](formats/settings.md)), under **Authentication** lists the servers your map uses with their logins (a login from the credentials file can be removed there), and under **Map sources** lists the lines of `E:\xcat\map.txt` to add an archive.org item, another map or one image, or remove one, and logs in to archive.org with your email and password on the on-screen keyboard (only the session is saved, in `E:\Apps\xcat\credentials`, never the password) and logs out, **Y** searches the games by name, year, region, maker or origin with the on-screen keyboard (**B** lists them all again), and **Back** asks before returning to the dashboard. Games already in the install folder say INSTALLED, or UNFINISHED if an earlier install stopped part way (installing again carries on), and the **INSTALLED** section lists the folders this app installed, with their size, files, title ID, checks and origin.
- **The install screen** shows what the install is doing (connecting, checking the image, writing, verifying), a progress bar, how much is done, the file, the speed and the time left, and each step and retry of the network. **B** asks before stopping; a stopped install keeps what's written and carries on from there when it's started again. At the end a dialog says INSTALLED, with the folder, or why not (the error and its details, like the space needed and free), with RETRY and BACK. If archive.org refused the login (expired, revoked, or not logged in), it offers **LOG IN** instead of RETRY: log in on the keyboard and the install carries on at once. Back in the library, the game is marked INSTALLED, FAILED or STOPPED, with that message.
- Holding **LT + RT + Back + Black** returns to the dashboard at any time.

The list holds:

1. **Every entry of the map `E:\xcat\map.txt`** ([map.md](formats/map.md)), including the games an `@archiveorg <item>` line lists (by name and size, [archiveorg.md](formats/archiveorg.md)). They download over HTTP and install while they stream in. Nothing is stored except the game's files ([ADR 0004](adr/0004-streaming-install.md)), so images of 4 GiB and more, such as full-disc dumps, install too. Entries that need a login use `E:\Apps\xcat\credentials` ([credentials.md](formats/credentials.md)). Entries can be `http://` or `https://` (see [HTTPS](#https) below). An entry whose URL ends in `.zip` installs the zip's one image (see [Zips](#zips) below).
   `@smb smb://<NAS>/<share>/<folder>` lines list the images (`.iso`, `.xiso`, `.zip`) and extracted game folders (a folder with a `default.xbe`) in a NAS's shared folder, which install over SMB the same way, resumably ([smb.md](formats/smb.md); see [NAS](#nas) below).
2. **Every image or zip in `E:\xcat\isos\`** (`*.iso`, `*.zip`), as in M2. These must be under 4 GiB, since FATX can't store a larger file.
3. If there's neither, the bundled test image from `D:\samples\` (`D:` is the folder or disc the XBE was started from), copied to `E:\xcat\isos\` first.

Each install, network or local, follows the same steps ([ARCHITECTURE.md §5](ARCHITECTURE.md#5-install-flow)):
- It checks that FATX can store every file under `E:\Games\<title>`, or the install folder chosen in SETTINGS ([ADR 0008](adr/0008-refuse-games-fatx-cant-store.md)) and that there's enough free space.
- It writes the files with progress and a **journal**, so a pulled cable is retried and a **power cut resumes** on the next start instead of starting over ([journal.md](formats/journal.md)).
- For a trimmed xiso it reads the image front to back in full and computes its **MD5 and SHA-1**, which it logs (from M6 they're compared with archive.org's; [journal.md §7](formats/journal.md#7-verification)). Full-disc layouts are checked by size and CRC-32 only.
- It **reads every installed file back and compares its CRC-32** with what was received, then writes `E:\Games\<title>\.xcat-installed`, with the hashes when there are some.

### The catalog (M7)

If `E:\Apps\xcat\catalog.bin` exists ([catalog.md](formats/catalog.md), built by [`tools/catalog/`](../tools/catalog/README.md#shipping-it)), the log says "Catalog: N games, M releases (<snapshot>)" and the list shows each game by its catalog title. An archive.org game is found by its name (its Redump name), an entry keyed by title ID by its title ID and variant, and a file in `E:\xcat\isos\` by its file name. The details show the year, region, developer, genres, and the release's Redump name and map key (`4D530004@US`). A game the catalog doesn't know is shown by its file name and its details say "Not in the catalog"; it still installs. After an install, the title ID read from `default.xbe` is compared with the catalog's: "matches the catalog: 4D530004@US, …", or "the catalog expected … : please report it", which means the catalog needs a fix (`[title_id]` in `overrides.toml`). Without the file, or with a damaged one (the log says why), the list works as before.

A title that's already installed gets a numbered folder (`small (2)`). An unfinished install of the same image is resumed in its own folder (pick the same game again after a power cut); one of a different image is left alone. The log `E:\xcat\install.log` has every detail of each install, starting with "Picked: <game>" and ending with "Installed and verified." or "Not installed.": the speeds, the hashes, the title ID, TLS, each retry.

### Zips

A zip must hold exactly one `.iso` or `.xiso` image, stored or Deflate-compressed ([zip.md](formats/zip.md), [ADR 0009](adr/0009-install-from-zip-archives.md)). A stored image installs like a bare one. A Deflate image is decompressed as it streams in ([stream-install.md](formats/stream-install.md)):
- Files whose directory table has already passed are written as they arrive. Everything else that might belong to a file goes to **spool files** (`.xcat-spool-<n>`) in the install folder. They use the free space on E: minus 256 MiB, and when that runs out the oldest are dropped.
- Once every table is known, the files that came before their tables are copied out of the spool. If the spool had to drop some, the zip is read again from a checkpoint for just those bytes (a second pass), and the log says how much was read again.
- The member's CRC-32 checks every decompressed byte. Later passes are checked against the first at every checkpoint.
- Every 64 MiB the install saves its state, so after a power cut it **continues from the last checkpoint**: the log then says "continued N MiB into the image". It also keeps `.xcat-state-0`, `.xcat-state-1` and `.xcat-zip-checkpoints` in the install folder until it's finished.

The log ends a zip install with "from the zip: <image>, Deflate", the number of passes, the MiB read again and the spool's largest size. Map entries that point *into* a zip (`…/game.zip/game.iso`, archive.org's on-the-fly extraction) are refused when the map is loaded, since such a download can't be resumed: use the zip's own URL.

### HTTPS

The first `https://` URL (a map entry, an `@include`, or a redirect from `http://`) sets up TLS once ([ADR 0012](adr/0012-tls-trust-time-and-entropy.md)), and the log shows each step:
- **Randomness** from timer jitter and `E:\Apps\xcat\random-seed`, which it renews.
- **The time** for certificate checks: from NTP, `pool.ntp.org` by default ([sntp.md](formats/sntp.md)). To change it, put a host name or IPv4 address on the first line of `E:\Apps\xcat\ntp.txt`, or `off` to skip NTP (until the settings screen, M8). If NTP fails, the console's clock is used, but only if it isn't earlier than the XBE's build. Otherwise HTTPS stops with "Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)"; `http://` entries still install.
- **Trust:** the built-in Mozilla roots, or only the certificates in `E:\Apps\xcat\cacert.pem` if that file exists (for a LAN server with its own certificate).

After each HTTPS install the log shows the TLS version and suite the server chose.

**Hold LT + RT + Back + Black** (the shortcut rocky5's tools use) at any time to return to the dashboard, including mid-install or while it waits out a network outage. An install stopped this way keeps everything written so far and resumes on the next start. Confirmed on hardware on 2026-10-15 ([testing.md](testing.md#results-log)).

### NAS

Games on a NAS (or a PC's shared folder) install over SMB2/3, with no PC or HTTP server in between ([ADR 0018](adr/0018-smb-sources.md), [smb.md](formats/smb.md), the setup guide [nas-setup.md](nas-setup.md)):

- **SETTINGS > NAS** shows the console's own address (`This Xbox: …`), lists the NASes with a saved login first, finds others on the console's network (TCP 445 on each address of its /24, then each one's NetBIOS name) or takes a typed address, and logs in with a user name and password on the on-screen keyboard (none: a guest). The login is saved to the credentials file, as an NT hash ([credentials.md §4.1](formats/credentials.md#41-smb-logins)), as soon as the NAS accepts it, so A on that NAS later goes straight in (X: another login). It browses shares and folders, and **Add this folder to GAMES** (or X on a highlighted folder) writes an `@smb` line to the map, lists the games and goes to GAMES.
- Images install like HTTP ones: streamed, resumable, checked. An extracted game folder is listed level by level and installed as if it were an image ([folder-image.md](formats/folder-image.md)): the same FATX checks, journal and read-back.
- **PLAY:** A on an installed game starts its `default.xbe`.
- The log names the NAS's own reason for a failure (`E:\xcat\install.log`), never the login.

## Run on hardware

1. Start the console's FTP server from your dashboard. On XBMC4Gamers and UnleashX it's normally enabled; the default login is `xbox` / `xbox`.
2. Connect with an FTP client (e.g. FileZilla) to the console's IP on port 21.
3. Create `E:\Apps\xiso-cartographer\` and upload the contents of `app/bin/` into it: `default.xbe` and the `fonts`, `ui` and `samples` folders. Use **binary** transfer mode.
4. Optionally, upload your own images (xiso, under 4 GiB) to `E:\xcat\isos\`.
5. Launch it from the dashboard's Applications list, or browse to the folder in the file manager.

Record each run in [testing.md](testing.md).
