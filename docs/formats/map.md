# Map file format (`xcat-map`), version 1

**Status:** Normative. Implemented by `xcat/map.h` (`core/src/map.c`, M4), which links each section it implements; `xcat-cli map` checks a map on a PC. `@archiveorg` generates entries from an archive.org item's listing (M6, [archiveorg.md](archiveorg.md)), and `@smb` from a NAS's shared folder ([smb.md](smb.md), [ADR 0018](../adr/0018-smb-sources.md)).

A map tells xiso-cartographer where the user's own disc images are. It is a plain-text file the user writes by hand or generates, and it is the only place sources come from: the app ships with no links (see [ADR 0006](../adr/0006-no-links-no-bypass.md)).

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-map v1
# Lines starting with # are comments.

# Title ID = URL of an uncompressed image (trimmed .xiso.iso or full-disc .iso)
4D530004       = https://archive.org/download/<item>/<file>.xiso.iso

# A regional variant of the same title, served from a PC on the LAN
4D530004@PAL   = http://192.168.1.20:8080/xbox/Halo (Europe).xiso.iso

# Pull in more entries from another map
@include http://192.168.1.20:8080/map.txt

# Generate entries from an archive.org item the user has access to
@archiveorg <item-identifier>

# Generate entries from the images and game folders in a NAS's shared folder
@smb smb://192.168.1.50/Games/Xbox

# One image, or one extracted game folder (the address ends in /), on a NAS
4D530064       = smb://192.168.1.50/Games/Xbox/Fable (USA).iso
4D530064@EX    = smb://192.168.1.50/Games/Xbox/Fable (USA)/
```

`<item>`, `<file>` and `<item-identifier>` are placeholders. Real maps contain the user's own URLs.

## 2. Encoding and lines

1. A map is UTF-8 text. A leading UTF-8 byte order mark (`EF BB BF`) **must** be ignored. Bytes that aren't valid UTF-8 make the line invalid (§7).
2. Lines end with LF or CRLF. A final line without a line ending is allowed.
3. A line **must not** be longer than **2048 bytes**, excluding the line ending.
4. Leading and trailing spaces and tabs ("whitespace") are ignored on every line.
5. Each line is exactly one of:
   - **blank:** empty after trimming whitespace;
   - **comment:** first character `#`;
   - **directive:** first character `@` (§5);
   - **entry:** anything else (§4).

There are no inline comments. A `#` after the start of a line is part of the line's content.

## 3. Header and versioning

The first line that isn't blank **must** be the header:

```
# xcat-map v<MAJOR>[.<MINOR>]
```

`MAJOR` and `MINOR` are decimal integers without leading zeros. `MINOR` defaults to `0`. Between `#`, `xcat-map` and the version there is one or more spaces or tabs. The header is matched case-sensitively.

A reader that implements version `M.n`:

| File version | Reader behaviour |
|---|---|
| Missing or malformed header | Reject the file: "Not an xcat-map file (missing `# xcat-map v1` header)". |
| Major > `M` | Reject the file: "This map needs a newer xiso-cartographer (map format v*MAJOR*)". |
| Major < `M` | Reject unless the reader explicitly supports that older major. |
| Major = `M`, any minor | Accept. Unknown directives (§5.3) produce warnings, not errors. |

Minor versions only **add** directives. Anything that changes the meaning of an existing line needs a new major version.

## 4. Entries

```
<key> = <url>
```

The line is split at the **first** `=`. Whitespace around the key and the URL is trimmed. Whitespace around the `=` is optional, so columns can be aligned.

### 4.1 Key

```
key        = title-id [ "@" variant ]
title-id   = 8HEXDIG                       ; case-insensitive, normalised to upper case
variant    = 1*16( ALPHA / DIGIT / "-" / "_" )   ; case-insensitive, normalised to upper case
```

- `title-id` is the 32-bit title ID from the game's XBE certificate, as 8 hex digits (e.g. `4D530004`).
- `variant` tells apart releases that share a title ID (regions, revisions). The valid variant names for each title come from the catalog ([catalog.md §4.1](catalog.md#41-variants)). A key whose title or variant isn't in the catalog is accepted and kept, but the app shows it as "not in catalog".
- `4D530004` (no variant) is the title's **default** entry. It's used for any variant of that title that has no entry of its own.

### 4.2 URL

- The URL is the rest of the line after the `=`, trimmed. It **must** start with `http://`, `https://` or `smb://` (scheme case-insensitive). Other schemes are invalid in v1.
- An `smb://` URL names an image on a NAS's shared folder, or an extracted game folder when it ends in `/` ([smb.md §1](smb.md#1-addresses), [§3.1](smb.md#31-game-folders)). It follows smb.md's rules instead of the HTTP ones below: no user name (logins are in the credentials file), no `?` or `#`, at least a share and, unless it ends in `/`, a path inside it, and not a file inside a zip.
- Spaces inside the URL are allowed (`Halo (Europe).xiso.iso`). The reader percent-encodes spaces and non-ASCII characters when it sends the request. Existing `%XX` escapes are kept as they are.
- The URL **must not** contain a literal `#` (fragment) or any control character. Write `%23` for a `#` in a file name.
- The URL **must not** contain user credentials (`user:pass@host`). Such entries are invalid: credentials would sit in plain text on the HDD and in logs. Sources that need a login get it from the separate, host-scoped [credentials file](credentials.md), never from the map.
- The URL **should** point to an uncompressed image, either a trimmed `.xiso.iso` or a full-disc `.iso` dump ([xdvdfs.md §1](xdvdfs.md#1-image-layouts)), on a server that supports HTTP `Range` requests. The layout is detected from the content, not the file name. That is checked at install time, not at load time.
- A URL whose path ends in `.zip` (any case; the query doesn't count) points to a **zip** holding one image, which is installed from it ([zip.md](zip.md), [ADR 0009](../adr/0009-install-from-zip-archives.md)).
- The URL **must not** point to a file inside a zip: a path segment ending in `.zip` followed by more path (`…/game.zip/game.iso`, archive.org's on-the-fly extraction, which can't be resumed). Such entries are invalid, with a message to use the zip's own URL.

### 4.3 Duplicate keys

Entries are processed in order, with includes and generated entries expanded where they appear (§5). If a key appears more than once, the **last** entry wins, and the reader records a warning naming both locations. The same applies to the names of generated entries (§5.2). To override a generated or included entry, put your own entry after the directive.

## 5. Directives

```
@<name> <argument>
```

`name` is lower-case ASCII letters. One or more spaces or tabs separate it from the argument, which is the rest of the line, trimmed.

### 5.1 `@include <location>`

Loads another map and processes its entries at this position.

- `location` is an `http://` or `https://` URL, or a path relative to the directory of the current map. Absolute local paths (`E:\…`, `/…`) **must not** be used, so a map can't reach outside its own folder. Relative paths **must not** contain `..` segments.
- The included file **must** have its own valid header (§3). If it is invalid, the include fails and the `@include` line is reported as an error (§7).
- Includes nest up to **4 levels** deep. Including a file that is already being processed (a cycle) is an error for that `@include` line.
- A relative `@include` inside a map that was itself loaded from a URL resolves against that URL, per RFC 3986 §5.

### 5.2 `@archiveorg <identifier>`

Generates entries from the files of one archive.org item, using the public metadata API (`https://archive.org/metadata/<identifier>`).

- `identifier` is 1–100 characters from `A–Z a–z 0–9 . _ -`.
- The reader fetches `https://archive.org/metadata/<identifier>` (through the same callback as `@include`) and turns the item's image files into entries. Which files, their names, URLs, sizes and hashes, and the errors for missing or taken-down items, are specified in [archiveorg.md](archiveorg.md) ([ADR 0013](../adr/0013-archiveorg-source.md)).
- Generated entries are keyed by **name** (the file name without its extension), not by title ID, and carry the listing's size and hashes. Names are compared ASCII case-insensitively. A listing isn't a map file, so it doesn't count toward the 32 files of §6. At most 32 `@archiveorg` directives are fetched per load; further ones are line errors.
- Generated entries are inserted at the directive's position, so §4.3 applies to them like any other entry.

### 5.3 Unknown directives

A directive name the reader doesn't know is skipped with a warning. That keeps maps written for a newer minor version loadable by older readers.

### 5.4 `@smb <folder>`

Generates entries from a folder on a NAS's shared folder, over SMB ([smb.md](smb.md), [ADR 0018](../adr/0018-smb-sources.md)).

- `folder` is an `smb://host[:port]/share[/path]` address ([smb.md §1](smb.md#1-addresses)), without a user name.
- The reader asks the platform to list it (`xc_map_smb_list_fn`; the map module never opens a socket): its images, and those of the folders directly in it, and its game folders ([smb.md §3](smb.md#3-listing-a-folder-for-smb)). An image's entry is named after its file without the extension, with the listed size; a game folder's is named after the folder, with no size, and its URL ends in `/`.
- Generated entries are keyed by **name**, like `@archiveorg`'s, and inserted at the directive's position (§4.3).
- The login is the one the credentials file gives the NAS's host and port ([credentials.md §4.1](credentials.md#41-smb-logins)), or a guest one.
- A folder that can't be listed (the NAS is off, the login is refused) makes the line an error; one with no images or game folders is a warning. At most 32 `@archiveorg` and `@smb` directives together are listed per load (§6).
- Readers before this one skip it with a warning (§5.3).

## 6. Limits

These protect the 64 MB console from hostile or broken input. A reader **must** enforce them:

| Limit | Value | When exceeded |
|---|---|---|
| File size (each file) | 1 MiB | File rejected |
| Line length | 2048 bytes | Line invalid |
| Total entries after expansion | 10,000 | Loading stops; entries read so far are kept, plus an error |
| Include depth | 4 | `@include` line invalid |
| `@archiveorg` and `@smb` directives per load, together | 32 | that line invalid |
| archive.org listing | 4 MiB ([archiveorg.md §1](archiveorg.md#1-identifier-and-request)) | `@archiveorg` line invalid |
| Images and game folders one `@smb` folder gives | 10,000 ([smb.md §3](smb.md#3-listing-a-folder-for-smb)) | the rest skipped, with a warning |
| Total files loaded (top-level + includes) | 32 | `@include` line invalid |
| Problems kept for display (§7) | 256 | Further problems are counted, not stored |

## 7. Errors and warnings

Every problem is reported with its source (file path or URL) and 1-based line number.

- **File errors** reject that whole file: unreadable, too large, missing or malformed header, unsupported major version.
- **Line errors** skip that line and keep loading: malformed key, missing `=`, invalid URL, invalid directive argument, invalid UTF-8, line too long, failed `@include`, an `@archiveorg` item that can't be fetched, doesn't exist, was taken down or didn't send its metadata, an `@smb` folder that can't be listed (with the reason: no answer, login refused, no such share or folder, no access).
- **Warnings** don't skip anything: duplicate key or name, unknown directive, title or variant not in the catalog, an archive.org item with no image files, files of an item skipped for a bad name, size or URL, malformed hashes ignored, an `@smb` folder with no images or game folders, or more than 10,000, or some whose names an address can't hold.

One typo should never make the whole library disappear. So the app loads every valid line, and it shows errors and warnings on a "Map problems" screen.

## 8. Grammar summary (ABNF, RFC 5234)

```
map         = *blank-line header-line *line
header-line = *WSP "#" 1*WSP "xcat-map" 1*WSP "v" version *WSP eol
version     = major [ "." minor ]
major       = "0" / ( %x31-39 *DIGIT )
minor       = "0" / ( %x31-39 *DIGIT )
line        = blank-line / comment-line / directive-line / entry-line
blank-line  = *WSP eol
comment-line   = *WSP "#" *any eol
directive-line = *WSP "@" 1*%x61-7A 1*WSP argument *WSP eol
entry-line  = *WSP key *WSP "=" *WSP url *WSP eol
key         = 8HEXDIG [ "@" 1*16( ALPHA / DIGIT / "-" / "_" ) ]
url         = ( "http://" / "https://" / "smb://" ) 1*url-char  ; scheme case-insensitive; §4.2
url-char    = %x20-22 / %x24-7E / UTF8-non-ascii      ; printable, no "#"
argument    = 1*( %x21-7E / UTF8-non-ascii ) *( 1*WSP 1*( %x21-7E / UTF8-non-ascii ) )
any         = %x09 / %x20-7E / UTF8-non-ascii
eol         = LF / CRLF / end-of-file
```

`UTF8-non-ascii` is any valid multi-byte UTF-8 sequence (RFC 3629). The prose sections are normative where they add constraints the grammar can't express (limits, path rules, duplicate handling).

## 9. Editing on the console

SETTINGS' Map sources page (M8) edits the top-level map, `E:\xcat\map.txt`, a line at a time (`xcat/map_edit.h`). Maps it includes are never edited.

- **Listed:** every line after the header that is an entry or a directive, in order. Comments and blank lines aren't listed, and invalid lines are, so they can be removed. Each line shows what the reader said about it (§7).
- **Added:** a new line goes at the end of the file, after a line ending if the last line has none. A missing file becomes a new map: the header, a blank line, then the line. Four kinds can be added: `@archiveorg <identifier>` (§5.2, the identifier checked as there), `@include <url>` (§5.1, `http://` or `https://` only, since a path typed on the console is rarely what's meant), `<title ID> = <url>` (§4), and `@smb <folder>`, which SETTINGS > NAS adds in its canonical form ([smb.md §1.4](smb.md#14-canonical-form)), once, when a NAS folder is chosen there. The include and the entry are checked by loading the line alone in a map, so they follow every rule above; an invalid one isn't added and the reason is shown. The title ID's hex digits are written in capitals.
- **Removed:** the whole line, line ending and all.
- Every other byte stays as it was, comments, blank lines and line endings included. A file without a valid header (§3) isn't changed. The result must stay within 1 MiB (§6). On the console the file is replaced atomically: written to `map.txt.tmp`, flushed, then renamed over `map.txt`.
- After each change the app loads the map again, includes, `@archiveorg` items and `@smb` folders too, and lists its games.

## 10. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-13 | First version. |
| 1.0 | 2026-10-15 | Implemented. §6 states the cap of 256 stored problems; nothing else changed. |
| 1.0 | 2026-10-20 | §4.2: zip URLs, and URLs inside a zip are invalid ([ADR 0009](../adr/0009-install-from-zip-archives.md) §8). Before any release, so the version stays 1.0. |
| 1.0 | 2026-10-20 | §5.2: `@archiveorg` is implemented, with entries keyed by name ([ADR 0013](../adr/0013-archiveorg-source.md)); §6, §7 updated. Readers before this one warn and skip it, which §5.2 allowed, so the version stays 1.0. |
| 1.0 | 2026-10-27 | §9: editing on the console (Map sources). The format is unchanged. |
| 1.0 | 2026-10-29 | §4.2: `smb://` URLs; §5.4: `@smb` folders ([ADR 0018](../adr/0018-smb-sources.md)); §6, §7, §8, §9 updated. Older readers reject `smb://` entries as invalid URLs and skip `@smb` with a warning, so the version stays 1.0. |
| 1.0 | 2026-10-30 | §4.2, §5.4: an `smb://` URL ending in `/` is an extracted game folder ([smb.md §3.1](smb.md#31-game-folders)), which may be a whole share. |
