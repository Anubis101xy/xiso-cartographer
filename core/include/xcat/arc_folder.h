#ifndef XCAT_ARC_FOLDER_H
#define XCAT_ARC_FOLDER_H

/*
 * A game or app folder inside an archive (docs/formats/folder-image.md §7): the archive's
 * reader (zip, 7z) lists its members; this finds the folder that holds the game's
 * default.xbe, keeps what's in it, and makes those files one image through the folder image
 * (xcat/folder_image.h), reading each member's bytes through the reader. xc_install then
 * installs it like any image. Nothing here touches files or sockets.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/folder_image.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One member of the archive, as its reader lists it. */
typedef struct xc_arc_member {
    const char *path; /* '/'-separated, as stored (a folder's may end in '/') */
    uint64_t size;    /* uncompressed */
    bool dir;
    uint64_t order; /* where its data is in the archive: members are read in this order */
    uint64_t stamp; /* anything that changes with its content (a CRC, a time), for §5 */
} xc_arc_member;

/* Reads `len` bytes at `offset` of member `member` (an index into the reader's list). */
typedef xc_err (*xc_arc_read_fn)(void *ctx, size_t member, uint64_t offset, void *buf, size_t len);

typedef struct xc_arc_folder xc_arc_folder;

/*
 * §7: finds the game folder among `count` members (the folder of the shallowest default.xbe),
 * and builds the image of what's in it. `members` need only last for the call; `read` and `ctx`
 * must outlive the folder. XC_ERR_GAME_FOLDER (`why` says what), XC_ERR_NOMEM; XC_ERR_ZIP_MEMBER
 * if no member is a default.xbe. `why` may be NULL.
 */
xc_err xc_arc_folder_build(const xc_arc_member *members, size_t count, xc_arc_read_fn read,
                           void *ctx, xc_arc_folder **out, xc_source *image, char *why,
                           size_t why_cap);

void xc_arc_folder_free(xc_arc_folder *f);

/* The game folder's path in the archive ("" for its top), and its files' count and size. */
const char *xc_arc_folder_root(const xc_arc_folder *f);
size_t xc_arc_folder_files(const xc_arc_folder *f);
uint64_t xc_arc_folder_bytes(const xc_arc_folder *f);
/* folder-image.md §5 */
const char *xc_arc_folder_validator(const xc_arc_folder *f);

/* True if archive member `member` is one of the image's files (it's in the game folder). */
bool xc_arc_folder_uses(const xc_arc_folder *f, size_t member);

/* True if the last name of `path` is "default.xbe", any case. */
bool xc_arc_is_default_xbe(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ARC_FOLDER_H */
