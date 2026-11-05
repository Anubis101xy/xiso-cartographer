# 0019. A game's picture and details from its own files: covers, title images, synopses

- **Status:** Accepted (owner of this fork, 2026-11-05)
- **Date:** 2026-11-05

## Context

The owner browses a few hundred games on his NAS from the console. The list showed file names, in the order the NAS listed them, and the details only what the map and the size said. He asked for the games in alphabetical order, the full name of a long one, and "game art and info on the games on the NAS side", with the console's and the NAS's addresses and the drive's space always in view.

Art has three possible sources:

1. **Inside the game.** Every retail `default.xbe` carries a certificate (real title, title ID, region, disc number) and almost every one a title image, an XPR0 texture in a `$$XTIMAGE` section, which the dashboards show. Nothing to set up, but the image is 128 pixels at most, and an archive's XBE is compressed.
2. **Beside the game, on the NAS.** The scene's convention is XBMC4Gamers': `_resources/artwork/poster.jpg` and `_resources/default.xml` (developer, publisher, genre, year, rating, overview) in the game's folder, which artwork packs and the Xbox Artwork Installer produce; XBMC's `default.tbn`, and pictures named after an image, are older conventions. The owner chooses and supplies them.
3. **From the internet.** A database lookup by title ID. It needs a service, its terms and its availability, an account or key, HTTPS on every browse, and downloads on every visit; and [ADR 0006](0006-no-links-no-bypass.md) keeps this app to the sources the owner's map names, with no built-in source list and no traffic the owner didn't start.

The owner chose 1 and 2 together.

## Decision

1. **Covers beside the game first, then the title image, and the synopsis when there's one** ([art.md §2](../formats/art.md#2-where-art-is-looked-for)): `<name>.jpg/.png/.tbn` beside an image; for a game folder, or an image alone in its folder, XBMC4Gamers' poster, `default.tbn`, `cover.*` and `folder.*`, and `_resources/default.xml`. Nothing from the internet.
2. **The XBE is read without unpacking the game.** A game folder's `default.xbe` is opened as it is; a disc image's is found in its root table alone (`xc_xdvdfs_find_root`, [xdvdfs.md §10](../formats/xdvdfs.md#10-a-name-in-the-root)) and read through a window of the image, a handful of small reads. Archives get no XBE: decoding a 7z's solid block to reach it could take a minute. The certificate's region, disc and version and the section headers are read as [xbe.md §4–§5](../formats/xbe.md#4-region-disc-and-version); the texture is decoded by our own XPR0 reader ([xpr.md](../formats/xpr.md): DXT1/3/5, swizzled and linear 16- and 32-bit), a few hundred lines, tested against the encoder that writes this app's own title image.
3. **JPEG and PNG through stb_image, fenced.** Covers are JPEG (often progressive) or PNG. stb_image (public domain or MIT, one file) is vendored at v2.30 and built with only those two decoders, from memory, with no SIMD, `-fwrapv` (fuzzing found its IDCT's int overflow on a crafted JPEG; it now wraps, as other fuzzed users build it) and its asserts off in the XBE; the sanitizer builds don't report its `memcpy` of nothing from a null pointer (an empty PNG `IDAT`). Its open security reports are in decoders that aren't compiled (GIF, TGA, PIC, HDR, BMP) and in `stb_image_write`. On top: a 4 MiB file limit, the size read from the header and refused past 2048 a side or 1536² pixels before decoding, and an allocator that stops a picture at 32 MiB ([art.md §4](../formats/art.md#4-pictures)). `fuzz_picture`, `fuzz_xpr`, `fuzz_synopsis` and the extended `fuzz_xbe` cover the new input. The alternatives were libjpeg-turbo and libpng (far larger, and nxdk ships neither for the app) or decoders without progressive JPEG (picojpeg, NanoJPEG), which many packs' posters use.
4. **The synopsis is read by a small tolerant reader** ([synopsis.md](../formats/synopsis.md)), not a general XML parser: the root must be `<synopsis>`, the known elements' text is taken with entities, CDATA, invalid UTF-8 and whitespace handled, and everything else skipped. A full XML parser would bring more code to fuzz for a flat file of a dozen fields.
5. **Read when the highlight rests, not while it moves, and kept.** 350 ms on a game reads its art over one connection (the footer says so while a NAS is read); the last 12 games' art stays in memory (about 80 KiB each), and a NAS that didn't answer isn't asked again for a minute ([art.md §7](../formats/art.md#7-when-its-read)). The read blocks the screen: the SMB client is synchronous and not safe to share with a thread, and on a LAN it's a fraction of a second.
6. **The library around it.** GAMES and INSTALLED are sorted by name, case folded, numbers by value ("Halo 2" before "Halo 10"); a highlighted name too long for its line slides to its end and back; a panel at the top shows the console's address, the highlighted game's NAS (else the map's first) with "off" if its folders didn't list, and each drive's free space with a bar. The title tab narrows to the sections' width to make room. The details show the picture above the heading, the synopsis's or the XBE's title in place of the file name, and White and Black scroll a paragraph that doesn't fit.

## Consequences

- Every game on a NAS or in INSTALLED gets at least its real title, title ID and region (from its XBE) and, almost always, a picture, with nothing to set up; an owner with XBMC4Gamers packs gets the posters and synopses they already have. Archives get only what's beside them.
- The XBE grows by about 60 KiB (stb_image's two decoders, XPR0, the synopsis reader). Memory: at most 32 MiB while a picture decodes, freed after; about 1 MiB for the kept art.
- stb_image becomes a third-party component to watch: `third_party/stb/README.md` says how to update it, and its fuzzer runs on every change.
- The list's order is the name's, no longer the map's or the NAS's.
- A NAS whose disks are asleep makes the first read after a while take seconds, during which the screen holds still with a message.
