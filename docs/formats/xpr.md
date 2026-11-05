# XPR0 textures (an XBE's title image)

**Status:** Normative. Implemented by `xc_picture_xpr` (`xcat/picture.h`, `core/src/xpr.c`), tested by `core/tests/test_picture.c` and fuzzed by `fuzz_xpr.c`. [ADR 0019](../adr/0019-game-art.md).

Dashboards show an XBE's title image, an XPR0 resource in its `$$XTIMAGE` section (and a smaller save image in `$$XSIMAGE`); [xbe.md §5](xbe.md#5-sections-and-the-title-image) says how the section is found. A game's details show it when no cover is found beside the game ([art.md §2](art.md#2-where-art-is-looked-for)). [xbe-title-image.md](xbe-title-image.md) describes what this project writes for its own XBE; this describes what it reads from anyone's, which is wider. There's no public specification: the layout is the one the dashboards read, as XDK tools write it and emulators (xemu, Cxbx-Reloaded) decode it. All integers are little-endian. The section comes from a game image or a NAS and is untrusted.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. What's decoded

The first level (the largest mipmap) of the first resource, if it's a 2D texture in a format of §3, into 32-bit ARGB pixels. Nothing else in the file is read.

## 2. The header

| Offset | Size | Field | Must be |
|---|---|---|---|
| `0x00` | 4 | magic | `XPR0` |
| `0x04` | 4 | total size | not read |
| `0x08` | 4 | header size: where the data starts | at least `0x20`, at most the section's size |
| `0x0C` | 4 | `Common` | bits 16–18 (the resource type) 4: a texture |
| `0x10` | 4 | `Data`: the texture's offset from the data's start | inside the section |
| `0x14` | 4 | `Lock` | not read |
| `0x18` | 4 | `Format` | below |
| `0x1C` | 4 | `Size` | for linear formats (§2.2) |

Anything else is `XC_ERR_CORRUPT`.

### 2.1 Format

Bits 4–7: the dimensionality, which must be 2. Bits 8–15: the colour format (§3). Bits 20–23 and 24–27: the width and height as powers of two (`1 << n`), for swizzled and DXT formats; more than 11 is `XC_ERR_LIMIT`. Other bits (the DMA context, border, mipmap count) aren't read.

### 2.2 Size (linear formats)

Bits 0–11: the width less one. Bits 12–23: the height less one. Bits 24–31: the pitch (bytes from one row to the next) in units of 64, less one. A pitch smaller than a row is `XC_ERR_CORRUPT`.

### 2.3 Limits

A width or height over 1024 is `XC_ERR_LIMIT`. The texture's bytes, as §3 counts them, must lie inside the section (`XC_ERR_CORRUPT` otherwise).

## 3. Colour formats

| Byte | Format | Layout | Bytes |
|---|---|---|---|
| `0x02` | A1R5G5B5 | swizzled | 2 a pixel |
| `0x03` | X1R5G5B5 | swizzled | 2 |
| `0x04` | A4R4G4B4 | swizzled | 2 |
| `0x05` | R5G6B5 | swizzled | 2 |
| `0x06` | A8R8G8B8 | swizzled | 4 |
| `0x07` | X8R8G8B8 | swizzled | 4 |
| `0x0C` | DXT1 | 4 × 4 blocks | 8 a block |
| `0x0E` | DXT2, DXT3 | 4 × 4 blocks | 16 |
| `0x0F` | DXT4, DXT5 | 4 × 4 blocks | 16 |
| `0x10` | LIN_A1R5G5B5 | linear | 2 |
| `0x11` | LIN_R5G6B5 | linear | 2 |
| `0x12` | LIN_A8R8G8B8 | linear | 4 |
| `0x1C` | LIN_X1R5G5B5 | linear | 2 |
| `0x1D` | LIN_A4R4G4B4 | linear | 2 |
| `0x1E` | LIN_X8R8G8B8 | linear | 4 |

Any other (palettes, luminance, the A8B8G8R8 orders, cube maps) is `XC_ERR_UNSUPPORTED`.

### 3.1 Pixels

16-bit pixels are read as a little-endian word, the fields from the top bit down as named; 5- and 6-bit fields are widened by repeating their top bits (`v << 3 | v >> 2`, `v << 2 | v >> 4`), 4-bit ones by repeating them (`v << 4 | v`), and a 1-bit alpha is 0 or 255. 32-bit pixels are the bytes B, G, R, A. The X formats' alpha is 255.

### 3.2 Swizzling

Swizzled textures (power-of-two sizes) store pixel (x, y) at the index whose bits interleave x's and y's, lowest first, x's bit before y's, while both have bits left (`1 << i` below the width, and below the height); the larger side's remaining bits follow. For 4 × 2: (1, 0) → 1, (0, 1) → 2, (1, 1) → 3, (2, 0) → 4.

### 3.3 DXT blocks

Blocks run left to right, then top to bottom, and aren't swizzled. A colour block is two RGB565 end colours `c0` and `c1` (widened as §3.1) and sixteen 2-bit indices, pixel 0 in the low bits, row by row. Four-colour mode (always for DXT3 and DXT5; for DXT1 when `c0 > c1` as numbers): the colours are `c0`, `c1`, `(2·c0 + c1) / 3` and `(c0 + 2·c1) / 3`, per channel, rounded down. DXT1's three-colour mode (`c0 ≤ c1`): `c0`, `c1`, `(c0 + c1) / 2`, and transparent black.

DXT3: sixteen 4-bit alphas (pixel 0 in the low bits of the first byte) before the colour block, widened as §3.1. DXT5: two alphas `a0`, `a1`, then sixteen 3-bit indices (48 bits, pixel 0 lowest), then the colour block. With `a0 > a1` the eight alphas are `a0`, `a1` and `((7 − i)·a0 + i·a1) / 7` for i = 1…6; otherwise `a0`, `a1`, `((5 − i)·a0 + i·a1) / 5` for i = 1…4, then 0 and 255. DXT2 and DXT4 (premultiplied) are decoded as DXT3 and DXT5.

Pixels of a block past the texture's edge are skipped.

## 4. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-11-05 | First version (ADR 0019). |
