#ifndef XCAT_SEVENZ_H
#define XCAT_SEVENZ_H

/*
 * 7z archives (docs/formats/sevenz.md): the archive's database through the LZMA SDK's reader
 * (third_party/lzma), and each file's bytes decoded as they're needed, a buffer at a time,
 * through LZMA or LZMA2 (with BCJ x86), so an image of any size installs without fitting in
 * memory. Installs take the one image (xc_install streams it like a Deflate zip member) or the
 * game or app folder (xcat/arc_folder.h). Nothing here touches files or sockets.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/arc_folder.h"
#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_7Z_MAX_FILES 65536U       /* §2: larger archives are refused (XC_ERR_LIMIT) */
#define XC_7Z_MAX_HEADER (16U << 20) /* §2: the database, decoded */
#define XC_7Z_MAX_NAME 1024U         /* §2: bytes of a file's UTF-8 name */

typedef struct xc_7z xc_7z;

/*
 * Reads the database of the 7z in `src` (§2). `max_dict` is the largest LZMA dictionary a
 * decoder may allocate (§4; 0: no limit). XC_ERR_NOT_7Z, XC_ERR_7Z_UNSUPPORTED (an encrypted
 * database), XC_ERR_LIMIT, XC_ERR_NOMEM, or a read error; `why` (may be NULL) says more.
 * `src` must outlive the archive.
 */
xc_err xc_7z_open(const xc_source *src, size_t max_dict, xc_7z **out, char *why, size_t why_cap);
void xc_7z_close(xc_7z *a);

/* Its files and folders; file `i`'s UTF-8 path ('/'-separated), size, and whether it's a
   folder. The path stays the archive's. */
size_t xc_7z_count(const xc_7z *a);
const char *xc_7z_path(const xc_7z *a, size_t i);
uint64_t xc_7z_size(const xc_7z *a, size_t i);
bool xc_7z_is_dir(const xc_7z *a, size_t i);

/* §3: the one .iso or .xiso file; XC_ERR_ZIP_MEMBER if there are none or several. */
xc_err xc_7z_pick_image(const xc_7z *a, size_t *index);

/* §4: whether file `i` can be decoded here (its method, its dictionary against max_dict).
   XC_ERR_7Z_UNSUPPORTED or XC_ERR_7Z_MEMORY, with `why`. */
xc_err xc_7z_check(const xc_7z *a, size_t i, char *why, size_t why_cap);

/* True, with the offset of its first byte in the archive, if file `i` is stored without
   compression (copy): it can then be read at any offset straight from the archive. */
bool xc_7z_stored_at(const xc_7z *a, size_t i, uint64_t *offset);

/*
 * Reads [offset, offset + len) of file `i` (§5): decoding forward in its solid block, from
 * the block's start again if the read goes back. Once a file has been read through from its
 * first byte to its last, its CRC-32 is checked (XC_ERR_ZIP_CRC). XC_ERR_CORRUPT,
 * XC_ERR_7Z_UNSUPPORTED, XC_ERR_7Z_MEMORY, XC_ERR_NOMEM, or a read error.
 */
xc_err xc_7z_read(xc_7z *a, size_t i, uint64_t offset, void *buf, size_t len);

/* §5: delivers file `i` from `from` to its end through `fn`, in pieces, each byte once (the
   sequential install's stream: xcat/seq.h). Stops with what `fn` returns. */
xc_err xc_7z_stream(xc_7z *a, size_t i, uint64_t from, xc_source_data_fn fn, void *fn_ctx);

/* The packed bytes file `i`'s solid block takes in the archive (for the journal's identity),
   and its CRC-32 if the archive has one (false if not). */
uint64_t xc_7z_packed_size(const xc_7z *a, size_t i);
bool xc_7z_crc(const xc_7z *a, size_t i, uint32_t *crc);

/* folder-image.md §7: the 7z's game or app folder as one image, its files laid out in the
   archive's order so each solid block is decoded once. `a` must outlive the folder. Errors as
   xc_arc_folder_build, and xc_7z_check's for the first file that can't be decoded. */
xc_err xc_7z_folder_open(xc_7z *a, xc_arc_folder **out, xc_source *image, char *why,
                         size_t why_cap);

/* True if a path or URL names a 7z: it ends in .7z (any case), ignoring a URL's query. */
bool xc_7z_is_7z_name(const char *path_or_url);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SEVENZ_H */
