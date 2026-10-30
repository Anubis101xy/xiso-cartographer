#ifndef XCAT_FOLDER_IMAGE_H
#define XCAT_FOLDER_IMAGE_H

/*
 * An extracted game folder seen as an image (docs/formats/folder-image.md). The platform
 * lists the folder's files and folders; this builds the directory tables of a trimmed xiso
 * that holds them, in memory, and an xc_source that serves the tables from memory and each
 * file's bytes through a callback. xc_install then installs the folder like any image: the
 * same FATX checks, journal, resume and verification. Nothing here touches files or sockets.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_FIMG_MAX_ITEMS 65536U      /* files and folders (§2), as xdvdfs.md §6 allows */
#define XC_FIMG_MAX_PATH 1024U        /* bytes of a path below the folder (§2) */
#define XC_FIMG_MAX_DEPTH 32U         /* names in a path (§2) */
#define XC_FIMG_MAX_NAME 255U         /* bytes of one name (§2) */
#define XC_FIMG_MAX_TABLES (8U << 20) /* bytes of all directory tables (§3) */
#define XC_FIMG_VALIDATOR_HEX 40U     /* §5 */

/* One file or folder of the game folder. */
typedef struct xc_fimg_item {
    const char *path; /* below the game folder, '/'-separated: "media/intro.xmv" */
    uint64_t size;    /* a file's size; ignored for folders */
    uint64_t mtime;   /* when it last changed, in any unit the platform has (§5 only) */
    bool dir;
} xc_fimg_item;

/* Reads `len` bytes at `offset` of items[item], a file, into `buf`: all of them, or an error. */
typedef xc_err (*xc_fimg_read_fn)(void *ctx, size_t item, uint64_t offset, void *buf, size_t len);

/* What xc_fimg_build refused (XC_ERR_GAME_FOLDER, or XC_ERR_INVALID_ARG for a listing that
   breaks §2's structural rules). */
typedef struct xc_fimg_problem {
    size_t item;     /* index into the items; SIZE_MAX for the listing as a whole */
    const char *why; /* a static string */
} xc_fimg_problem;

typedef struct xc_fimg xc_fimg;

/*
 * Builds the image of `count` items (§2-§4). `items` and their paths need only last for the
 * call (the image keeps what it needs); `read` and `ctx` must outlive the image. Fills `src`
 * to read it. XC_ERR_GAME_FOLDER (`problem` says which item and why), XC_ERR_INVALID_ARG,
 * XC_ERR_NOMEM. `problem` may be NULL.
 */
xc_err xc_fimg_build(const xc_fimg_item *items, size_t count, xc_fimg_read_fn read, void *ctx,
                     xc_fimg **out, xc_source *src, xc_fimg_problem *problem);

void xc_fimg_free(xc_fimg *img);

/* §5: lower-case hex SHA-1 of the listing, to pass as the install's ETag. */
const char *xc_fimg_validator(const xc_fimg *img);

/* Files (folders not counted) and their total size. */
size_t xc_fimg_file_count(const xc_fimg *img);
uint64_t xc_fimg_file_bytes(const xc_fimg *img);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_FOLDER_IMAGE_H */
