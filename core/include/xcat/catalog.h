#ifndef XCAT_CATALOG_H
#define XCAT_CATALOG_H

/*
 * The game catalog (docs/formats/catalog.md, ADR 0014): games, their releases by title ID
 * and variant, and releases by Redump name for map entries keyed by name. The file is
 * checked whole by xc_catalog_open (§7) and read in place: nothing is allocated, and the
 * caller keeps the bytes alive while it uses the catalog.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_CATALOG_MAX_SIZE ((size_t)8 * 1024 * 1024) /* §2 */
#define XC_CATALOG_MAX_COUNT 65535U                   /* §2: games, releases */
#define XC_CATALOG_MAX_STRING 255                     /* §6, bytes without the NUL */
#define XC_CATALOG_MAX_VARIANT 16                     /* §4.1 */
#define XC_CATALOG_VERSION_MAJOR 1
#define XC_CATALOG_VERSION_MINOR 0
#define XC_CATALOG_NONE ((size_t) - 1) /* "not found" from the find functions */

/* An opened catalog. Fields are filled by xc_catalog_open; treat them as read-only. */
typedef struct xc_catalog {
    const uint8_t *data; /* the caller's bytes */
    size_t size;
    uint16_t minor;
    uint32_t game_count;
    uint32_t release_count;
    uint16_t game_size;
    uint16_t release_size;
    uint32_t games_off;
    uint32_t releases_off;
    uint32_t names_off;
    uint32_t strings_off;
    uint32_t strings_size;
    const char *snapshot; /* §2, may be "" */
} xc_catalog;

/* §3. Strings point into the catalog's bytes and are never NULL ("" when unknown). */
typedef struct xc_catalog_game {
    const char *title;
    const char *developer;
    const char *publisher;
    const char *genres;
    uint32_t wikidata; /* Q number; 0 if none */
    uint16_t year;     /* 0 if unknown */
} xc_catalog_game;

/* §4. */
typedef struct xc_catalog_release {
    uint32_t game; /* index of its game */
    uint32_t title_id;
    const char *variant;
    const char *name;   /* Redump name */
    const char *region; /* may be "" */
    uint32_t redump_id; /* 0 if none */
    uint64_t size;      /* of the full-disc image */
    uint32_t crc32;
    uint8_t md5[XC_MD5_BYTES];
    uint8_t sha1[XC_SHA1_BYTES];
} xc_catalog_release;

/*
 * Checks `data` as a catalog (§7) and, on success, fills `cat`. The bytes must stay alive
 * and unchanged while `cat` is used. Errors: XC_ERR_LIMIT if larger than
 * XC_CATALOG_MAX_SIZE, XC_ERR_UNSUPPORTED for another major version, XC_ERR_CATALOG for
 * anything else the format forbids (the CRC included). On failure `cat` is zeroed.
 */
xc_err xc_catalog_open(xc_catalog *cat, const void *data, size_t size);

/* Game or release `i` (< game_count or release_count); false if `i` is out of range. */
bool xc_catalog_game_at(const xc_catalog *cat, size_t i, xc_catalog_game *out);
bool xc_catalog_release_at(const xc_catalog *cat, size_t i, xc_catalog_release *out);

/* The releases of `title_id` are [first, first + *count) (§4: sorted by title ID, then
 * variant). Returns XC_CATALOG_NONE, with *count 0, if there are none. */
size_t xc_catalog_find_title(const xc_catalog *cat, uint32_t title_id, size_t *count);

/* The release with this title ID and variant (ASCII case-insensitive, §6.1), or
 * XC_CATALOG_NONE. */
size_t xc_catalog_find_release(const xc_catalog *cat, uint32_t title_id, const char *variant);

/* The release whose Redump name equals `name` under §6.1 (a map entry's name, map.md §5.2),
 * or XC_CATALOG_NONE. Found through the name index (§5). */
size_t xc_catalog_find_name(const xc_catalog *cat, const char *name);

/* §6.1: <0, 0 or >0 as a sorts before, equal to or after b. Bytes, with A–Z folded. */
int xc_catalog_collate(const char *a, const char *b);

/* True if game `i`'s title, developer or publisher contains `query` (ASCII
 * case-insensitive; "" matches every game). For simple searches by the UI and xcat-cli. */
bool xc_catalog_game_matches(const xc_catalog *cat, size_t i, const char *query);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_CATALOG_H */
