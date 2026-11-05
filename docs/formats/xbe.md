# XBE certificate (title ID and name) and title image

**Status:** Normative. Implemented by `xcat/xbe.h` (M6; §4–§5 since 0.12.0). After an install, the installer reads the game's title ID from its `default.xbe` and records it ([ADR 0013](../adr/0013-archiveorg-source.md) decision 1, [journal.md §7.1](journal.md#71-the-marker)). This lets entries keyed by name ([archiveorg.md](archiveorg.md)) be tied to a title ID later. A game's details also show its region, disc and title image ([art.md §3](art.md#3-the-xbe), [ADR 0019](../adr/0019-game-art.md)).

An XBE is the Xbox's executable format. Only the few header fields needed to find the certificate and the sections are read, and the title image's section; nothing is executed or loaded. All integers are little-endian. The file comes from a downloaded image or a NAS, so it is untrusted.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Image header

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | magic `XBEH` |
| `0x004` | 256 | signature (not checked) |
| `0x104` | 4 | base address: where the headers are loaded in memory |
| `0x108` | 4 | size of all headers, in bytes |
| `0x118` | 4 | certificate address (in memory) |
| `0x11C` | 4 | number of sections (§5) |
| `0x120` | 4 | section headers address (in memory, §5) |

## 2. Certificate

The certificate sits at file offset `certificate address − base address`.

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | size of the certificate |
| `0x004` | 4 | time stamp |
| `0x008` | 4 | **title ID**, e.g. `0x4D530004`, the same number map keys use ([map.md §4.1](map.md#41-key)) |
| `0x00C` | 80 | **title name**, 40 UTF-16LE code units, ended by a 0 unit if shorter |
| `0x05C` | 64 | alternate title IDs (not read) |
| `0x09C` | 4 | allowed media (not read) |
| `0x0A0` | 4 | **game region** (§4) |
| `0x0A4` | 4 | game ratings (not read) |
| `0x0A8` | 4 | **disk number**: 0 for the first disc |
| `0x0AC` | 4 | **version** |

## 3. Checks

The reader refuses the file ("not an XBE") unless all of these hold:

1. The file is at least `0x178` bytes and starts with `XBEH`.
2. `size of headers` is at least `0x178` and no larger than the file.
3. `certificate address ≥ base address`, and the certificate's offset is at least `0x178`.
4. The certificate's first `0x5C` bytes lie inside the headers (`offset + 0x5C ≤ size of headers`), and its `size` field is at least `0x5C`.

The title name is converted to UTF-8. A unit in `0xD800`–`0xDFFF` (UTF-16 surrogates; a valid pair is two of them) and a control character (below `0x20`, or `0x7F`–`0x9F`) each become `?`. Trailing spaces are trimmed. An empty name is allowed.

## 4. Region, disc and version

`xc_xbe_read_info` reads, after §3's checks, the game region, disk number and version, if the certificate reaches them: its `size` field is at least `0xB0` and `offset + 0xB0 ≤ size of headers` (and within the first 64 KiB, §5). Otherwise they're left out (zero, `has_region` false) and the file is still an XBE.

The region's bits: `0x1` North America, `0x2` Japan, `0x4` the rest of the world, `0x80000000` manufacturing (debug) consoles. [art.md §6.1](art.md#61-regions) names them.

## 5. Sections and the title image

The headers, up to their size or **64 KiB**, whichever is less, are read into memory. The section headers are at file offset `section headers address − base address`: `number of sections` of them, 56 bytes each, which must all lie inside what was read, the address must be at or past the base, and there must be from 1 to **512** of them. Otherwise no section is looked at (and the file is still an XBE).

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | flags |
| `0x04` | 4 | virtual address |
| `0x08` | 4 | virtual size |
| `0x0C` | 4 | **raw address**: where its bytes are in the file |
| `0x10` | 4 | **raw size** |
| `0x14` | 4 | **section name address** (in memory) |
| `0x18`–`0x37` | | reference counts, shared-page addresses, SHA-1 (not read) |

A section's name is the NUL-terminated string at `name address − base address`; it counts only if it's wholly inside what was read, and the address is at or past the base. The **title image** is the first section named exactly `$$XTIMAGE`, else the first named `$$XSIMAGE` (the save image, often smaller), whose bytes lie inside the file and whose raw size is from `0x20` to **256 KiB**; a section that breaks these is passed over, not an error. Its bytes are an XPR0 texture ([xpr.md](xpr.md)). Neither: no title image.

## 6. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-10-20 | First version (M6). |
| 2 | 2026-11-05 | §4: region, disc and version; §5: the sections and the title image (ADR 0019). |
