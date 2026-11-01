#ifndef XCAT_ZIP_H
#define XCAT_ZIP_H

/*
 * Zip archives read through an xc_source (docs/formats/zip.md, ADR 0009): find the central
 * directory (Zip64 too), list the entries, choose the image and find where its data starts.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_ZIP_MAX_CD_SIZE (4U * 1024U * 1024U) /* zip.md §4.2 */
#define XC_ZIP_MAX_ENTRIES 16384U               /* zip.md §4.2 */

#define XC_ZIP_STORED 0U
#define XC_ZIP_DEFLATE 8U

/* One central directory entry (zip.md §4). */
typedef struct xc_zip_entry {
    char *name; /* NUL-terminated; holds no NUL itself */
    uint16_t method;
    uint16_t flags;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t size; /* uncompressed */
    uint64_t local_offset;
} xc_zip_entry;

typedef struct xc_zip_listing {
    xc_zip_entry *entries;
    size_t count;
    uint64_t cd_offset; /* every member's data ends at or before this */
    bool zip64;         /* true if the archive has Zip64 end records (zip.md §3) */
} xc_zip_listing;

/* The chosen member, ready to read (zip.md §6). */
typedef struct xc_zip_member {
    size_t index; /* into the listing */
    uint16_t method;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t size;
    uint64_t data_offset; /* the first byte of the member's (compressed) data */
} xc_zip_member;

/*
 * Reads the end records and the central directory (zip.md §2–§4). XC_ERR_NOT_ZIP,
 * XC_ERR_CORRUPT, XC_ERR_LIMIT, XC_ERR_ZIP_UNSUPPORTED (split archives), XC_ERR_NOMEM or a
 * read error. On success the caller frees `out` with xc_zip_listing_free(); on failure it
 * is left empty.
 */
xc_err xc_zip_list(const xc_source *src, xc_zip_listing *out);

/* Frees everything in a listing and zeroes it. Safe on a zeroed listing. */
void xc_zip_listing_free(xc_zip_listing *listing);

/* True if the entry's name ends in .iso or .xiso and it isn't a directory (zip.md §5.1). */
bool xc_zip_is_image_name(const char *name);

/* True if a path or URL names a zip: it ends in .zip (any case), ignoring a URL's query and
   fragment. Installers then install the zip's image (zip.md §1). */
bool xc_zip_is_zip_name(const char *path_or_url);

/* The one image entry (zip.md §5); XC_ERR_ZIP_MEMBER if there are none or several. */
xc_err xc_zip_pick_image(const xc_zip_listing *listing, size_t *index);

/*
 * Checks entry `index` and its local header (zip.md §6) and fills `out`.
 * XC_ERR_ZIP_UNSUPPORTED (method or encryption), XC_ERR_CORRUPT, or a read error.
 */
xc_err xc_zip_open_member(const xc_source *src, const xc_zip_listing *listing, size_t index,
                          xc_zip_member *out);

/* "stored", "Deflate", "Deflate64", "LZMA", "AES-encrypted" ... or NULL if not known. */
const char *xc_zip_method_name(uint16_t method);

/* --- reading a member (zip.md §9–§10) ---------------------------------------------- */

#define XC_ZIP_DICT_SIZE 32768U /* checkpoints fall on multiples of this */

/* Where a read is: the next uncompressed byte, the compressed bytes consumed, the CRC-32 of
   the uncompressed bytes before `offset`. */
typedef struct xc_zip_position {
    uint64_t offset;
    uint64_t compressed;
    uint32_t crc32;
} xc_zip_position;

/* Receives a checkpoint (zip.md §10) to store; non-OK stops the read. */
typedef xc_err (*xc_zip_checkpoint_fn)(void *ctx, const xc_zip_position *pos, const uint8_t *blob,
                                       size_t len);

typedef struct xc_zip_read {
    const xc_source *src;
    const xc_zip_member *member; /* from xc_zip_open_member() */
    xc_source_data_fn on_data;   /* gets the uncompressed bytes in order; offset = in the image */
    void *data_ctx;
    uint64_t checkpoint_every; /* multiple of XC_ZIP_DICT_SIZE; 0 = no checkpoints */
    xc_zip_checkpoint_fn on_checkpoint;
    void *checkpoint_ctx;
    const uint8_t *resume; /* a checkpoint to continue from, or NULL to start at 0 */
    size_t resume_len;
    uint64_t stop_at; /* deliver only bytes before this offset; 0 = the whole member */
    size_t read_size; /* piece size for sources without read_stream; 0 = 64 KiB */
} xc_zip_read;

/*
 * Reads the member (zip.md §9). XC_ERR_CORRUPT (bad Deflate data, wrong sizes, a damaged
 * checkpoint), XC_ERR_ZIP_CRC, XC_ERR_UNSUPPORTED (a checkpoint from another build),
 * XC_ERR_INVALID_ARG, XC_ERR_NOMEM, a read error, or what a callback returned. `end` (may
 * be NULL) gets the position reached.
 */
xc_err xc_zip_read_member(const xc_zip_read *req, xc_zip_position *end);

/* Checks a checkpoint against `member` (zip.md §10) and returns its position. */
xc_err xc_zip_checkpoint_position(const uint8_t *blob, size_t len, const xc_zip_member *member,
                                  xc_zip_position *pos);

/* The length of a checkpoint for a member with this method (0 for unsupported methods). */
size_t xc_zip_checkpoint_size(uint16_t method);

/* --- a game or app folder in a zip (docs/formats/folder-image.md §7) ----------------- */

typedef struct xc_zip_folder xc_zip_folder;

/*
 * The zip's game or app folder as one image (`image`), for a zip with no image of its own:
 * the folder of its shallowest default.xbe and what's in it (xcat/arc_folder.h). Stored and
 * Deflate members; each is inflated as the install reads it, and its CRC-32 checked once read
 * to its end. `src` and `listing` must outlive the folder. XC_ERR_ZIP_MEMBER (no default.xbe),
 * XC_ERR_GAME_FOLDER (`why` says what), XC_ERR_ZIP_UNSUPPORTED (a member's method or
 * encryption, named in `why`), XC_ERR_NOMEM.
 */
xc_err xc_zip_folder_open(const xc_source *src, const xc_zip_listing *listing, xc_zip_folder **out,
                          xc_source *image, char *why, size_t why_cap);
void xc_zip_folder_close(xc_zip_folder *z);
/* The folder's details (root, files, validator). */
const struct xc_arc_folder *xc_zip_folder_arc(const xc_zip_folder *z);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ZIP_H */
