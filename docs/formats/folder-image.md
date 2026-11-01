# Game folders as images

**Status:** Normative. Implemented by `xcat/folder_image.h` (`core/src/folder_image.c`), tested by `core/tests/test_folder_image.c` and `fuzz_folder_image.c`; §7 by `xcat/arc_folder.h` (`core/src/arc_folder.c`, `zip_folder.c`, and `sevenz.c` for 7z), tested by `test_arc_folder.c` and `test_sevenz.c`. Used for extracted game folders on a NAS ([smb.md §3.2](smb.md#32-installing-a-game-folder), [ADR 0018](../adr/0018-smb-sources.md) decision 8) and for game and app folders in zips and 7z archives (decision 9).

The installer installs images: it reads their directory tables, checks them against FATX, and copies their files with a journal so it can resume ([ADR 0004](../adr/0004-streaming-install.md)). An extracted game folder has no image. Instead of a second installer, the folder's listing is turned into the directory tables of a trimmed xiso that exists only in memory, and its files' bytes are read from the folder as the installer asks for them. Everything the installer does for an image, it then does for a folder.

The image is never written anywhere, and its bytes aren't those any other tool would write for the same folder: only this project's XDVDFS reader ([xdvdfs.md](xdvdfs.md)) reads it, and it must accept every image this builds.

## 1. Input

The platform lists the folder, every level of it, and gives one item per file and folder: its path below the game folder (names joined with `/`), whether it's a folder, a file's size, when it last changed (any unit; it's used only for §5), and an `order` for its data (§4; 0 for all is path order). The order of the items doesn't matter. What it leaves out is the platform's decision (smb.md §3.2, §7 here).

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
| after the last table | The files' data, by `order` and then path order, each starting at a sector. An empty file takes no space and has sector 0. |
| the rest of each file's last sector | Zeros |

The image ends at the end of the last file's last sector (or of the last table, if there are no files). Its size is a multiple of 2048 bytes, and it's a trimmed xiso (xdvdfs.md §1), so the reader finds it at offset 0.

Reading the image: bytes in the tables and the descriptor come from memory, padding is zeros, and each file's bytes are asked of the platform by item and offset within the file. A read that spans several files asks for each in turn. An install reads the files in the image's order, each once from start to end, so a source that must be read in its own order (a 7z's solid blocks) gives that order as `order`.

## 5. Validator

A digest of the listing, given to the installer as the image's ETag ([journal.md §4](journal.md)), so an unfinished install isn't continued from a folder that has changed since: the lower-case hex SHA-1 of, for each item in path order, its path, a 0 byte, `d` (folder) or `f` (file), the file's size (0 for a folder) and its change time, both as 8-byte little-endian numbers.

The same listing in any order gives the same validator; a file added, removed, renamed, resized or rewritten (if the platform's change time changes) gives a different one.

The installer doesn't hash the image ([journal.md §7](journal.md)): it isn't one anyone else has. It still checks each file's CRC-32 after writing it.

## 6. Errors

| Error | When |
|---|---|
| `XC_ERR_GAME_FOLDER` | §2's names, case or limits; §3's table that doesn't fit; §7's several games, `..`, or a name twice. The reason names the item. |
| `XC_ERR_INVALID_ARG` | §2's structure: a listing the platform got wrong (a missing folder, a path twice) |
| `XC_ERR_NOMEM` | Memory for the tables ran out |
| What the platform's read gives | A file that can't be read, or changed size, while the install reads it |

## 7. Archives

A zip or 7z with no image of its own may hold a game or app folder ([zip.md §5](zip.md), [sevenz.md §3](sevenz.md#3-image-or-folder)). Its members become the items of §1:

1. **The game folder** is the folder of the archive's shallowest `default.xbe` (any case; the fewest `/` in its path): the archive's top, or a folder in it (`My App/default.xbe`). Paths are taken with `\` as `/`, without a leading `./` or `/` or a trailing `/`. If two `default.xbe` are equally shallow, the archive holds several games and is refused (`XC_ERR_GAME_FOLDER`); a `default.xbe` deeper down (a tool inside the app) is just one of its files. None at all: `XC_ERR_ZIP_MEMBER`.
2. **What's installed** is every member below the game folder, with the folder's path taken off. Left out: members outside it (a `readme.txt` beside it), and names starting with `.` or named `__MACOSX` at any level (macOS's clutter). A `..` in a path refuses the archive (`XC_ERR_GAME_FOLDER`).
3. **Folders** the paths are in are items even when the archive has no member for them. The same path twice is one item if both are folders; a file listed twice, or a path that's a file and a folder, refuses the archive.
4. Each member's data `order` is where it is in the archive (a zip's local header offset, a 7z's position in its solid blocks), and its stamp for §5 is its CRC-32. Then §2-§5 apply: names, case, sizes and limits as for any folder.
5. **Reading** goes to the archive's reader: a zip's stored member is read in place, a Deflate member inflated from its start (again, if a read goes back), and its CRC-32 checked once it's read to its end (`XC_ERR_ZIP_CRC`); members with other methods or encryption refuse the archive before anything is written (`XC_ERR_ZIP_UNSUPPORTED`, naming the member). 7z: [sevenz.md §5](sevenz.md#5-reading).

## 8. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-30 | First version (ADR 0018 decision 8). |
| 1.1 | 2026-11-01 | §1, §4: `order` lays files' data out; §7: game and app folders in zips and 7z archives (ADR 0018 decision 9). |
