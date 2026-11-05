#ifndef XCAT_XDVDFS_H
#define XCAT_XDVDFS_H

/*
 * Xbox disc filesystem (XDVDFS) reader. Behaviour is specified in
 * docs/formats/xdvdfs.md; every input is treated as hostile.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xc_xdvdfs_layout {
    XC_XDVDFS_XISO, /* trimmed image: partition at 0 */
    XC_XDVDFS_XGD1, /* full disc, partition at 0x18300000 */
    XC_XDVDFS_XGD2, /* full disc, partition at 0x0FD90000 */
    XC_XDVDFS_XGD3  /* full disc, partition at 0x02080000 */
} xc_xdvdfs_layout;

typedef struct xc_xdvdfs_volume {
    xc_xdvdfs_layout layout;
    uint64_t partition_offset; /* absolute byte offset of the game partition */
    uint32_t root_sector;      /* partition-relative */
    uint32_t root_size;        /* bytes; 0 with root_sector 0 = no files */
    uint64_t filetime;         /* creation time, Windows FILETIME */
} xc_xdvdfs_volume;

typedef struct xc_xdvdfs_limits {
    uint32_t max_entries;  /* files + directories */
    uint32_t max_depth;    /* root directory = 0 */
    uint32_t max_path_len; /* bytes, excluding the terminator */
} xc_xdvdfs_limits;

typedef struct xc_xdvdfs_entry {
    const char *path;   /* '/'-separated, relative to the root; owned by the listing */
    uint64_t offset;    /* absolute offset of the data (file) or table (directory) */
    uint32_t size;      /* file size, or the directory's table size */
    uint32_t sector;    /* partition-relative start sector */
    uint8_t attributes; /* raw XDVDFS attributes; 0x10 = directory */
} xc_xdvdfs_entry;

typedef struct xc_xdvdfs_listing {
    xc_xdvdfs_entry *files; /* sorted by offset, ties by path */
    size_t file_count;
    xc_xdvdfs_entry *dirs; /* parents before children; includes empty directories */
    size_t dir_count;
    uint64_t total_file_bytes;
    char *paths_; /* storage for every path; internal */
    /* xc_xdvdfs_list_partial() only (§9): tables that weren't available. dir_pending[i] is
       true when dirs[i]'s table wasn't read, so nothing below it is listed; NULL if none. */
    bool *dir_pending;
    size_t pending_count; /* includes the root when root_pending */
    bool root_pending;
} xc_xdvdfs_listing;

/* Fills `out` with the defaults from docs/formats/xdvdfs.md §6. */
void xc_xdvdfs_default_limits(xc_xdvdfs_limits *out);

/*
 * Finds the volume descriptor (xiso, then XGD1/2/3) and validates it.
 * XC_ERR_NOT_XDVDFS if no layout matches; XC_ERR_CORRUPT if one matches but is invalid.
 */
xc_err xc_xdvdfs_open(const xc_source *src, xc_xdvdfs_volume *out);

/*
 * Walks every directory and returns all files and directories. `limits` may be
 * NULL for the defaults. On success the caller frees `out` with
 * xc_xdvdfs_listing_free(); on failure `out` is left empty.
 */
xc_err xc_xdvdfs_list(const xc_source *src, const xc_xdvdfs_volume *vol,
                      const xc_xdvdfs_limits *limits, xc_xdvdfs_listing *out);

/*
 * §10: finds `name` (any case of a-z) among the entries of the root directory only, reading
 * just its table: for a game's default.xbe, without listing the whole image. On success
 * *found says whether it's there and `out` (path NULL) gives it. XC_ERR_CORRUPT for a root
 * table that breaks §3's rules on the way (a loop, an entry outside it); XC_ERR_NOMEM.
 */
xc_err xc_xdvdfs_find_root(const xc_source *src, const xc_xdvdfs_volume *vol, const char *name,
                           xc_xdvdfs_entry *out, bool *found);

/* Frees everything in a listing and zeroes it. Safe on a zeroed listing. */
void xc_xdvdfs_listing_free(xc_xdvdfs_listing *listing);

/* True if the first `len` bytes of the directory table at absolute `offset` can be read. */
typedef bool (*xc_xdvdfs_have_fn)(void *ctx, uint64_t offset, size_t len);

/*
 * Like xc_xdvdfs_list(), for a reader that gets the image as a stream (§9): a table that
 * `have` says isn't available yet isn't read. Its directory is listed and marked pending,
 * and nothing below it is listed. Once every table is available, the result equals
 * xc_xdvdfs_list()'s, checks included.
 */
xc_err xc_xdvdfs_list_partial(const xc_source *src, const xc_xdvdfs_volume *vol,
                              const xc_xdvdfs_limits *limits, xc_xdvdfs_have_fn have,
                              void *have_ctx, xc_xdvdfs_listing *out);

/*
 * Where directory tables sit relative to file data (docs/formats/xdvdfs.md §8). A
 * sequential reader (e.g. decompressing a zip member, ADR 0009) can only write a file as
 * its bytes pass by if every directory table on the file's path has already been read.
 * Offsets are absolute in the image; *_start is UINT64_MAX when there is nothing.
 */
typedef struct xc_xdvdfs_order {
    size_t tables;                  /* directory tables, including the root */
    uint64_t tables_start;          /* first byte of any table */
    uint64_t tables_end;            /* end of the last table */
    uint64_t data_start;            /* first byte of non-empty file data */
    uint64_t data_end;              /* end of the last file data */
    size_t late_files;              /* files that start before a table on their path ends */
    uint64_t late_bytes;            /* total size of those files */
    uint64_t first_late;            /* lowest offset of a late file; a re-read must start here */
    uint64_t last_table_end_needed; /* the latest table end any late file waits for */
} xc_xdvdfs_order;

/* Fills `out` for a listing from xc_xdvdfs_list(). XC_ERR_NOMEM, or XC_ERR_INVALID_ARG. */
xc_err xc_xdvdfs_stream_order(const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                              xc_xdvdfs_order *out);

/*
 * What a sequential install would spool or read twice (docs/formats/xdvdfs.md §8.1). Until
 * the last directory table has been read, a byte that isn't a table or part of a file whose
 * directory is known could still belong to a file: it's "unidentified". The installer
 * spools unidentified bytes to disk and keeps the latest `budget` of them, dropping the
 * oldest when the spool is full. Late file bytes it dropped are read again in a second
 * pass, from the checkpoint at or before the first of them.
 */
typedef struct xc_xdvdfs_spool {
    uint64_t all_known;    /* end of the last table: every byte after it is identified */
    uint64_t unidentified; /* spooling all of these avoids a second pass */
    uint64_t late_bytes;   /* the unidentified bytes that turn out to be file data */
    uint64_t spooled;      /* min(budget, unidentified): peak spool on disk */
    uint64_t reread_start; /* first late byte that was dropped; UINT64_MAX if none */
    uint64_t reread_from;  /* the checkpoint pass 2 restarts from; UINT64_MAX if none */
    uint64_t reread_end;   /* end of the last dropped late byte: pass 2 stops here */
} xc_xdvdfs_spool;

/*
 * Fills `out` for a spool of `budget` bytes and checkpoints every `checkpoint_bytes` of the
 * image (0 = pass 2 starts exactly at reread_start). XC_ERR_NOMEM, or XC_ERR_INVALID_ARG.
 */
xc_err xc_xdvdfs_spool_plan(const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                            uint64_t budget, uint64_t checkpoint_bytes, xc_xdvdfs_spool *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_XDVDFS_H */
