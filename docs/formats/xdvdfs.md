# XDVDFS (Xbox disc filesystem): what the reader accepts

**Status:** Normative for `core/src/xdvdfs.c`. XDVDFS is Microsoft's format, not ours. This document defines how *our* reader interprets it, and which images it rejects as corrupt or hostile. The code links to these sections.

All integers are little-endian. A **sector** is 2048 bytes.

## 1. Image layouts

An image is either a trimmed **xiso** (the game partition starts at byte 0) or a **full-disc** dump, where the game partition starts at a fixed offset after a video partition:

| Layout | Partition offset |
|---|---|
| xiso | `0` |
| XGD1 | `0x18300000` |
| XGD2 | `0x0FD90000` |
| XGD3 | `0x02080000` |

The reader probes the offsets in that order and uses the first one with a valid volume descriptor (§2). It skips any offset whose descriptor would end past the end of the image. The file name and extension are ignored.

Every sector number in the filesystem is **relative to the partition start**. The absolute byte offset of sector *s* is `partition_offset + s × 2048`, computed in 64 bits.

## 2. Volume descriptor

At partition offset `0x10000` (sector 32), 2048 bytes:

| Offset | Size | Field |
|---|---|---|
| `0x000` | 20 | Magic `MICROSOFT*XBOX*MEDIA` |
| `0x014` | 4 | Root directory table sector |
| `0x018` | 4 | Root directory table size (bytes) |
| `0x01C` | 8 | Creation time (Windows FILETIME) |
| `0x024` | 0x7C8 | Unused |
| `0x7EC` | 20 | Magic again |

- The leading magic decides whether a layout matches. If the leading magic matches but the trailing one doesn't, the image is **corrupt** (it isn't skipped).
- Root sector `0` with size `0` means an image with no files. That's valid, and the listing is empty.
- Otherwise the root table (§3) must lie inside the image.

## 3. Directory tables

A directory is a table of entries forming a binary search tree. Its root entry is at table offset 0. Each entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Left child: offset from the table start, in 4-byte units; `0` = none |
| 2 | 2 | Right child: same |
| 4 | 4 | Start sector (file data, or a subdirectory's table) |
| 8 | 4 | Size in bytes (file size, or the subdirectory's table size) |
| 12 | 1 | Attributes; `0x10` = directory |
| 13 | 1 | Name length *n* |
| 14 | *n* | Name (bytes, no terminator) |

Writers pad entries to 4 bytes and fill unused space with `0xFF`, and an entry never crosses a sector boundary. The reader doesn't rely on padding or sector alignment; it follows the child offsets only.

**Reader rules:**

1. Only the first 256 KiB + 2048 bytes of a table are read. Child offsets are 16-bit counts of 4-byte units, so nothing beyond that is reachable.
2. A table of size 0, or whose first two bytes are `0xFFFF`, is an **empty directory**.
3. Every entry must lie entirely inside the bytes read: header and name. A child offset pointing at padding (`0xFFFF` left field, anywhere but the root) or outside the table is **corrupt**.
4. Each entry may be reached only once. Reaching one twice (a cycle, or two parents sharing a child) is **corrupt**. The walk uses an explicit stack, not recursion.
5. The tree's ordering isn't verified. Writers sort by name, but a reader doesn't need that order, so it isn't trusted.

## 4. Names and paths

A name is **invalid**, and the image **corrupt**, if it:

- is empty, `.` or `..`;
- contains `/`, `\` or a byte below `0x20`.

Two names in the same directory that are equal after folding `a`–`z` to upper case make the image **corrupt**: on extraction one would overwrite the other.

Other bytes (including non-ASCII) pass through unchanged; making names safe for FATX is a later step (`fatxname`). Paths in the listing are the names joined with `/`, relative to the image root.

## 5. Files and directories

- An entry with attribute bit `0x10` is a directory. Its sector and size describe its table, which is read in turn (§3).
- Any other entry is a file. A non-empty file's data `[offset, offset + size)` must lie inside the image, otherwise the image is **corrupt** (typically a truncated download). A zero-length file's sector isn't checked.
- File sizes are 32-bit, so every file is under 4 GiB. The image itself can be any size.

## 6. Limits

These bound memory and time on hostile input. They're defaults; callers may tighten them.

| Limit | Default | When exceeded |
|---|---|---|
| Entries (files + directories) | 65,536 | `XC_ERR_LIMIT` |
| Directory depth (root = 0) | 64 | `XC_ERR_LIMIT` |
| Path length | 1024 bytes | `XC_ERR_LIMIT` |

A directory whose table points back to an ancestor is caught by the depth limit.

## 7. Output

`xc_xdvdfs_list()` returns:

- **files**, sorted by absolute offset, with ties (zero-length files) broken by path. This is the order the extractor reads them in ([ADR 0004](../adr/0004-streaming-install.md)).
- **dirs**, parents before children, so they can be created in order. Empty directories are included.
- the total size of all files.

## 8. Stream order

`xc_xdvdfs_stream_order()` reports where the directory tables sit relative to the file data. Random-access sources (local files, HTTP `Range`) read the tables first and don't care. A **sequential** source, such as a Deflate-compressed zip member ([ADR 0009](../adr/0009-install-from-zip-archives.md)), can only write a file as its bytes pass if every table on the file's path has already been read.

- A directory is **known** once its own table and all its ancestors' tables have been read. The root is known at the end of the root table.
- A non-empty file is **late** if it starts before its parent directory is known.
- The report gives the table and data ranges, the number and total size of late files, the lowest late offset (where a second pass would have to start), and the latest table end any late file waits for.

`xcat-cli layout <image>` prints this report for an image on a PC.

### 8.1 Spool plan

`xc_xdvdfs_spool_plan()` sizes the two ways a sequential install can handle late files: spool bytes to disk, or read them again in a second pass. It models an installer that learns the layout only as the stream passes:

- The stream is modelled from the partition offset (nothing before it is game data) to **all known**, the point where every directory table has been read. After it, every byte is identified.
- A byte in that range is **identified** if it is in a directory table, or in a file whose parent directory is already known when the byte passes. A file whose directory becomes known part-way through is identified from that point on. Every other byte is **unidentified**: it might belong to a file listed in a table not yet read. Gaps and late file bytes can't be told apart until all tables are known.
- The installer spools unidentified bytes and keeps the **latest** `budget` of them: when the spool is full, its oldest bytes are dropped. Spooling all of them means no second pass. The peak spool on disk is `min(budget, unidentified)`; once all tables are known, spooled gap bytes are dropped.
- Keeping the latest bytes, not the first, matters: on the retail full-disc dumps measured so far, the unidentified bytes start with hundreds of MiB (up to ~3 GiB) of padding, and the late files sit just before the tables that list them.
- Late file bytes the spool dropped are read again in pass 2: from the checkpoint at or before the first such byte (checkpoints every `checkpoint_bytes` of the image, from offset 0) to the end of the last one.
- Files that share bytes (several entries with the same extent) are counted per byte, not per file.

`xcat-cli layout` prints the plan for several budgets.

## 9. Partial listings

A reader that gets the image as a stream ([stream-install.md](stream-install.md)) has only some directory tables at a time. `xc_xdvdfs_list_partial()` lists with the tables it has:

- before reading a table, it asks the caller whether the table's first bytes (rule 1 of §3) are available;
- an unavailable table's directory is listed and marked **pending**, and nothing below it is listed. An unavailable root gives an empty listing with the root pending;
- every other rule applies as in §3–§7, to what is listed. Limits count only listed entries.

Once every table is available, the result is the same as `xc_xdvdfs_list()`'s.

## 10. A name in the root

A game's details read its `default.xbe` ([art.md §3](art.md#3-the-xbe)) without listing the image: `xc_xdvdfs_find_root()` reads only the root's table (at most the size §6 reads), and visits its entries as §3 does, each at most once (a loop, or a child outside the table, is `XC_ERR_CORRUPT`). It finds the first entry whose name equals the one asked for with `a`–`z` folded to upper case, and gives its offset, size and attributes; directories below aren't read, so the name of a file deeper down isn't found. An empty root finds nothing.
