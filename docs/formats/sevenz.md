# 7z archives (`.7z`)

**Status:** Normative. Implemented by `xcat/sevenz.h` (`core/src/sevenz.c`) over the LZMA SDK's 7z reader and decoders (`third_party/lzma`, public domain), and by `xc_install` (`req.sevenz`). Tested by `core/tests/test_sevenz.c` against archives 7-Zip made (`tools/fixtures/sevenz/make.py`) and fuzzed (`fuzz_sevenz.c`). [ADR 0018](../adr/0018-smb-sources.md) decision 9.

A `.7z` holds either a disc image or a game or app folder. The console installs either straight from the archive, on a NAS, an HTTP server or its own drive: nothing is unpacked to a temporary file first, and an image of any size installs with no more memory than the archive's dictionary.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Which files are 7z

A source whose path or URL ends in `.7z` (any case; a URL's query and fragment don't count) is read as a 7z. `@smb` folders list `.7z` files like images ([smb.md §3](smb.md#3-listing-a-folder-for-smb)), named after the file without `.7z`. Its content decides what's installed (§3), not its name.

## 2. The database

The archive's database (its list of files, their sizes, CRC-32s and solid blocks) is read by the SDK's `7zArcIn.c`, which also decodes it when it's compressed (LZMA, as 7-Zip writes it). Every input is treated as hostile:

- No single allocation the reader makes may exceed **16 MiB** (the decoded database, its arrays); a larger one is refused (`XC_ERR_LIMIT`).
- At most **65,536** files and folders; names of at most **1024 bytes** once converted from UTF-16 to UTF-8 (an unpaired surrogate becomes `?`, `\` becomes `/`).
- No 7z signature, a damaged database, or one whose CRC-32 doesn't match: `XC_ERR_NOT_7Z`. A database encrypted with a password (7-Zip's "encrypt file names"): `XC_ERR_7Z_UNSUPPORTED`.

## 3. Image or folder

1. If exactly one file (not a folder) has a name ending in `.iso` or `.xiso` ([zip.md §5.1](zip.md)), that image is installed, wherever it is in the archive.
2. Else, if no file is an image, the archive's game or app folder is installed: the folder of its shallowest `default.xbe` ([folder-image.md §7](folder-image.md#7-archives)).
3. Else (several images): `XC_ERR_ZIP_MEMBER`. So is an archive with neither an image nor a `default.xbe`.

An image stored without compression (method Copy, 7-Zip's level 0) is read in place, at any offset, like a bare image. A compressed image is streamed through the sequential install ([stream-install.md](stream-install.md)), as a Deflate zip member is.

## 4. Methods and memory

Every solid block ("folder" in 7z's words) a file is in must be one of:

| Coders | Notes |
|---|---|
| Copy | No compression |
| LZMA, or LZMA2 | 7-Zip's defaults |
| LZMA or LZMA2, then BCJ (x86) | 7-Zip adds BCJ for executables it recognises |

Anything else is `XC_ERR_7Z_UNSUPPORTED`, and the reason names it: BCJ2 (7-Zip's Ultra level for executables), PPMd, Deflate, BZip2, other branch filters, AES (a password). It's checked for the image, or for every file of the folder, before anything is written.

**Memory.** LZMA needs its dictionary in memory while it decodes. The dictionary a block needs is the one its coder names, but never more than the block's unpacked size (matches can't reach before its first byte), and at least 4 KiB. The installer passes the most it can give (`max_dict`): on the console, the largest block the heap gives at that moment, less 4 MiB for the install's own buffers and the network. A block that needs more is `XC_ERR_7Z_MEMORY`, and the reason gives both sizes. 7-Zip's Normal level uses 16 MiB, Maximum 32 MiB and Ultra 64 MiB; a stock 64 MB console fits Normal and usually Maximum, never Ultra for a large image.

## 5. Reading

- A block is decoded front to back, a 256 KiB buffer at a time, from 64 KiB reads of the archive. A read in the same block at or after the decoder's place decodes forward to it; a read before it starts the block's decoding again from its first byte.
- BCJ is applied to the decoder's output as it goes, keeping back the bytes the converter can't decide yet; at the block's end those are passed as they are, as 7-Zip does.
- **CRC-32:** a file read from its first byte to its last is checked against the archive's CRC-32 for it, if it has one (`XC_ERR_ZIP_CRC`). Data the decoder can't decode is `XC_ERR_CORRUPT`.
- **A folder's files** are laid out in the image in the archive's order ([folder-image.md §4](folder-image.md#4-layout): `order`), so each solid block is decoded once, start to end, however 7-Zip sorted the files in it (by extension, by default).
- **An image's stream** starts at its block's first byte each time it's asked for, and delivers from the offset asked for (`xc_seq_stream_fn`). The sequential install's first pass reads it to its end, so its CRC-32 is checked.
- **Resume.** The decoder's state isn't saved: after a stop or a power cut, an image's stream starts again from the image's first byte, and the files the journal says are finished aren't written again. A folder's files resume as any folder image's do.

## 6. Errors

| Error | When |
|---|---|
| `XC_ERR_NOT_7Z` | §2: not a 7z, or a damaged database |
| `XC_ERR_7Z_UNSUPPORTED` | §2, §4: a password, or a method the console can't decode |
| `XC_ERR_7Z_MEMORY` | §4: the dictionary doesn't fit |
| `XC_ERR_ZIP_MEMBER` | §3: no image and no game folder, or several images |
| `XC_ERR_GAME_FOLDER` | §3: the folder can't be installed ([folder-image.md §2, §7](folder-image.md#2-the-listing)) |
| `XC_ERR_ZIP_CRC`, `XC_ERR_CORRUPT` | §5: damaged data |
| `XC_ERR_LIMIT` | §2: too many files, a name or database too large |

## 7. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-11-01 | First version (ADR 0018 decision 9). |
