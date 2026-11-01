# Zip archives (reading the directory and choosing the image)

**Status:** Normative. §1–§10 are implemented by `xcat/zip.h` (M5b). §1–§7 are used by `xcat-cli zip` and `xc_install` (`xcat-cli install <zip>`), which installs a stored member like a bare image and a Deflate member through §9–§10 ([stream-install.md](stream-install.md)). There, pass 1 reads the whole member so its CRC-32 covers every byte; later passes restart at a checkpoint and check their CRC-32 at each checkpoint against pass 1's. The checkpoints are also kept on disk, so an install can continue after a power cut ([stream-install.md §7](stream-install.md#7-resuming-after-a-power-cut)). The reasons are in [ADR 0009](../adr/0009-install-from-zip-archives.md).

The installer reads a zip with HTTP `Range` like any other source ([ADR 0004](../adr/0004-streaming-install.md)): first the end of the file, then the central directory, then the chosen member's local header. This document fixes the subset of [PKWARE's APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT) that it accepts, and every check. Anything outside it is refused with a clear reason; nothing is guessed.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119. All integers are little-endian; offsets are from the start of the zip file.

## 1. Overview

```
[local header 1][data 1] ... [local header n][data n]
[central directory: one header per entry]
[Zip64 end record][Zip64 locator]   (only in Zip64 archives)
[end of central directory record][comment]
```

The **central directory** is authoritative: names, methods, sizes and CRC-32s come from it. A local header is only read for the chosen member, to find where its data starts (§6).

## 2. End of central directory record (EOCD)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x06054b50` (`PK\5\6`) |
| 4 | 2 | number of this disk |
| 6 | 2 | disk where the central directory starts |
| 8 | 2 | entries on this disk |
| 10 | 2 | entries in total |
| 12 | 4 | central directory size |
| 16 | 4 | central directory offset |
| 20 | 2 | comment length `k` |
| 22 | `k` | comment |

1. The reader reads the last `min(size, 20 + 22 + 65,535)` bytes of the file in one request: the longest EOCD, plus room for a Zip64 locator before it (§3). It searches backwards from `size − 22` for the signature. The first candidate whose comment length ends exactly at the end of the file (`pos + 22 + k == size`) is the EOCD.
2. No candidate, or a file shorter than 22 bytes: **not a zip** (`XC_ERR_NOT_ZIP`).
3. Both disk numbers must be 0 and the two entry counts equal. Otherwise the zip is split or spanned: **unsupported** (`XC_ERR_ZIP_UNSUPPORTED`).

## 3. Zip64

| Zip64 end locator (20 bytes, right before the EOCD) | | |
|---|---|---|
| 0 | 4 | signature `0x07064b50` |
| 4 | 4 | disk with the Zip64 end record |
| 8 | 8 | Zip64 end record offset |
| 16 | 4 | total number of disks |

| Zip64 end record | | |
|---|---|---|
| 0 | 4 | signature `0x06064b50` |
| 4 | 8 | size of the rest of the record (at least 44) |
| 12 | 2 | version made by |
| 14 | 2 | version needed |
| 16 | 4 | number of this disk |
| 20 | 4 | disk where the central directory starts |
| 24 | 8 | entries on this disk |
| 32 | 8 | entries in total |
| 40 | 8 | central directory size |
| 48 | 8 | central directory offset |

1. If the 20 bytes before the EOCD start with the locator signature, the archive is **Zip64**:
   - the locator's disk must be 0 and its disk count 1 (otherwise unsupported);
   - the Zip64 end record must lie wholly before the locator, start with its signature, and have a rest size of at least 44;
   - its disk numbers must be 0 and its entry counts equal (otherwise unsupported);
   - its entry count, size and offset **replace** the EOCD's.
2. Without a locator, an EOCD field holding its all-ones value (`0xFFFF` or `0xFFFFFFFF`) means a Zip64 record is missing: **corrupt** (`XC_ERR_CORRUPT`).
3. The **end of the directory area** is where the Zip64 end record starts (Zip64) or where the EOCD starts (otherwise).

## 4. Central directory

1. The central directory must lie inside `[0, end of the directory area)`: `offset + size ≤ end`. Data before the first local header (a self-extractor stub) shifts every offset and isn't supported: offsets that don't fit are corrupt.
2. Its size must be at most **4 MiB**, and the entry count at most **16,384** (`XC_ERR_LIMIT`). The reader reads it in one request.
3. The reader parses exactly `entries` headers, one after another from the start. Bytes after the last one (e.g. a digital signature record) are ignored. Each header:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x02014b50` |
| 4 | 2 | version made by |
| 6 | 2 | version needed |
| 8 | 2 | flags |
| 10 | 2 | method |
| 12 | 4 | DOS time and date |
| 16 | 4 | CRC-32 |
| 20 | 4 | compressed size |
| 24 | 4 | uncompressed size |
| 28 | 2 | name length `n` |
| 30 | 2 | extra length `m` |
| 32 | 2 | comment length `k` |
| 34 | 2 | disk number start |
| 36 | 2 | internal attributes |
| 38 | 4 | external attributes |
| 42 | 4 | local header offset |
| 46 | `n` + `m` + `k` | name, extra fields, comment |

4. A header whose signature is wrong, or which doesn't fit in the directory, is corrupt.
5. **Names:** 1 to 65,535 bytes and no NUL byte, otherwise corrupt. They're kept as bytes; flag bit 11 (UTF-8) only affects display. A name ending in `/` is a directory.
6. **Extra fields** are a sequence of `id` (2 bytes), `size` (2 bytes) and `size` bytes of data, which must exactly fill the `m` bytes (otherwise corrupt). Only the Zip64 field is read; others are skipped.
7. **Zip64 extended information** (`id 0x0001`): it holds, in this order, only the values whose header field is all ones: uncompressed size (8), compressed size (8), local header offset (8), disk number start (4). Its data must be at least as long as those values (extra bytes after them are ignored). A header field that is all ones without a Zip64 field to replace it is corrupt.
8. The disk number start must be 0 (otherwise unsupported).
9. Each entry's local header offset must be below the central directory offset (otherwise corrupt).

## 5. Choosing the image

1. The image is the one entry that isn't a directory and whose name ends in `.iso` or `.xiso`, compared without case for ASCII letters. A `.xiso.iso` name ends in `.iso`.
2. More than one: the install is refused (`XC_ERR_ZIP_MEMBER`), and every entry is listed so the user can see why. A map syntax for choosing one of several may come later (ADR 0009 §3).
3. None: the zip's game or app folder is installed, the folder of its shallowest `default.xbe`, its members read as the install asks ([folder-image.md §7](folder-image.md#7-archives), ADR 0018 decision 9). Without a `default.xbe` either, the install is refused (`XC_ERR_ZIP_MEMBER`).

## 6. The chosen member

1. **Supported:** method **0** (stored) or **8** (Deflate) ([ADR 0009 §2](../adr/0009-install-from-zip-archives.md)). Any other method is unsupported, and the message names it: 9 is Deflate64 (Windows' built-in zipper), 14 is LZMA (7-Zip), 99 is AES encryption.
2. Flag bits 0 (encrypted), 6 (strong encryption) and 13 (encrypted central directory) make the member unsupported. Bit 3 (sizes in a data descriptor after the data) is fine: the central directory has the sizes. Bits 1–2 (Deflate level) are ignored.
3. A stored member's compressed and uncompressed sizes must be equal (otherwise corrupt).
4. **Local header** (30 bytes, then name and extra) at the entry's local header offset:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `0x04034b50` |
| 4 | 2 | version needed |
| 6 | 2 | flags |
| 8 | 2 | method |
| 10 | 4 | DOS time and date |
| 14 | 4 | CRC-32 |
| 18 | 4 | compressed size |
| 22 | 4 | uncompressed size |
| 26 | 2 | name length `n` |
| 28 | 2 | extra length `m` |

   - The signature, the method and the name bytes must match the central directory entry (otherwise corrupt).
   - Its CRC-32, sizes and extra fields aren't used. They may be zero, all ones, or Zip64 values, and may disagree when bit 3 is set.
   - The member's data starts at `local offset + 30 + n + m`, and must end at or before the central directory offset (otherwise corrupt).

## 7. Result

`xc_zip_list()` returns every entry (name, method, flags, CRC-32, both sizes, local header offset). `xc_zip_pick_image()` applies §5, and `xc_zip_open_member()` applies §6 and returns the data offset. The CRC-32 is checked when the member's data is read (§9).

## 8. Security

Every offset and length comes from the file, which may be hostile. All reads go through bounds-checked helpers, sums are checked for overflow before use, and the reader allocates at most the 4 MiB directory plus one name per entry. `fuzz_zip` exercises §2–§6 on memory sources, and `fuzz_zip_inflate` exercises §9–§10 on raw Deflate data and on damaged checkpoints (CONTRIBUTING.md#fuzzing). The decompressor's memory is fixed (§10), whatever the input.

## 9. Reading a member

`xc_zip_read_member()` delivers a member's uncompressed bytes **in order, each once**, to a callback, with their offset in the uncompressed image. It starts at offset 0, or at a checkpoint (§10).

1. **Input:** the member's compressed bytes `[data offset, data offset + compressed size)` (§6). If the source can stream (`read_stream`), they are read with one request from the starting point to the member's end, which over HTTP is one `Range` request. Otherwise they are read in pieces with `read_at`.
2. **Stored** members are copied as they are. **Deflate** members go through miniz's `tinfl` as raw Deflate (RFC 1951: no zlib header, no Adler-32), with a 32 KiB wrapping dictionary.
3. **Checks, all fatal:**
   - the Deflate data is invalid, or ends before its last block: corrupt (`XC_ERR_CORRUPT`);
   - the member produces more bytes than its uncompressed size, or fewer by the time its compressed bytes run out: corrupt;
   - the last block ends before the compressed size has been read (trailing bytes): corrupt;
   - when the whole member has been read, the CRC-32 of all its uncompressed bytes (from offset 0, carried across checkpoints) must equal the central directory's (`XC_ERR_ZIP_CRC`).
4. **Stopping early:** the caller may name an offset to stop at (a second pass reads only up to its last late byte, ADR 0009 amendment 1). Bytes from that offset on aren't delivered, the rest of the input isn't read, and there's no CRC check. A non-OK return from the data callback also stops the read and is returned as is.
5. **Checkpoints:** every `checkpoint_every` uncompressed bytes (a multiple of 32 KiB, so a checkpoint always falls where the dictionary wraps), the reader passes the caller a checkpoint (§10) and its position: the uncompressed offset, the compressed bytes consumed, and the CRC-32 so far. A second pass checks re-read bytes by comparing this CRC-32 with pass 1's at the same offset.

## 10. Checkpoints

A checkpoint is everything needed to continue reading a member from an uncompressed offset: miniz's decompressor state, the 32 KiB dictionary, and the positions. It's stored on the console's disk with the install journal, and only the same build reads it back.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `XCZ1` |
| 4 | 4 | build tag: `sizeof(tinfl_decompressor)` XOR (`MZ_VERNUM` << 16). A different miniz version, compiler or CPU word size gives another tag. |
| 8 | 2 | method (0 or 8) |
| 10 | 2 | 0 |
| 12 | 4 | the member's CRC-32 (from the central directory) |
| 16 | 8 | the member's compressed size |
| 24 | 8 | the member's uncompressed size |
| 32 | 8 | uncompressed offset: the next byte to deliver |
| 40 | 8 | compressed bytes consumed: the next byte to read is at data offset + this |
| 48 | 4 | CRC-32 of the uncompressed bytes before the offset |
| 52 | 4 | 0 |
| 56 | `sizeof(tinfl_decompressor)` | the decompressor, as bytes (Deflate only) |
| … | 32,768 | the dictionary (Deflate only) |
| last 4 | 4 | CRC-32 of all the bytes before it |

All integers are little-endian. A checkpoint is about 41 KiB for Deflate and 60 bytes for stored members.

Before resuming, the reader checks:

- the length and the trailing CRC-32 (otherwise corrupt);
- the magic and the build tag (otherwise `XC_ERR_UNSUPPORTED`: the checkpoint is from another build, and the caller should start again from 0);
- the method, CRC-32 and both sizes against the member being read;
- that the offsets lie inside the member and the uncompressed offset is a multiple of 32 KiB.

A checkpoint that fails any of the last two checks is corrupt.

A checkpoint is trusted once these checks pass: only the console writes one, in its own install folder. The trailing CRC-32 catches a damaged disk, not an attacker who can already write to it.
