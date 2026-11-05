# A game's picture and details

**Status:** Normative. Where things are looked for: `xcat/art.h` (`core/src/art.c`, tested by `test_art.c`). Pictures: `xcat/picture.h` (`core/src/picture.c` over stb_image, `core/src/xpr.c`; `test_picture.c`, `fuzz_picture.c`, `fuzz_xpr.c`). Reading them from a NAS or the console's drive and composing the details: `app/main.c` (`art_frame`). [ADR 0019](../adr/0019-game-art.md).

When the highlight rests on a game in GAMES or INSTALLED, its details show a picture (a cover found beside the game, else the title image inside its `default.xbe`) and what the game's own files say about it: its real title, region and disc from the XBE, and who made it, its genre, year, rating and what it's about from an XBMC4Gamers synopsis. Nothing is fetched from the internet: what's shown is what the owner keeps with the game.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Which games

- **GAMES:** a game on a NAS (`smb://`, [smb.md](smb.md)): an image or archive file, or an extracted game folder. A file in `E:\xcat\isos`. Games from HTTP servers and archive.org have none: reading a cover there would mean downloading it on every visit.
- **INSTALLED:** each install's folder on the console (`E:\Games\<name>`, `E:\Apps\<name>`), which holds the game's `default.xbe`, and its `_resources` if the owner copied them (the XBMC4Gamers artwork installer puts them there).

## 2. Where art is looked for

Paths are '/'-separated and relative to **the game's folder**: an extracted game folder, or an install's folder, is its own; an image's or archive's is the folder the file is in.

An image is **alone in its folder** when that folder isn't one of the map's `@smb` folders: it was listed one level down ([smb.md §3](smb.md#3-listing-a-folder-for-smb)), as in `Xbox\Halo (USA)\Halo (USA).iso`. Then the folder's own files are the game's too. An image in a listed folder shares it with others, so only files named after it count. A file in `E:\xcat\isos` is never alone.

### 2.1 Pictures

In this order; the first that can be read and decoded (§4) is shown:

1. For an image or archive: `<base>.jpg`, `<base>.png`, `<base>.tbn` beside it, where `<base>` is its file name without `.xiso.iso`, `.iso`, `.xiso`, `.zip` or `.7z` (any case), else without its last extension (`Halo (USA).iso` → `Halo (USA)`). A path longer than 191 bytes is skipped.
2. For a game folder, or an image alone in its folder: `_resources/artwork/poster.jpg` (XBMC4Gamers), `default.tbn` (XBMC's thumbnail), `cover.jpg`, `cover.png`, `folder.jpg`, `folder.png`.
3. Else the XBE's title image (§3).

The name's extension doesn't decide what a file is: §4 looks at its content (a `.tbn` is a JPEG; packs ship JPEGs named `.png`).

### 2.2 The synopsis

For a game folder, or an image alone in its folder: `_resources/default.xml`, read as [synopsis.md](synopsis.md).

### 2.3 Names on a NAS

Files are opened by name; whether `Poster.JPG` matches `poster.jpg` is the NAS's choice (Samba and Windows match any case). A file that isn't there costs one request.

## 3. The XBE

- For a game folder or an install: its `default.xbe`.
- For a disc image (`.iso`, `.xiso`): the `default.xbe` in the image's root ([xdvdfs.md §10](xdvdfs.md#10-a-name-in-the-root)), as a window of the image, without listing the image.
- Archives (`.zip`, `.7z`): none. Their `default.xbe` is compressed, and decoding a 7z's solid block to reach it could take a minute.

Its certificate and title image are read as [xbe.md §4–§5](xbe.md#4-region-disc-and-version), and the image decoded as [xpr.md](xpr.md). An XBE that isn't one by xbe.md §3 is ignored.

## 4. Pictures

- A file starting `FF D8 FF` is a JPEG (baseline or progressive); one starting with PNG's signature (`89 50 4E 47 0D 0A 1A 0A`) a PNG. Anything else is skipped (`XC_ERR_UNSUPPORTED`): GIF, BMP and the rest aren't decoded.
- A file over **4 MiB** isn't read. A picture over **2048** pixels on a side, or **1536 × 1536** pixels in all, isn't decoded (its size is read from its header first). Decoding one may allocate at most **32 MiB** in all; past that it stops (`XC_ERR_LIMIT`).
- Decoding is stb_image's (`third_party/stb`), compiled with its JPEG and PNG decoders only, no file I/O, `-fwrapv`, and its asserts off on the console. A picture it can't decode is skipped (`XC_ERR_CORRUPT`), and the next is tried.
- Alpha is kept until §5.

## 5. Fitting

The picture is scaled to fit **154 × 130** pixels (`UI_LIB_ART_W`, `UI_LIB_ART_H`), keeping its shape, at most twice its size, at least 1 × 1. Each pixel of the result is the average of the source pixels it covers (enlarging repeats them), each laid over the details panel's colour (`ui_col_panel`, without its alpha) by its alpha first, so the result is opaque. The details show it centred at their top.

## 6. The details

- **Heading:** the synopsis's `title`, else the XBE's title name, else the list's name. The list keeps its name, so it stays in the order the user knows.
- **Facts line:** for a game the catalog doesn't know: the synopsis's `year` (if the line doesn't have it) and the XBE's region (§6.1); then the item's own facts (its size). Parts are separated by " · ". A game the catalog knows, and an install, keep their line, which has them already.
- **Paragraph:** "By `developer` for `publisher`." (or one of them), the genre, `features_general`, "Online: `features_online`", "ESRB `esrb`", "Rated `rating`", each with a full stop; "Title ID `XXXXXXXX`", with ", disc N" for a second or later disc (disc number + 1, when under 10); then the item's own paragraph (where it comes from); then, on a new line, the synopsis's `overview`.
- White and Black move the paragraph up and down a line when it doesn't fit, and the footer says "White Black more"; another game starts at its top.

### 6.1 Regions

From the XBE's region bits ([xbe.md §4](xbe.md#4-region-disc-and-version)): North America only "NTSC-U", Japan only "NTSC-J", the rest of the world only "PAL", North America and Japan "NTSC-U/J", with the rest of the world "NTSC-U, PAL" or "NTSC-J, PAL", all three "Region free", none "" (left out). The debug bit is ignored.

## 7. When it's read

- Only once the highlight has rested on a game for **350 ms**, so moving through the list reads nothing. The footer says "Reading the game's picture and details..." while a NAS is read; the screen doesn't move meanwhile (on a LAN it takes a fraction of a second; a NAS whose disks were asleep may take several).
- One connection to the NAS per game, closed after. A NAS that doesn't answer leaves the details as they were, and isn't asked again for that game for **60 s**.
- The last **12** games' art is kept, the least recently shown dropped first; a game shown again is drawn at once, without reading anything.
- Each read adds a line to `E:\xcat\install.log` (where the picture came from, its size, whether a synopsis and an XBE were found), and a line for a file that isn't a picture this app reads.

## 8. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-11-05 | First version (ADR 0019). |
