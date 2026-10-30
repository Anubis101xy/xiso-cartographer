# Third-party code

Every dependency is a pinned git submodule under `third_party/` (or vendored with its licence file intact). Licence compatibility is covered by [ADR 0002](docs/adr/0002-license.md). Upgrades go in a dedicated MR.

| Component | Version | Licence | Path | Used for | Ships in release |
|---|---|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) | v2.7.0 | MIT | `third_party/unity` | Host unit tests ([ADR 0003](docs/adr/0003-unit-test-framework.md)) | No |
| [BearSSL](https://www.bearssl.org/) | `7bea48e` (master, 2026-04-06; v0.6 plus fixes) | MIT | `third_party/bearssl` | TLS 1.2, X.509, hashes ([ADR 0005](docs/adr/0005-bearssl-for-tls.md), [ADR 0012](docs/adr/0012-tls-trust-time-and-entropy.md)). Fetched from bearssl.org, so Dependabot can't track it: check upstream and CVE feeds at each release | Yes (from M5) |
| [CA bundle](https://curl.se/docs/caextract.html) (`cacert.pem`, Mozilla's root store as curl publishes it) | Mozilla data as of 2026-09-25, SHA-256 `a41b5d35…0505` (pinned in `cacert.pem.sha256`) | MPL-2.0 | `third_party/cacert` (vendored, unmodified) | Built-in TLS trust anchors ([ADR 0012](docs/adr/0012-tls-trust-time-and-entropy.md) §3); updated in a dedicated PR, see its README | Yes (from M5) |
| [miniz](https://github.com/richgel999/miniz) | 3.1.2 (`77d0dce`, 2026-07-01) | MIT | `third_party/miniz` | Its inflater (tinfl) only, for Deflate zip members ([ADR 0009](docs/adr/0009-install-from-zip-archives.md), [zip.md §9](docs/formats/zip.md)); built by `cmake/miniz.cmake` with its own `miniz_export.h` | Yes (from M5b) |
| [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic) (Regular, Bold) | v2.015, hinted TTF (notofonts.github.io `28b15b4`, 2024-11-21), pinned in `SHA256SUMS` | SIL OFL 1.1 | `third_party/fonts` (vendored, unmodified, with `OFL.txt`) | The UI font ([ADR 0015](docs/adr/0015-ui-sdl2-own-widgets.md) decision 7); copied next to each XBE with a UI, with `OFL.txt` ([README](third_party/fonts/README.md)) | Yes (from M8) |
| [Michroma](https://github.com/googlefonts/Michroma-font) (Regular) | 1.100 (google/fonts `45e3d52`, 2026-10-21), pinned in `third_party/fonts/SHA256SUMS` | SIL OFL 1.1 | `third_party/fonts` (vendored, unmodified, with `OFL-Michroma.txt`) | Screen titles ([ui-style.md](docs/ui-style.md#fonts)); shipped like Noto Sans | Yes (from M8) |
| [Chakra Petch](https://github.com/m4rc1e/Chakra-Petch) (Bold) | 1.000 (google/fonts `45e3d52`), pinned the same way | SIL OFL 1.1 | `third_party/fonts` (vendored, unmodified, with `OFL-ChakraPetch.txt`) | Section names and labels ([ui-style.md](docs/ui-style.md#fonts)); shipped like Noto Sans | Yes (from M8) |
| [libsmb2](https://github.com/sahlberg/libsmb2) | `727feaa` (master, 2026-10-25) | LGPL-2.1-or-later | `third_party/libsmb2` (vendored: `lib/`, `include/` and its CMake checks, with the nxdk port in `xcat/nxdk.patch`; [README](third_party/libsmb2/xcat/README.md)) | SMB2/3 client for NAS shared folders ([ADR 0018](docs/adr/0018-smb-sources.md)); statically linked, so the release says where its source is and the XBE can be rebuilt with a changed copy | Yes (from 0.10.0) |
| [nxdk](https://github.com/XboxDev/nxdk) | `14d5ee9` (2026-09-30) | Mixed; see below | `third_party/nxdk` | XBE toolchain and runtime libraries | Linked parts only |

## nxdk components

nxdk brings its own submodules. Licences per nxdk's README and `LICENSES/` folder at the pinned commit:

| Component | Licence | Linked into our XBE |
|---|---|---|
| nxdk core (OpenXDK-derived), pbkit, hal, winapi, xboxrt | MIT | Yes |
| compiler-rt-derived runtime | MIT | Yes |
| PDCLib (C library) | CC0 | Yes |
| lwIP (network stack) | Modified BSD | Yes (from M3) |
| SDL2 | zlib | Yes: the controller (from M4) and the UI (from M8, [ADR 0015](docs/adr/0015-ui-sdl2-own-widgets.md)) |
| SDL_ttf | zlib | Yes, from M8 (ADR 0015) |
| FreeType 2.4.12 (inside SDL_ttf) | FreeType License (FTL), credit required in the documentation: given in the release's `CREDITS.txt` ([tools/catalog/build.py](tools/catalog/build.py)) | Yes, from M8; loads only the bundled font (ADR 0015) |
| libusbohci (USB host, Nuvoton; with nxdk's `libusbohci_xbox` glue) | Apache-2.0, with Nuvoton's disclaimer | Yes: every nxdk XBE links it for the controllers (found in the release audit, ADR 0017) |
| SDL2_image | zlib | Only if used |
| libpng, libjpeg-turbo, zlib | libpng / IJG + BSD / zlib | Only if used |
| libc++ | Apache-2.0 WITH LLVM-exception / NCSA | Only for C++ builds |
| cxbe | GPL-2.0-or-later | No: build-time tool ([ADR 0002](docs/adr/0002-license.md#recheck-nxdk-added-in-mr-4)) |
| NVIDIA Cg compiler (`cgc`) | Proprietary | No: build-time tool, shaders only |
| extract-xiso | BSD | No: build-time tool (creates the test ISO) |

Release zips include the licence notices for every component marked "Yes" ([ADR 0017](docs/adr/0017-release-packaging.md)): `tools/release/package.py` copies each one into the zip's `licenses\` folder and lists them in `THIRD_PARTY.txt`, and refuses to pack if one is missing. A new linked component needs a row here and an entry in its list.
