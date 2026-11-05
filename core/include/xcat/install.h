#ifndef XCAT_INSTALL_H
#define XCAT_INSTALL_H

/*
 * One whole install (docs/ARCHITECTURE.md §5): read the image's directory
 * tables, check that FATX can store it (ADR 0008), choose or resume the install
 * folder (docs/formats/journal.md), check free space, extract with the journal,
 * hash the whole image when it's a trimmed xiso (journal.md §7), read every file
 * back and compare its CRC-32, then write the marker and drop the journal. The same code runs on
 * the Xbox and in xcat-cli; the platform provides file access through xc_install_io.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/extract.h"
#include "xcat/fatx.h"
#include "xcat/hash.h"
#include "xcat/sink.h"
#include "xcat/source.h"
#include "xcat/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_INSTALL_MARKER ".xcat-installed"
#define XC_INSTALL_MAX_PATH 1024                    /* folder paths this module builds */
#define XC_INSTALL_MAX_PROBLEMS 8                   /* FATX problems reported (all are counted) */
#define XC_INSTALL_MAX_NUMBERED 99                  /* "Title (2)" ... "Title (99)" (ADR 0008) */
#define XC_INSTALL_CHECKPOINT (64U * 1024U * 1024U) /* bytes between checkpoints (ADR 0011) */
/* Free space a Deflate zip install never lets its spool use (ADR 0009 amendment 1). */
#define XC_INSTALL_SPOOL_RESERVE ((uint64_t)256 * 1024 * 1024)
#define XC_INSTALL_SPOOL_PREFIX ".xcat-spool-" /* + chunk number: spool files in the folder */
/* Resuming a Deflate zip install in the middle of the stream (stream-install.md §7): */
#define XC_INSTALL_STATE_PREFIX ".xcat-state-"             /* + 0 or 1: the engine's state */
#define XC_INSTALL_ZIP_CHECKPOINTS ".xcat-zip-checkpoints" /* the zip checkpoints, in order */

/*
 * File access for the install, in the platform's own path syntax: the module
 * joins paths with `separator`. "Durable" means flushed to the device, so it
 * survives a power cut (on the Xbox, NtFlushBuffersFile).
 */
typedef struct xc_install_io {
    char separator; /* '\\' on the Xbox, '/' on a PC */
    /* True if a file or directory exists at `path`. */
    bool (*exists)(void *ctx, const char *path);
    /* Size of the file at `path`; an error if there is none. */
    xc_err (*file_size)(void *ctx, const char *path, uint64_t *size);
    /* Reads a whole small file into a malloc'd buffer (XC_ERR_LIMIT if over max_len). */
    xc_err (*read_all)(void *ctx, const char *path, size_t max_len, char **data, size_t *len);
    /* Appends to a file (creating it), durably. */
    xc_err (*append)(void *ctx, const char *path, const void *data, size_t len);
    /* Creates or replaces a file with `data`, durably. */
    xc_err (*write_all)(void *ctx, const char *path, const void *data, size_t len);
    xc_err (*remove)(void *ctx, const char *path);
    /* Free bytes on the drive holding `dir`. */
    xc_err (*free_space)(void *ctx, const char *dir, uint64_t *bytes);
    /*
     * A sink writing under `root` (created with missing parents). Its end_file
     * with complete = true must make the file durable before returning, so the
     * journal line that follows describes data on disk (journal.md §2.3).
     */
    xc_err (*open_sink)(void *ctx, const char *root, xc_sink *sink, void **handle);
    void (*close_sink)(void *ctx, void *handle);
    /* A source reading an installed file, for verification. */
    xc_err (*open_file)(void *ctx, const char *path, xc_source *src, void **handle);
    void (*close_file)(void *ctx, void *handle);
    /*
     * Spool files for Deflate zip installs (docs/formats/stream-install.md §4). Optional:
     * without them such installs fail with XC_ERR_UNSUPPORTED. They needn't be durable, and
     * may keep the last file open between calls.
     */
    xc_err (*spool_append)(void *ctx, const char *path, const void *data, size_t len);
    xc_err (*spool_read)(void *ctx, const char *path, uint64_t pos, void *buf, size_t len);
    xc_err (*spool_remove)(void *ctx, const char *path);
    /*
     * Optional: makes every spool append so far durable, in every spool file (§7.1). Without
     * it, a Deflate zip install that's interrupted starts its stream again from the start.
     */
    xc_err (*spool_sync)(void *ctx);
    void *ctx;
} xc_install_io;

typedef enum xc_install_phase {
    XC_INSTALL_READING_TABLES,
    XC_INSTALL_WRITING,
    XC_INSTALL_VERIFYING,
} xc_install_phase;

typedef struct xc_install_progress {
    xc_install_phase phase;
    uint64_t bytes_done; /* of bytes_total, in this phase */
    uint64_t bytes_total;
    size_t files_done;
    size_t file_count;
    const char *current; /* listing path of the file being written or checked */
} xc_install_progress;

/* Return false to cancel (the install fails with XC_ERR_CANCELLED and can be resumed). */
typedef bool (*xc_install_progress_fn)(void *ctx, const xc_install_progress *p);

typedef struct xc_install_req {
    const xc_source *src; /* the image (or zip); read_stream is used when present */
    /* The source is a zip archive holding the image (docs/formats/zip.md): a stored member
       installs like a bare image, a Deflate one from a stream (stream-install.md). */
    bool zip;
    /* The source is a 7z archive (docs/formats/sevenz.md): its one image, streamed like a
       Deflate member, or its game or app folder. max_dict bounds the LZMA dictionary a 7z
       may need (0: no bound; the console passes what its memory allows). */
    bool sevenz;
    size_t max_dict;
    uint64_t spool_reserve; /* Deflate zips: 0 = XC_INSTALL_SPOOL_RESERVE */
    const char *etag;       /* the server's validators, NULL if none (journal.md §4) */
    const char *last_modified;
    const char *source_name; /* URL or path, for the journal and messages */
    const char *games_dir;   /* e.g. "E:\\Games" */
    /* games_dir's length on the console from the '\' after the drive ("\Games": 6), for the
       FATX path limit (ADR 0008); 0 means 6. On the PC games_dir is any path, so it can't say. */
    size_t games_dir_len;
    const char *title; /* the install folder is named after it (ADR 0008) */
    const xc_install_io *io;
    uint8_t *buf; /* I/O buffer, at least 64 KiB; 1 MiB recommended */
    size_t buf_size;
    uint32_t cluster_size;           /* for the free-space estimate; 0 = none (exact sizes) */
    uint64_t coalesce_gap;           /* 0 = XC_EXTRACT_DEFAULT_GAP */
    uint32_t checkpoint_bytes;       /* 0 = XC_INSTALL_CHECKPOINT; tests use small values */
    xc_install_progress_fn progress; /* may be NULL */
    void *progress_ctx;
    /* Whole-image hashes (journal.md §7): computed for trimmed xisos unless `no_hash`. An
       expected value that doesn't match fails the install with XC_ERR_IMAGE_HASH. */
    bool no_hash;
    const uint8_t *expect_md5;  /* XC_MD5_BYTES, or NULL */
    const uint8_t *expect_sha1; /* XC_SHA1_BYTES, or NULL */
    /* The size a listing gives for the source (the zip itself, for a zip; archiveorg.md §8);
       0 for none. Any other size fails with XC_ERR_SOURCE_SIZE before anything is written. */
    uint64_t expect_size;
} xc_install_req;

typedef enum xc_install_hash {
    XC_INSTALL_HASH_NONE,     /* not computed: hash_note says why */
    XC_INSTALL_HASH_COMPUTED, /* md5 and sha1 hold the image's hashes; nothing to compare */
    XC_INSTALL_HASH_MATCHED,  /* computed, and every expected value matched */
} xc_install_hash;

typedef struct xc_install_result {
    char folder[XC_INSTALL_MAX_PATH]; /* where the game went (or would have) */
    bool resumed;                     /* an unfinished install in `folder` was continued */
    const char *other_folder_reason;  /* why an existing folder of that name wasn't used, or NULL */
    size_t files_skipped;             /* already there from an earlier attempt */
    size_t files_continued;           /* resumed inside the file from a checkpoint (ADR 0011) */
    uint64_t bytes_continued;         /* bytes those files already had */
    size_t files_written;
    size_t file_count;
    uint64_t bytes_written;
    uint64_t bytes_read;       /* from the source, including directory tables and gaps */
    xc_install_hash hash;      /* journal.md §7 */
    const char *hash_note;     /* XC_INSTALL_HASH_NONE: why (a static string) */
    bool hash_restarted;       /* a resume had to hash the image from its first byte again */
    uint8_t md5[XC_MD5_BYTES]; /* when computed (also on XC_ERR_IMAGE_HASH) */
    uint8_t sha1[XC_SHA1_BYTES];
    /* From the installed default.xbe (docs/formats/xbe.md), recorded in the marker
       (journal.md §7.1); title_note says why not when has_title_id is false. */
    bool has_title_id;
    uint32_t title_id;
    char title_name[3 * XC_XBE_TITLE_UNITS + 1];
    const char *title_note; /* a static string */
    /* zips (req->zip) */
    char zip_member[256];  /* the image's name in the zip; for a folder, its path and '/' */
    bool archive_folder;   /* the archive held a game or app folder (folder-image.md §7) */
    char folder_why[160];  /* XC_ERR_GAME_FOLDER, XC_ERR_ZIP_UNSUPPORTED: which and why */
    uint16_t zip_method;   /* 0 stored, 8 Deflate; on XC_ERR_ZIP_UNSUPPORTED, the method seen */
    unsigned passes;       /* Deflate: passes over the member (stream-install.md §3) */
    uint64_t reread_bytes; /* Deflate: image bytes read again in passes 2 and later */
    uint64_t spool_peak;   /* Deflate: the spool's largest size */
    size_t files_from_spool;
    uint64_t zip_resumed_at; /* Deflate: the stream offset a resume continued from, or 0 */
    uint64_t space_needed;   /* when XC_ERR_NO_SPACE */
    uint64_t space_free;
    char bad_file[256];     /* XC_ERR_VERIFY: the first file that didn't match */
    char current_file[256]; /* on any failure: the file being written or checked */
    /* XC_ERR_NOT_INSTALLABLE: problems[i].path points into problem_paths[i] (the listing is
       gone by the time xc_install returns), so read them through this struct, not a copy. */
    xc_fatx_problem problems[XC_INSTALL_MAX_PROBLEMS];
    size_t problem_count; /* all of them, even beyond the array */
    char problem_paths[XC_INSTALL_MAX_PROBLEMS][256];
} xc_install_result;

/*
 * Runs the install. Errors: XC_ERR_NOT_INSTALLABLE (result->problems), XC_ERR_NO_SPACE,
 * XC_ERR_IMAGE_HASH (an expected MD5 or SHA-1 didn't match; the journal is kept),
 * XC_ERR_VERIFY (result->bad_file; the journal is kept, so a retry rewrites only what's
 * needed), XC_ERR_FOLDER (no free folder name), XC_ERR_CANCELLED, and whatever the source
 * or the I/O return. Anything but success can be resumed by calling again with the same
 * request: finished files are kept.
 */
xc_err xc_install(const xc_install_req *req, xc_install_result *result);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_INSTALL_H */
