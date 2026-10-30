# Game folders as images

**Status:** Normative. Implemented by `xcat/folder_image.h` (`core/src/folder_image.c`), tested by `core/tests/test_folder_image.c` and `fuzz_folder_image.c`. Used for extracted game folders on a NAS ([smb.md §3.2](smb.md#32-installing-a-game-folder), [ADR 0018](../adr/0018-smb-sources.md) decision 8).

The installer installs images: it reads their directory tables, checks them against FATX, and copies their files with a journal so it can resume ([ADR 0004](../adr/0004-streaming-install.md)). An extracted game folder has no image. Instead of a second installer, the folder's listing is turned into the directory tables of a trimmed xiso that exists only in memory, and its files' bytes are read from the folder as the installer asks for them. Everything the installer does for an image, it then does for a folder.

The image is never written anywhere, and its bytes aren't those any other tool would write for the same folder: only this project's XDVDFS reader ([xdvdfs.md](xdvdfs.md)) reads it, and it must accept every image this builds.

## 1. Input

The platform lists the folder, every level of it, and gives one item per file and folder: its path below the game folder (names joined with `/`), whether it's a folder, a file's size, and when it last changed (any unit; it's used only for §5). The order doesn't matter. What it leaves out is the platform's decision (smb.md §3.2).

## 2. The listing

The image is built only if every item passes:

1. **Structure** (a broken listing: `XC_ERR_INVALID_ARG`): every item has a path, no path is listed twice, and the folder holding each item (the path up to its last `/`) is listed as a folder, unless the item is at the top.
2. **Names** (`XC_ERR_GAME_FOLDER`): every name in a path is 1–255 bytes, not `.` or `..`, with no `\` and no byte below `0x20` ([xdvdfs.md §4](xdvdfs.md#4-names-and-paths)). So no path is empty or has an empty name (a leading, trailing or doubled `/`).
3. **Case** (`XC_ERR_GAME_FOLDER`): no two names in one folder are equal once `a`–`z` are folded to upper case: the reader would reject the image, and FATX couldn't hold both.
4. **Limits** (`XC_ERR_GAME_FOLDER`): a file is under 4 GiB (XDVDFS sizes are 32 bits, and FATX's too); a path is at most 1024 bytes and 32 names deep; there are at most 65,536 items (xdvdfs.md §6); a folder's table fits (§3); all tables together are at most 8 MiB; the image is at most 2³² sectors.

A refusal names the item (or the listing as a whole) and gives the reason, for the log.

## 3. Directory tables

As in [xdvdfs.md §3](xdvdfs.md#3-directory-tables):

- A folder's table holds an entry for each item directly in it, sorted by name with `a`–`z` folded to upper case, then by the unfolded bytes.
- The entries form a balanced binary search tree: the middle entry is the root, the entries before it are its left subtree and those after it its right subtree, each built the same way. Each entry is written before its subtrees (pre-order), so the root is at table offset 0, which is where the reader starts.
- An entry is the 14-byte header and the name, padded to a multiple of 4 bytes. An entry that wouldn't fit in what's left of a 2048-byte sector starts at the next one. Every unused byte of a table is `0xFF`.
- Child offsets are in 4-byte units and must be at most `0xFFFE` (`0xFFFF` would read as padding). A folder whose entries don't fit that is refused (§2).
- Attributes: `0x10` for a folder, `0x20` for a file.
- A folder's entry gives its table's sector and its size rounded up to a whole sector. An empty folder has no table: sector 0, size 0 (xdvdfs.md §3 rule 2).

## 4. Layout

| Bytes | What |
|---|---|
| `0`–`0xFFFF` | Zeros |
| sector 32 (`0x10000`) | The volume descriptor ([xdvdfs.md §2](xdvdfs.md#2-volume-descriptor)): the magic at `0x000` and `0x7EC`, the root table's sector and size, creation time 0, zeros elsewhere. A game folder with nothing in it has root sector 0 and size 0. |
| sector 33 on | The tables: the game folder's first, then each folder's in path order (byte order of the paths, so a folder's table comes after its parent's), each starting at a sector. |
| after the last table | The files' data, in path order, each starting at a sector. An empty file takes no space and has sector 0. |
| the rest of each file's last sector | Zeros |

The image ends at the end of the last file's last sector (or of the last table, if there are no files). Its size is a multiple of 2048 bytes, and it's a trimmed xiso (xdvdfs.md §1), so the reader finds it at offset 0.

Reading the image: bytes in the tables and the descriptor come from memory, padding is zeros, and each file's bytes are asked of the platform by item and offset within the file. A read that spans several files asks for each in turn. Files are in path order, so an install reads each file once, from start to end, one after another.

## 5. Validator

A digest of the listing, given to the installer as the image's ETag ([journal.md §4](journal.md)), so an unfinished install isn't continued from a folder that has changed since: the lower-case hex SHA-1 of, for each item in path order, its path, a 0 byte, `d` (folder) or `f` (file), the file's size (0 for a folder) and its change time, both as 8-byte little-endian numbers.

The same listing in any order gives the same validator; a file added, removed, renamed, resized or rewritten (if the platform's change time changes) gives a different one.

The installer doesn't hash the image ([journal.md §7](journal.md)): it isn't one anyone else has. It still checks each file's CRC-32 after writing it.

## 6. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-30 | First version (ADR 0018 decision 8). |
