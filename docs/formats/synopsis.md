# Synopses (`_resources/default.xml`)

**Status:** Normative. Implemented by `xcat/synopsis.h` (`core/src/synopsis.c`), tested by `core/tests/test_synopsis.c` and fuzzed by `fuzz_synopsis.c`. [ADR 0019](../adr/0019-game-art.md).

XBMC4Gamers, and the artwork packs and tools made for it, keep each game's details in `_resources/default.xml` in the game's folder, beside `_resources/artwork/poster.jpg` and the other pictures. A game's details on the console show what it says ([art.md §6](art.md#6-the-details)). There's no formal schema: this is the subset read, as the packs write it. The file comes from a NAS and is untrusted.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```xml
<synopsis>
<title>Example Game: Second Edition</title>
<developer>A Studio</developer>
<publisher>A Publisher</publisher>
<features_general>Players 1-4, 480p, Dolby Digital</features_general>
<features_online>System Link 2-16</features_online>
<esrb>T</esrb>
<esrb_descriptors>Violence</esrb_descriptors>
<genre>Action, Racing</genre>
<release_date>01 Nov 2004</release_date>
<year>2004</year>
<rating>7.9</rating>
<platform>Xbox</platform>
<exclusive>No</exclusive>
<titleid>58430002</titleid>
<overview>What the game is about, a paragraph or two.</overview>
</synopsis>
```

## 2. The file

- At most **64 KiB**; a larger file is refused (`XC_ERR_LIMIT`).
- It may start with a UTF-8 byte order mark, then whitespace, an XML declaration and other `<?…?>` instructions, comments and a `<!DOCTYPE …>`, in any order.
- Then the root element must be `<synopsis>` (attributes allowed, `<synopsis/>` allowed: nothing in it). Anything else is `XC_ERR_CORRUPT`, so a file of another kind isn't read as one.
- The root's children are read in order until `</synopsis>` or the end of the file; a missing end tag isn't an error. Comments between them are skipped.

## 3. Fields

| Element | Field |
|---|---|
| `title` | `title` (160 bytes) |
| `developer`, `publisher` | `developer`, `publisher` (96) |
| `genre` | `genre` (160) |
| `release_date`, `year` | `release_date` (40), `year` (16) |
| `esrb`, `rating` | `esrb`, `rating` (16) |
| `features_general`, `features_online` | `features`, `online` (160) |
| `titleid` | `title_id` (16) |
| `overview` | `overview` (2048) |

Other elements are skipped. The first of each counts; later ones are skipped. A field's text is everything up to the element's own end tag (`</name>` exactly, so `</titlex>` doesn't end `<title>`), or the end of the file:

- `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;` and `&#N;` / `&#xH;` (a code point from 1 to `0x10FFFF`, not a surrogate) are decoded; any other `&` is kept as it is.
- A `<![CDATA[…]]>` section is taken as it is; other markup inside (`<b>`) is left out.
- Bytes that don't form valid UTF-8 (an overlong form, a surrogate, a stray continuation byte) each become `?`.
- Whitespace and control characters (below `0x20`, `0x7F`–`0x9F`) become spaces; runs of them become one, and none is kept at either end.
- Text longer than its field is cut at the end of a whole code point.

## 4. Changes

| Version | Date | Change |
|---|---|---|
| 1 | 2026-11-05 | First version (ADR 0019). |
