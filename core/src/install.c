/*
 * One install, start to finish: docs/ARCHITECTURE.md §5, ADR 0004 (stream-extract,
 * resume), ADR 0008 (FATX rules, folder names) and docs/formats/journal.md
 * (§2 life cycle, §6 resuming, §7 verification and whole-image hashes).
 */
#include "xcat/install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/arc_folder.h"
#include "xcat/crc32.h"
#include "xcat/hash.h"
#include "xcat/journal.h"
#include "xcat/seq.h"
#include "xcat/sevenz.h"
#include "xcat/version.h"
#include "xcat/xbe.h"
#include "xcat/xdvdfs.h"
#include "xcat/zip.h"

#define MIN_BUF ((size_t)64 * 1024)
#define JOURNAL_LINE_CAP (XC_JOURNAL_MAX_LINE + 16)

/* --- a source that counts what it reads -------------------------------------------- */

typedef struct counted {
    const xc_source *inner;
    uint64_t bytes;
} counted;

static xc_err counted_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    counted *c = ctx;
    xc_err err = xc_source_read(c->inner, offset, buf, len);
    if (err == XC_OK) {
        c->bytes += len;
    }
    return err;
}

typedef struct counted_stream {
    counted *c;
    xc_source_data_fn fn;
    void *fn_ctx;
} counted_stream;

static xc_err counted_data(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    counted_stream *cs = ctx;
    cs->c->bytes += len;
    return cs->fn(cs->fn_ctx, offset, data, len);
}

static xc_err counted_read_stream(void *ctx, uint64_t offset, uint64_t len, xc_source_data_fn fn,
                                  void *fn_ctx)
{
    counted *c = ctx;
    counted_stream cs = {c, fn, fn_ctx};
    return c->inner->read_stream(c->inner->ctx, offset, len, counted_data, &cs);
}

/* --- state -------------------------------------------------------------------------- */

/* A zip checkpoint kept for later passes (zip.md §10). */
typedef struct zck {
    xc_zip_position pos;
    uint8_t *blob;
    size_t len;
} zck;

/* A file's CRC-32 by path, for verifying a Deflate zip install. */
typedef struct path_crc {
    char *path;
    uint32_t crc;
} path_crc;

typedef struct job {
    const xc_install_req *req;
    const xc_install_io *io;
    xc_install_result *res;
    xc_source src; /* counted */
    counted count;
    xc_source image;      /* the image: src, or a stored zip member's window of it */
    xc_window_source win; /* a stored member */
    /* An archive's game or app folder (folder-image.md §7): its image is made of members. */
    xc_zip_listing folder_zl;
    xc_zip_folder *zip_folder;
    /* 7z archives (sevenz.md): the archive, and the image's index in it, or its folder */
    xc_7z *sevenz;
    size_t seven_index;
    xc_arc_folder *seven_folder;
    bool no_hash; /* not an image anyone has: nothing to hash (folder-image.md §5) */
    /* Deflate zip members (stream-install.md) */
    xc_zip_member member;
    xc_journal_zip_state zjournal;
    zck *zcks; /* sorted by offset */
    size_t nzcks, cap_zcks;
    path_crc *crcs;
    size_t ncrcs, cap_crcs;
    unsigned pass;
    /* stream-install.md §7: saved states */
    bool states;         /* the spool and the sink can be made durable */
    void *seq_ctx;       /* the engine, as the stream function sees it */
    uint64_t state_seq;  /* of the last `state` line, 0 if none */
    size_t state_len[2]; /* the state files' sizes, as this attempt wrote them */
    size_t cks_on_file;  /* checkpoints in the checkpoints file */
    char ck_path[XC_INSTALL_MAX_PATH];
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing listing;
    xc_journal_id id;
    xc_journal_state journal;
    bool *done;
    uint32_t *crc;
    uint32_t *resume_bytes; /* ADR 0011: checkpoints to continue from */
    uint32_t *resume_crc;
    char journal_path[XC_INSTALL_MAX_PATH];
    char line[JOURNAL_LINE_CAP];
    char path[XC_INSTALL_MAX_PATH]; /* scratch */
    xc_install_progress p;
    /* journal.md §7: the whole-image hash, fed by the extractor's on_bytes */
    xc_image_hash *hash;      /* NULL: not hashing */
    bool hash_broken;         /* bytes came out of order (overlapping files) */
    uint64_t hash_next;       /* the image offset the next byte must have */
    uint64_t hash_saved;      /* offset of the last `hash` line written */
    uint32_t hash_save_every; /* image bytes between `hash` lines */
} job;

/* dir + sep + rel, with rel's '/' turned into sep. False if it doesn't fit. */
static bool join(const job *j, char *out, const char *dir, const char *rel)
{
    size_t dl = strlen(dir);
    size_t rl = strlen(rel);
    if (dl + 1 + rl + 1 > XC_INSTALL_MAX_PATH) {
        return false;
    }
    memcpy(out, dir, dl);
    out[dl] = j->io->separator;
    for (size_t i = 0; i < rl; i++) {
        out[dl + 1 + i] = rel[i] == '/' ? j->io->separator : rel[i];
    }
    out[dl + 1 + rl] = '\0';
    return true;
}

static bool report(job *j)
{
    return j->req->progress == NULL || j->req->progress(j->req->progress_ctx, &j->p);
}

/* --- §6: choosing or resuming the folder -------------------------------------------- */

static xc_err choose_folder(job *j)
{
    const xc_install_io *io = j->io;
    char base[XC_FATX_MAX_NAME + 1];
    char name[XC_FATX_MAX_NAME + 1];
    (void)xc_fatx_title_name(j->req->title != NULL ? j->req->title : "", base, XC_FATX_MAX_NAME);
    for (unsigned k = 1; k <= XC_INSTALL_MAX_NUMBERED; k++) {
        if (k == 1) {
            memcpy(name, base, strlen(base) + 1);
        } else {
            (void)xc_fatx_numbered_name(base, k, name);
        }
        char *dir = j->res->folder;
        if (!join(j, dir, j->req->games_dir, name) ||
            !join(j, j->journal_path, dir, XC_JOURNAL_NAME) ||
            !join(j, j->path, dir, XC_INSTALL_MARKER)) {
            return XC_ERR_LIMIT;
        }
        if (!io->exists(io->ctx, dir)) {
            return XC_OK; /* a fresh folder */
        }
        if (io->exists(io->ctx, j->path) || !io->exists(io->ctx, j->journal_path)) {
            /* A finished install, or a folder that isn't ours: never touched (ADR 0008). */
            if (j->res->other_folder_reason == NULL) {
                j->res->other_folder_reason = io->exists(io->ctx, j->path)
                                                  ? "already installed there"
                                                  : "a folder of that name exists";
            }
            continue;
        }
        char *data = NULL;
        size_t len = 0;
        xc_err err = io->read_all(io->ctx, j->journal_path, XC_JOURNAL_MAX_FILE, &data, &len);
        if (err == XC_ERR_NOMEM) {
            return err;
        }
        if (err == XC_OK) {
            bool valid = false;
            const char *why = NULL;
            if (j->id.zip) {
                xc_journal_zip_state_free(&j->zjournal);
                err = xc_journal_read_zip(data, len, &j->id, &j->zjournal);
                valid = j->zjournal.valid;
                why = j->zjournal.why;
            } else {
                xc_journal_state_free(&j->journal);
                err = xc_journal_read(data, len, &j->listing, &j->id, &j->journal);
                valid = j->journal.valid;
                why = j->journal.why;
            }
            free(data);
            if (err != XC_OK) {
                return err;
            }
            if (valid) {
                j->res->resumed = true;
                return XC_OK;
            }
            if (j->res->other_folder_reason == NULL) {
                j->res->other_folder_reason = why;
            }
        } else if (j->res->other_folder_reason == NULL) {
            j->res->other_folder_reason = "its journal can't be read";
        }
    }
    return XC_ERR_FOLDER;
}

/* §6.5: a done file must still be there with its size, or it's written again. */
static void check_done_files(job *j)
{
    for (size_t i = 0; i < j->listing.file_count; i++) {
        if (!j->done[i]) {
            continue;
        }
        uint64_t size = 0;
        if (!join(j, j->path, j->res->folder, j->listing.files[i].path) ||
            j->io->file_size(j->io->ctx, j->path, &size) != XC_OK ||
            size != j->listing.files[i].size) {
            j->done[i] = false;
        }
    }
}

/* --- extraction callbacks ----------------------------------------------------------- */

static bool on_extract_progress(void *ctx, const xc_extract_progress *ep)
{
    job *j = ctx;
    j->p.phase = XC_INSTALL_WRITING;
    j->p.bytes_done = ep->bytes_done;
    j->p.bytes_total = ep->bytes_total;
    j->p.files_done = ep->files_done;
    j->p.file_count = ep->file_count;
    j->p.current = ep->current;
    return report(j);
}

/* journal.md §7: every image byte, in order, goes through the hash. */
static xc_err on_bytes(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    job *j = ctx;
    if (j->hash == NULL || j->hash_broken) {
        return XC_OK;
    }
    if (offset != j->hash_next) {
        j->hash_broken = true; /* files overlap: a span went back (journal.md §7) */
        return XC_OK;
    }
    xc_image_hash_update(j->hash, data, len);
    j->hash_next += len;
    return XC_OK;
}

/* journal.md §5: after a done or part line, a `hash` line once 64 MiB more were hashed. */
static xc_err save_hash(job *j)
{
    if (j->hash == NULL || j->hash_broken || j->hash_next - j->hash_saved < j->hash_save_every) {
        return XC_OK;
    }
    uint8_t md5[XC_MD5_BYTES];
    uint8_t sha1[XC_SHA1_BYTES];
    uint64_t off = xc_image_hash_save(j->hash, md5, sha1);
    size_t len = 0;
    xc_err err = xc_journal_format_hash(off, md5, sha1, j->line, sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    if (err == XC_OK) {
        j->hash_saved = off;
    }
    return err;
}

/* journal.md §2.3: the sink made the file durable; now record it, durably. */
static xc_err on_file_done(void *ctx, size_t index, uint32_t crc)
{
    job *j = ctx;
    const xc_xdvdfs_entry *f = &j->listing.files[index];
    j->crc[index] = crc;
    j->done[index] = true;
    size_t len = 0;
    xc_err err = xc_journal_format_done(f->path, f->size, crc, j->line, sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    return err == XC_OK ? save_hash(j) : err;
}

/* ADR 0011: the sink flushed the first `bytes` bytes; record the checkpoint, durably. */
static xc_err on_checkpoint(void *ctx, size_t index, uint32_t bytes, uint32_t crc)
{
    job *j = ctx;
    size_t len = 0;
    xc_err err = xc_journal_format_part(j->listing.files[index].path, bytes, crc, j->line,
                                        sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    return err == XC_OK ? save_hash(j) : err;
}

/*
 * journal.md §6.7 and §7: whether to hash, and from where. Returns the image offset the
 * whole-image read starts at. A journal's hash state is used only if nothing still to be
 * written starts before it; otherwise hashing starts again at byte 0.
 */
static xc_err start_hash(job *j, uint64_t *from)
{
    *from = 0;
    if (j->req->no_hash || j->no_hash) {
        j->res->hash_note = "not asked for";
        return XC_OK;
    }
    if (j->vol.layout != XC_XDVDFS_XISO) {
        j->res->hash_note = "full-disc layout: checked by size and CRC-32 only (ADR 0004 §5)";
        return XC_OK;
    }
    xc_err err = xc_image_hash_create(&j->hash);
    if (err != XC_OK) {
        return err;
    }
    if (!j->res->resumed) {
        return XC_OK;
    }
    uint64_t first_needed = UINT64_MAX;
    bool any_kept = false;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &j->listing.files[i];
        any_kept = any_kept || j->done[i] || j->resume_bytes[i] > 0;
        if (!j->done[i] && f->size > 0 && f->offset + j->resume_bytes[i] < first_needed) {
            first_needed = f->offset + j->resume_bytes[i];
        }
    }
    if (j->journal.has_hash && j->journal.hash_offset <= first_needed) {
        err = xc_image_hash_restore(j->hash, j->journal.hash_offset, j->journal.hash_md5,
                                    j->journal.hash_sha1);
        if (err == XC_OK) {
            *from = j->journal.hash_offset;
        }
        return err;
    }
    j->res->hash_restarted = any_kept;
    return XC_OK;
}

/* journal.md §7: the result, and the comparison with the expected values. */
static xc_err finish_hash(job *j)
{
    if (j->hash == NULL) {
        return XC_OK;
    }
    if (j->hash_broken || j->hash_next != j->image.size) {
        j->res->hash_note = "files overlap in the image, so it couldn't be read front to back";
        return XC_OK;
    }
    xc_image_hash_final(j->hash, j->res->md5, j->res->sha1);
    bool md5_bad =
        j->req->expect_md5 != NULL && memcmp(j->req->expect_md5, j->res->md5, XC_MD5_BYTES) != 0;
    bool sha1_bad = j->req->expect_sha1 != NULL &&
                    memcmp(j->req->expect_sha1, j->res->sha1, XC_SHA1_BYTES) != 0;
    j->res->hash = j->req->expect_md5 != NULL || j->req->expect_sha1 != NULL
                       ? XC_INSTALL_HASH_MATCHED
                       : XC_INSTALL_HASH_COMPUTED;
    if (md5_bad || sha1_bad) {
        j->res->hash = XC_INSTALL_HASH_COMPUTED;
        return XC_ERR_IMAGE_HASH;
    }
    return XC_OK;
}

/* journal.md §6.6: an unfinished file continues at its last checkpoint if the file on disk is
   at least that long; otherwise it's written from the start. */
static void check_checkpoints(job *j)
{
    for (size_t i = 0; i < j->listing.file_count; i++) {
        uint32_t keep = j->journal.part_bytes[i];
        uint64_t size = 0;
        if (j->done[i] || keep == 0 ||
            !join(j, j->path, j->res->folder, j->listing.files[i].path) ||
            j->io->file_size(j->io->ctx, j->path, &size) != XC_OK || size < keep) {
            continue;
        }
        j->resume_bytes[i] = keep;
        j->resume_crc[i] = j->journal.part_crc[i];
        j->res->files_continued++;
        j->res->bytes_continued += keep;
    }
}

/* --- §7: read-back verification ----------------------------------------------------- */

static xc_err verify(job *j)
{
    const xc_install_io *io = j->io;
    j->p.phase = XC_INSTALL_VERIFYING;
    j->p.bytes_done = 0;
    j->p.bytes_total = j->listing.total_file_bytes;
    j->p.files_done = 0;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        const xc_xdvdfs_entry *f = &j->listing.files[i];
        j->p.current = f->path;
        if (!join(j, j->path, j->res->folder, f->path)) {
            return XC_ERR_LIMIT;
        }
        xc_source src;
        void *h = NULL;
        xc_err err = io->open_file(io->ctx, j->path, &src, &h);
        bool ok = err == XC_OK && src.size == f->size;
        uint32_t crc = 0;
        for (uint64_t pos = 0; ok && pos < f->size;) {
            size_t n =
                f->size - pos < j->req->buf_size ? (size_t)(f->size - pos) : j->req->buf_size;
            err = xc_source_read(&src, pos, j->req->buf, n);
            ok = err == XC_OK;
            if (ok) {
                crc = xc_crc32(crc, j->req->buf, n);
                pos += n;
                j->p.bytes_done += n;
                if (!report(j)) {
                    io->close_file(io->ctx, h);
                    return XC_ERR_CANCELLED;
                }
            }
        }
        if (h != NULL) {
            io->close_file(io->ctx, h);
        }
        if (!ok || crc != j->crc[i]) {
            (void)snprintf(j->res->bad_file, sizeof j->res->bad_file, "%s", f->path);
            /* Deleting it makes the next attempt write it again (§6.5); the journal stays. */
            (void)io->remove(io->ctx, j->path);
            return err == XC_ERR_NOMEM ? err : XC_ERR_VERIFY;
        }
        j->p.files_done++;
    }
    return XC_OK;
}

/* --- the install ------------------------------------------------------------------- */

/* The title ID and name from the installed default.xbe (docs/formats/xbe.md); a game without
 * a readable one still installs. */
static void read_title(job *j)
{
    xc_install_result *res = j->res;
    xc_source src;
    void *handle = NULL;
    res->title_note = "the game has no default.xbe";
    if (!join(j, j->path, res->folder, "default.xbe") ||
        j->io->open_file(j->io->ctx, j->path, &src, &handle) != XC_OK) {
        return;
    }
    xc_xbe_cert cert;
    xc_err err = xc_xbe_read_cert(&src, &cert);
    j->io->close_file(j->io->ctx, handle);
    if (err != XC_OK) {
        res->title_note = err == XC_ERR_CORRUPT ? "default.xbe isn't an XBE" : xc_err_str(err);
        return;
    }
    res->has_title_id = true;
    res->title_id = cert.title_id;
    memcpy(res->title_name, cert.title, sizeof res->title_name);
    res->title_note = NULL;
}

/* journal.md §7.1 */
static xc_err write_marker(job *j)
{
    read_title(j);
    char text[XC_INSTALL_MAX_PATH + 512];
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    char title[sizeof j->res->title_name + 32] = "";
    xc_hex_lower(j->res->md5, XC_MD5_BYTES, md5);
    xc_hex_lower(j->res->sha1, XC_SHA1_BYTES, sha1);
    bool hashed = j->res->hash != XC_INSTALL_HASH_NONE;
    if (j->res->has_title_id) {
        (void)snprintf(title, sizeof title, "title_id=%08lX\ntitle=%s\n",
                       (unsigned long)j->res->title_id, j->res->title_name);
    }
    int n =
        snprintf(text, sizeof text,
                 "xiso-cartographer %s\nimage=%s\nfiles=%zu\nbytes=%llu\nverified=%s\n%s%s%s%s%s%s",
                 xc_version_string(), j->req->source_name != NULL ? j->req->source_name : "",
                 j->listing.file_count, (unsigned long long)j->listing.total_file_bytes,
                 j->res->hash == XC_INSTALL_HASH_MATCHED ? "crc32,image-hash" : "crc32",
                 hashed ? "md5=" : "", hashed ? md5 : "", hashed ? "\nsha1=" : "",
                 hashed ? sha1 : "", hashed ? "\n" : "", title);
    if (n < 0 || !join(j, j->path, j->res->folder, XC_INSTALL_MARKER)) {
        return XC_ERR_LIMIT;
    }
    size_t len = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1;
    return j->io->write_all(j->io->ctx, j->path, text, len);
}

/* ADR 0008: the paths are what they'd be under <games dir>\<folder> on the console. */
static xc_err check_fatx(job *j, const xc_xdvdfs_listing *listing)
{
    xc_install_result *res = j->res;
    const char *name = strrchr(res->folder, j->io->separator);
    name = name != NULL ? name + 1 : res->folder;
    size_t dir_len = j->req->games_dir_len != 0 ? j->req->games_dir_len : strlen("\\Games");
    xc_err err = xc_fatx_check_listing(listing, dir_len + 1U + strlen(name), res->problems,
                                       XC_INSTALL_MAX_PROBLEMS, &res->problem_count);
    if (err == XC_OK && res->problem_count > 0) {
        for (size_t i = 0; i < res->problem_count && i < XC_INSTALL_MAX_PROBLEMS; i++) {
            (void)snprintf(res->problem_paths[i], sizeof res->problem_paths[i], "%s",
                           res->problems[i].path);
            res->problems[i].path = res->problem_paths[i];
        }
        err = XC_ERR_NOT_INSTALLABLE;
    }
    return err;
}

static uint64_t round_up(uint64_t v, uint32_t cluster)
{
    return cluster == 0 ? v : (v + cluster - 1U) / cluster * cluster;
}

static xc_err run_install(job *j)
{
    const xc_install_req *req = j->req;
    const xc_install_io *io = j->io;
    xc_install_result *res = j->res;

    j->p.phase = XC_INSTALL_READING_TABLES;
    if (!report(j)) {
        return XC_ERR_CANCELLED;
    }
    xc_err err = xc_xdvdfs_open(&j->image, &j->vol);
    if (err == XC_OK) {
        err = xc_xdvdfs_list(&j->image, &j->vol, NULL, &j->listing);
    }
    if (err != XC_OK) {
        return err;
    }
    res->file_count = j->listing.file_count;
    size_t files = j->listing.file_count > 0 ? j->listing.file_count : 1;
    j->done = calloc(files, sizeof *j->done);
    j->crc = calloc(files, sizeof *j->crc);
    j->resume_bytes = calloc(files, sizeof *j->resume_bytes);
    j->resume_crc = calloc(files, sizeof *j->resume_crc);
    if (j->done == NULL || j->crc == NULL || j->resume_bytes == NULL || j->resume_crc == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_journal_make_id(&j->id, &j->listing, j->vol.partition_offset, j->image.size, req->etag,
                       req->last_modified);

    err = choose_folder(j);
    if (err != XC_OK) {
        return err;
    }
    err = check_fatx(j, &j->listing);
    if (err != XC_OK) {
        return err;
    }
    if (res->resumed) {
        for (size_t i = 0; i < j->listing.file_count; i++) {
            j->done[i] = j->journal.done[i];
            j->crc[i] = j->journal.crc[i];
        }
        check_done_files(j);
        check_checkpoints(j);
    }

    /* Free space for everything still to be written, cluster-rounded (as M2 did). */
    uint64_t need = (uint64_t)(j->listing.dir_count + 2U) * req->cluster_size;
    for (size_t i = 0; i < j->listing.file_count; i++) {
        if (!j->done[i]) {
            need += round_up(j->listing.files[i].size - j->resume_bytes[i], req->cluster_size);
        }
    }
    uint64_t free_bytes = 0;
    err = io->free_space(io->ctx, req->games_dir, &free_bytes);
    if (err != XC_OK) {
        return err;
    }
    if (free_bytes < need) {
        res->space_needed = need;
        res->space_free = free_bytes;
        return XC_ERR_NO_SPACE;
    }

    xc_sink sink;
    void *sink_handle = NULL;
    err = io->open_sink(io->ctx, res->folder, &sink, &sink_handle);
    if (err != XC_OK) {
        return err;
    }
    if (!res->resumed) {
        /* journal.md §2.2: the journal comes before any game file. */
        size_t len = 0;
        char *start = malloc((size_t)3 * XC_JOURNAL_MAX_LINE);
        err = start != NULL ? xc_journal_format_start(&j->id, req->source_name, start,
                                                      (size_t)3 * XC_JOURNAL_MAX_LINE, &len)
                            : XC_ERR_NOMEM;
        if (err == XC_OK) {
            err = io->append(io->ctx, j->journal_path, start, len);
        }
        free(start);
    }
    if (err == XC_OK) {
        xc_extract_opts o;
        memset(&o, 0, sizeof o);
        o.buf = req->buf;
        o.buf_size = req->buf_size;
        o.coalesce_gap = req->coalesce_gap != 0 ? req->coalesce_gap : XC_EXTRACT_DEFAULT_GAP;
        o.progress = on_extract_progress;
        o.progress_ctx = j;
        o.file_done = on_file_done;
        o.file_done_ctx = j;
        o.checkpoint_every =
            req->checkpoint_bytes != 0 ? req->checkpoint_bytes : XC_INSTALL_CHECKPOINT;
        o.checkpoint = on_checkpoint;
        o.resume_bytes = j->resume_bytes;
        o.resume_crc = j->resume_crc;
        uint64_t from = 0;
        err = start_hash(j, &from);
        j->hash_next = from;
        j->hash_saved = from;
        j->hash_save_every = o.checkpoint_every;
        o.whole_image = j->hash != NULL;
        o.whole_from = from;
        o.on_bytes = on_bytes;
        o.on_bytes_ctx = j;
        /* `skip` must not change while extracting: on_file_done marks files in a copy. */
        bool *skip = err == XC_OK ? malloc(files * sizeof *skip) : NULL;
        if (skip == NULL) {
            err = err == XC_OK ? XC_ERR_NOMEM : err;
        } else {
            memcpy(skip, j->done, files * sizeof *skip);
            o.skip = skip;
            xc_extract_stats st;
            err = xc_extract(&j->image, &j->listing, &sink, &o, &st);
            res->files_skipped = st.files_skipped;
            res->files_written = st.files_written;
            res->bytes_written = st.bytes_written;
            free(skip);
        }
    }
    io->close_sink(io->ctx, sink_handle);
    if (err == XC_OK) {
        err = finish_hash(j);
    }
    if (err == XC_OK) {
        err = verify(j);
    }
    if (err == XC_OK) {
        err = write_marker(j);
    }
    if (err == XC_OK) {
        (void)io->remove(io->ctx, j->journal_path); /* §2.4: finished */
    }
    return err;
}

/* --- Deflate zip members (stream-install.md, zip.md §9-§10) ----------------------------- */

static uint64_t zip_checkpoint_every(const job *j)
{
    uint64_t every =
        j->req->checkpoint_bytes != 0 ? j->req->checkpoint_bytes : XC_INSTALL_CHECKPOINT;
    every = (every + XC_ZIP_DICT_SIZE - 1U) / XC_ZIP_DICT_SIZE * XC_ZIP_DICT_SIZE;
    return every;
}

/* Keeps a checkpoint in memory (sorted, one per offset). */
static xc_err keep_checkpoint(job *j, size_t at, const xc_zip_position *pos, const uint8_t *blob,
                              size_t len)
{
    if (j->nzcks == j->cap_zcks) {
        size_t cap = j->cap_zcks > 0 ? j->cap_zcks * 2 : 32;
        zck *v = realloc(j->zcks, cap * sizeof *v);
        if (v == NULL) {
            return XC_ERR_NOMEM;
        }
        j->zcks = v;
        j->cap_zcks = cap;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(copy, blob, len);
    memmove(j->zcks + at + 1, j->zcks + at, (j->nzcks - at) * sizeof *j->zcks);
    j->zcks[at].pos = *pos;
    j->zcks[at].blob = copy;
    j->zcks[at].len = len;
    j->nzcks++;
    return XC_OK;
}

static size_t checkpoint_at(const job *j, uint64_t offset)
{
    size_t at = 0;
    while (at < j->nzcks && j->zcks[at].pos.offset < offset) {
        at++;
    }
    return at;
}

static bool state_path(const job *j, uint64_t seq, char *out)
{
    char name[32];
    (void)snprintf(name, sizeof name, "%s%u", XC_INSTALL_STATE_PREFIX, (unsigned)(seq % 2U));
    return join(j, out, j->res->folder, name);
}

/*
 * stream-install.md §7.1: the engine's state at `offset` goes to the state file not named by
 * the last `state` line, then a new `state` line names it.
 */
static xc_err save_state(job *j, uint64_t offset)
{
    uint8_t *state = NULL;
    size_t len = 0;
    xc_err err = xc_seq_save(j->seq_ctx, offset, &state, &len);
    if (err != XC_OK || state == NULL) {
        return err;
    }
    char path[XC_INSTALL_MAX_PATH];
    uint64_t seq = j->state_seq + 1;
    err = state_path(j, seq, path) ? j->io->write_all(j->io->ctx, path, state, len) : XC_ERR_LIMIT;
    uint32_t crc = xc_crc32(0, state, len);
    free(state);
    size_t n = 0;
    if (err == XC_OK) {
        err = xc_journal_format_state(seq, offset, (uint32_t)len, crc, j->line, sizeof j->line, &n);
    }
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, n);
    }
    if (err == XC_OK) {
        j->state_seq = seq;
        j->state_len[seq % 2U] = len;
    }
    return err;
}

/*
 * Keeps a checkpoint, or checks a later pass against the one pass 1 kept (zip.md §9.5); then
 * saves the engine's state there (stream-install.md §7.1). A new checkpoint is appended to
 * the checkpoints file first, so a state never names an offset without one.
 */
static xc_err on_zip_checkpoint(void *ctx, const xc_zip_position *pos, const uint8_t *blob,
                                size_t len)
{
    job *j = ctx;
    size_t at = checkpoint_at(j, pos->offset);
    xc_err err = XC_OK;
    if (at < j->nzcks && j->zcks[at].pos.offset == pos->offset) {
        /* the same bytes again: their CRC-32 so far must match */
        if (j->zcks[at].pos.crc32 != pos->crc32) {
            return XC_ERR_ZIP_CRC;
        }
    } else {
        err = keep_checkpoint(j, at, pos, blob, len);
        if (err == XC_OK && j->states) {
            err = j->io->append(j->io->ctx, j->ck_path, blob, len);
            j->cks_on_file += err == XC_OK ? 1U : 0U;
        }
    }
    if (err == XC_OK && j->states) {
        err = save_state(j, pos->offset);
    }
    return err;
}

/* xc_seq_stream_fn: the member from the last checkpoint at or before `from`. */
/* sevenz.md §5: the image from a 7z, decoded from its block's start each pass. */
static xc_err sevenz_stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    job *j = ctx;
    j->pass++;
    j->seq_ctx = fn_ctx;
    return xc_7z_stream(j->sevenz, j->seven_index, from, fn, fn_ctx);
}

static xc_err zip_stream(void *ctx, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    job *j = ctx;
    const zck *best = NULL;
    for (size_t i = 0; i < j->nzcks && j->zcks[i].pos.offset <= from; i++) {
        best = &j->zcks[i];
    }
    xc_zip_read q;
    memset(&q, 0, sizeof q);
    q.src = &j->src;
    q.member = &j->member;
    q.on_data = fn;
    q.data_ctx = fn_ctx;
    q.checkpoint_every = zip_checkpoint_every(j);
    q.on_checkpoint = on_zip_checkpoint;
    q.checkpoint_ctx = j;
    q.resume = best != NULL ? best->blob : NULL;
    q.resume_len = best != NULL ? best->len : 0;
    j->pass++;
    j->seq_ctx = fn_ctx;
    return xc_zip_read_member(&q, NULL);
}

/* xc_seq_spool on the install folder's spool files. */
static bool spool_path(const job *j, uint32_t chunk, char *out)
{
    char name[48];
    (void)snprintf(name, sizeof name, "%s%u", XC_INSTALL_SPOOL_PREFIX, (unsigned)chunk);
    return join(j, out, j->res->folder, name);
}

static xc_err spool_append(void *ctx, uint32_t chunk, const uint8_t *data, size_t len)
{
    job *j = ctx;
    char path[XC_INSTALL_MAX_PATH];
    return spool_path(j, chunk, path) ? j->io->spool_append(j->io->ctx, path, data, len)
                                      : XC_ERR_LIMIT;
}

static xc_err spool_read(void *ctx, uint32_t chunk, uint64_t pos, uint8_t *buf, size_t len)
{
    job *j = ctx;
    char path[XC_INSTALL_MAX_PATH];
    return spool_path(j, chunk, path) ? j->io->spool_read(j->io->ctx, path, pos, buf, len)
                                      : XC_ERR_LIMIT;
}

/* stream-install.md §7.3: once a state exists, a `drop` line comes before the file goes. */
static xc_err spool_drop(void *ctx, uint32_t chunk)
{
    job *j = ctx;
    char path[XC_INSTALL_MAX_PATH];
    if (!spool_path(j, chunk, path)) {
        return XC_ERR_LIMIT;
    }
    xc_err err = XC_OK;
    if (j->state_seq > 0) {
        size_t n = 0;
        err = xc_journal_format_drop(chunk, j->line, sizeof j->line, &n);
        if (err == XC_OK) {
            err = j->io->append(j->io->ctx, j->journal_path, j->line, n);
        }
    }
    return err == XC_OK ? j->io->spool_remove(j->io->ctx, path) : err;
}

static xc_err spool_size(void *ctx, uint32_t chunk, uint64_t *bytes)
{
    job *j = ctx;
    char path[XC_INSTALL_MAX_PATH];
    *bytes = 0;
    if (!spool_path(j, chunk, path)) {
        return XC_ERR_LIMIT;
    }
    return j->io->exists(j->io->ctx, path) ? j->io->file_size(j->io->ctx, path, bytes) : XC_OK;
}

static xc_err spool_sync(void *ctx)
{
    job *j = ctx;
    return j->io->spool_sync(j->io->ctx);
}

/*
 * stream-install.md §7.1: what the checkpoints file and the state files will still take. It
 * comes out of the spool's share, so the reserve stays free. A state file is assumed to grow
 * by up to 64 KiB between two states.
 */
static uint64_t state_growth(const job *j)
{
    if (!j->states) {
        return 0;
    }
    uint64_t each = xc_zip_checkpoint_size(j->member.method);
    uint64_t total = j->member.size / zip_checkpoint_every(j);
    uint64_t growth = total > j->cks_on_file ? (total - j->cks_on_file) * each : 0;
    for (size_t k = 0; k < 2; k++) {
        uint64_t last = j->state_len[j->state_seq % 2U];
        uint64_t room = last + 65536U;
        growth += room > j->state_len[k] ? room - j->state_len[k] : 0;
    }
    return growth;
}

static xc_err spool_free(void *ctx, uint64_t *bytes)
{
    job *j = ctx;
    xc_err err = j->io->free_space(j->io->ctx, j->req->games_dir, bytes);
    uint64_t growth = state_growth(j);
    *bytes = *bytes > growth ? *bytes - growth : 0;
    return err;
}

static xc_err seq_listing(void *ctx, const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *l,
                          bool complete)
{
    job *j = ctx;
    (void)vol;
    if (complete) {
        j->res->file_count = l->file_count;
    }
    return check_fatx(j, l); /* names and paths as soon as they're known */
}

static int path_crc_cmp(const void *a, const void *b)
{
    const path_crc *x = a;
    const path_crc *y = b;
    return strcmp(x->path, y->path);
}

static xc_err add_crc(job *j, const char *path, uint32_t crc)
{
    if (j->ncrcs == j->cap_crcs) {
        size_t cap = j->cap_crcs > 0 ? j->cap_crcs * 2 : 256;
        path_crc *v = realloc(j->crcs, cap * sizeof *v);
        if (v == NULL) {
            return XC_ERR_NOMEM;
        }
        j->crcs = v;
        j->cap_crcs = cap;
    }
    size_t len = strlen(path) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(copy, path, len);
    j->crcs[j->ncrcs].path = copy;
    j->crcs[j->ncrcs].crc = crc;
    j->ncrcs++;
    return XC_OK;
}

/* journal.md §6.8 and §6.5: done in an earlier attempt, and still there with its size. */
static bool seq_is_done(void *ctx, const xc_xdvdfs_entry *f)
{
    job *j = ctx;
    const xc_journal_done *d = xc_journal_zip_find(&j->zjournal, f->path);
    uint64_t size = 0;
    if (d == NULL || d->size != f->size || !join(j, j->path, j->res->folder, f->path) ||
        j->io->file_size(j->io->ctx, j->path, &size) != XC_OK || size != f->size) {
        return false;
    }
    return add_crc(j, f->path, d->crc) == XC_OK;
}

static xc_err seq_file_done(void *ctx, const xc_xdvdfs_entry *f, uint32_t crc)
{
    job *j = ctx;
    size_t len = 0;
    xc_err err = xc_journal_format_done(f->path, f->size, crc, j->line, sizeof j->line, &len);
    if (err == XC_OK) {
        err = j->io->append(j->io->ctx, j->journal_path, j->line, len);
    }
    return err == XC_OK ? add_crc(j, f->path, crc) : err;
}

static bool seq_progress(void *ctx, const xc_seq_progress *sp)
{
    job *j = ctx;
    j->p.phase = XC_INSTALL_WRITING;
    j->p.bytes_done = sp->offset;
    j->p.bytes_total = sp->image_size;
    j->p.files_done = sp->files_done;
    j->p.file_count = sp->file_count;
    j->p.current = sp->current;
    return report(j);
}

/*
 * stream-install.md §7.3: the checkpoints an earlier attempt kept. A damaged tail (a power cut
 * during an append) or a checkpoint from another build ends the list, and the file is then
 * rewritten with what's left, so later appends line up again.
 */
static xc_err load_checkpoints(job *j)
{
    const xc_install_io *io = j->io;
    size_t each = xc_zip_checkpoint_size(j->member.method);
    uint64_t every = zip_checkpoint_every(j);
    uint64_t most = j->member.size / every + 1;
    if (each == 0 || most > SIZE_MAX / each || !io->exists(io->ctx, j->ck_path)) {
        return XC_OK;
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = io->read_all(io->ctx, j->ck_path, (size_t)most * each, &data, &len);
    if (err == XC_ERR_NOMEM) {
        return err;
    }
    size_t good = 0;
    for (; err == XC_OK && good + each <= len; good += each) {
        const uint8_t *blob = (const uint8_t *)data + good;
        xc_zip_position pos;
        if (xc_zip_checkpoint_position(blob, each, &j->member, &pos) != XC_OK ||
            pos.offset % every != 0) {
            break;
        }
        size_t at = checkpoint_at(j, pos.offset);
        if (at < j->nzcks && j->zcks[at].pos.offset == pos.offset) {
            break;
        }
        err = keep_checkpoint(j, at, &pos, blob, each);
        j->cks_on_file += err == XC_OK ? 1U : 0U;
    }
    if (err == XC_ERR_NOMEM) {
        free(data);
        return err;
    }
    if (err != XC_OK || good != len) {
        err = good > 0 ? io->write_all(io->ctx, j->ck_path, data, good)
                       : io->remove(io->ctx, j->ck_path);
    }
    free(data);
    return err;
}

/*
 * stream-install.md §7.3: the state the last `state` line names, if it's there as written
 * and a checkpoint exists at its offset; otherwise none, and the stream starts again at 0.
 */
static xc_err load_state(job *j, uint8_t **state, size_t *len)
{
    const xc_journal_zip_state *z = &j->zjournal;
    *state = NULL;
    *len = 0;
    j->state_seq = z->has_state ? z->state_seq : 0;
    size_t at = checkpoint_at(j, z->state_offset);
    char path[XC_INSTALL_MAX_PATH];
    if (!z->has_state || !j->states || at == j->nzcks ||
        j->zcks[at].pos.offset != z->state_offset || !state_path(j, z->state_seq, path)) {
        return XC_OK;
    }
    char *data = NULL;
    size_t n = 0;
    xc_err err = j->io->read_all(j->io->ctx, path, z->state_bytes, &data, &n);
    if (err == XC_ERR_NOMEM) {
        return err;
    }
    uint64_t offset = 0;
    if (err != XC_OK || n != z->state_bytes || xc_crc32(0, data, n) != z->state_crc ||
        xc_seq_state_offset((const uint8_t *)data, n, j->member.size, zip_checkpoint_every(j),
                            &offset) != XC_OK ||
        offset != z->state_offset) {
        free(data);
        return XC_OK;
    }
    /* chunk files dropped after the state was saved may have been made again since */
    for (size_t i = 0; i < z->drop_count; i++) {
        if (spool_path(j, z->drops[i], j->path) && j->io->exists(j->io->ctx, j->path)) {
            err = j->io->spool_remove(j->io->ctx, j->path);
            if (err != XC_OK) {
                free(data);
                return err;
            }
        }
    }
    *state = (uint8_t *)data;
    *len = n;
    return XC_OK;
}

/* §2.4: a finished install leaves no state files behind. */
static void remove_state_files(job *j)
{
    char path[XC_INSTALL_MAX_PATH];
    for (uint64_t k = 0; k < 2; k++) {
        if (state_path(j, k, path) && j->io->exists(j->io->ctx, path)) {
            (void)j->io->remove(j->io->ctx, path);
        }
    }
    if (j->io->exists(j->io->ctx, j->ck_path)) {
        (void)j->io->remove(j->io->ctx, j->ck_path);
    }
}

static xc_err run_zip_install(job *j)
{
    const xc_install_req *req = j->req;
    const xc_install_io *io = j->io;
    xc_install_result *res = j->res;
    if (io->spool_append == NULL || io->spool_read == NULL || io->spool_remove == NULL) {
        return XC_ERR_UNSUPPORTED;
    }
    xc_journal_make_zip_id(&j->id, j->member.size, j->member.crc32, j->member.compressed_size,
                           req->etag, req->last_modified);
    xc_err err = choose_folder(j);
    if (err != XC_OK) {
        return err;
    }
    if (!join(j, j->ck_path, res->folder, XC_INSTALL_ZIP_CHECKPOINTS)) {
        return XC_ERR_LIMIT;
    }
    xc_sink sink;
    void *sink_handle = NULL;
    err = io->open_sink(io->ctx, res->folder, &sink, &sink_handle);
    if (err != XC_OK) {
        return err;
    }
    /* A 7z's decoder state isn't saved (sevenz.md §5): after a power cut its stream starts
       again, and the files already finished are kept. */
    j->states = io->spool_sync != NULL && sink.sync != NULL && j->sevenz == NULL;
    uint8_t *state = NULL;
    size_t state_len = 0;
    if (res->resumed) {
        err = load_checkpoints(j);
        if (err == XC_OK) {
            err = load_state(j, &state, &state_len);
        }
    } else {
        size_t len = 0;
        char *start = malloc((size_t)3 * XC_JOURNAL_MAX_LINE);
        err = start != NULL ? xc_journal_format_start(&j->id, req->source_name, start,
                                                      (size_t)3 * XC_JOURNAL_MAX_LINE, &len)
                            : XC_ERR_NOMEM;
        if (err == XC_OK) {
            err = io->append(io->ctx, j->journal_path, start, len);
        }
        free(start);
    }
    xc_seq_result sr;
    memset(&sr, 0, sizeof sr);
    if (err == XC_OK) {
        xc_seq_spool spool = {spool_append,
                              spool_read,
                              spool_drop,
                              spool_free,
                              j,
                              spool_size,
                              j->states ? spool_sync : NULL};
        xc_seq_req q;
        memset(&q, 0, sizeof q);
        q.image_size = j->member.size;
        q.stream = j->sevenz != NULL ? sevenz_stream : zip_stream;
        q.stream_ctx = j;
        q.sink = &sink;
        q.spool = &spool;
        q.chunk_bytes = zip_checkpoint_every(j);
        q.reserve = req->spool_reserve != 0 ? req->spool_reserve : XC_INSTALL_SPOOL_RESERVE;
        q.cluster_size = req->cluster_size;
        q.buf = req->buf;
        q.buf_size = req->buf_size;
        q.full_first_pass = true; /* the member's CRC-32 covers every byte */
        q.on_listing = seq_listing;
        q.file_done = seq_file_done;
        q.is_done = seq_is_done;
        q.progress = seq_progress;
        q.ctx = j;
        q.listing_out = &j->listing;
        q.resume = state;
        q.resume_len = state_len;
        err = xc_seq_extract(&q, &sr);
        j->p.current = NULL; /* it pointed into the engine's listing, which is gone */
        if (err != XC_OK) {
            (void)snprintf(res->current_file, sizeof res->current_file, "%s", sr.current_file);
        }
        res->zip_resumed_at = sr.resumed_at;
        res->files_continued = sr.file_continued ? 1U : 0U;
        res->bytes_continued = sr.bytes_continued;
        res->passes = sr.passes;
        res->reread_bytes = sr.reread;
        res->spool_peak = sr.spool_peak;
        res->files_from_spool = sr.files_from_spool;
        res->files_written = sr.files_written + sr.files_from_spool;
        res->files_skipped = sr.files_skipped;
        res->bytes_written = sr.bytes_written;
        if (err == XC_ERR_NO_SPACE) {
            res->space_needed = sr.space_needed;
            res->space_free = sr.space_free;
        }
        j->vol = sr.vol;
    }
    free(state);
    io->close_sink(io->ctx, sink_handle);
    if (err != XC_OK) {
        return err;
    }
    remove_state_files(j);
    res->file_count = j->listing.file_count;
    res->hash_note = j->sevenz != NULL
                         ? "7z: every byte checked by the file's CRC-32 instead (sevenz.md §5)"
                         : "zip: every byte checked by the member's CRC-32 instead (zip.md §9)";

    /* §7: read back against the CRC-32s, from this attempt or the journal */
    size_t files = j->listing.file_count > 0 ? j->listing.file_count : 1;
    j->crc = calloc(files, sizeof *j->crc);
    if (j->crc == NULL) {
        return XC_ERR_NOMEM;
    }
    if (j->ncrcs > 0) {
        qsort(j->crcs, j->ncrcs, sizeof *j->crcs, path_crc_cmp);
    }
    for (size_t i = 0; i < j->listing.file_count; i++) {
        path_crc key = {(char *)j->listing.files[i].path, 0};
        const path_crc *hit =
            j->ncrcs > 0 ? bsearch(&key, j->crcs, j->ncrcs, sizeof *j->crcs, path_crc_cmp) : NULL;
        if (hit == NULL) {
            return XC_ERR_VERIFY; /* can't happen: every file was written or kept */
        }
        j->crc[i] = hit->crc;
    }
    err = verify(j);
    if (err == XC_OK) {
        err = write_marker(j);
    }
    if (err == XC_OK) {
        (void)io->remove(io->ctx, j->journal_path); /* journal.md §2.4: finished */
    }
    return err;
}

/* zip.md §2-§6: the member, then its install as a bare image (stored) or a stream. */
static xc_err run_zip(job *j)
{
    j->p.phase = XC_INSTALL_READING_TABLES;
    if (!report(j)) {
        return XC_ERR_CANCELLED;
    }
    xc_zip_listing zl;
    xc_err err = xc_zip_list(&j->src, &zl);
    if (err != XC_OK) {
        return err;
    }
    size_t index = 0;
    err = xc_zip_pick_image(&zl, &index);
    size_t images = 0;
    for (size_t i = 0; i < zl.count; i++) {
        images += xc_zip_is_image_name(zl.entries[i].name) ? 1U : 0U;
    }
    if (err == XC_ERR_ZIP_MEMBER && images == 0) {
        /* No image: a game or app folder, if it has a default.xbe (folder-image.md §7). */
        j->folder_zl = zl;
        err = xc_zip_folder_open(&j->src, &j->folder_zl, &j->zip_folder, &j->image,
                                 j->res->folder_why, sizeof j->res->folder_why);
        if (err != XC_OK) {
            return err;
        }
        const xc_arc_folder *af = xc_zip_folder_arc(j->zip_folder);
        j->res->archive_folder = true;
        (void)snprintf(j->res->zip_member, sizeof j->res->zip_member, "%s/",
                       xc_arc_folder_root(af));
        j->no_hash = true;
        return run_install(j);
    }
    if (err == XC_OK) {
        (void)snprintf(j->res->zip_member, sizeof j->res->zip_member, "%s", zl.entries[index].name);
        j->res->zip_method = zl.entries[index].method;
        err = xc_zip_open_member(&j->src, &zl, index, &j->member);
    }
    xc_zip_listing_free(&zl);
    if (err != XC_OK) {
        return err;
    }
    if (j->member.method == XC_ZIP_STORED) {
        xc_source_window(&j->image, &j->win, &j->src, j->member.data_offset, j->member.size);
        return run_install(j);
    }
    return run_zip_install(j);
}

/* sevenz.md §3: the 7z's one image (stored: read in place; compressed: streamed), or its game
   or app folder (folder-image.md §7). */
static xc_err run_7z(job *j)
{
    j->p.phase = XC_INSTALL_READING_TABLES;
    if (!report(j)) {
        return XC_ERR_CANCELLED;
    }
    xc_install_result *res = j->res;
    xc_err err =
        xc_7z_open(&j->src, j->req->max_dict, &j->sevenz, res->folder_why, sizeof res->folder_why);
    if (err != XC_OK) {
        return err;
    }
    size_t index = 0;
    err = xc_7z_pick_image(j->sevenz, &index);
    size_t images = 0;
    for (size_t i = 0; i < xc_7z_count(j->sevenz); i++) {
        images +=
            !xc_7z_is_dir(j->sevenz, i) && xc_zip_is_image_name(xc_7z_path(j->sevenz, i)) ? 1U : 0U;
    }
    if (err == XC_ERR_ZIP_MEMBER && images == 0) {
        err = xc_7z_folder_open(j->sevenz, &j->seven_folder, &j->image, res->folder_why,
                                sizeof res->folder_why);
        if (err != XC_OK) {
            return err;
        }
        res->archive_folder = true;
        (void)snprintf(res->zip_member, sizeof res->zip_member, "%s/",
                       xc_arc_folder_root(j->seven_folder));
        j->no_hash = true;
        return run_install(j);
    }
    if (err != XC_OK) {
        return err;
    }
    (void)snprintf(res->zip_member, sizeof res->zip_member, "%s", xc_7z_path(j->sevenz, index));
    err = xc_7z_check(j->sevenz, index, res->folder_why, sizeof res->folder_why);
    if (err != XC_OK) {
        return err;
    }
    uint64_t at = 0;
    uint64_t size = xc_7z_size(j->sevenz, index);
    if (xc_7z_stored_at(j->sevenz, index, &at)) {
        xc_source_window(&j->image, &j->win, &j->src, at, size); /* no compression */
        return run_install(j);
    }
    /* Compressed: a stream, installed as a Deflate zip member is (stream-install.md). */
    j->seven_index = index;
    memset(&j->member, 0, sizeof j->member);
    j->member.index = index;
    j->member.method = 0xFFFFU; /* no zip checkpoints (xc_zip_checkpoint_size: 0) */
    j->member.size = size;
    (void)xc_7z_crc(j->sevenz, index, &j->member.crc32);
    j->member.compressed_size = xc_7z_packed_size(j->sevenz, index);
    return run_zip_install(j);
}

xc_err xc_install(const xc_install_req *req, xc_install_result *res)
{
    if (res != NULL) {
        memset(res, 0, sizeof *res);
    }
    if (req == NULL || res == NULL || req->src == NULL || req->io == NULL ||
        req->games_dir == NULL || req->buf == NULL || req->buf_size < MIN_BUF) {
        return XC_ERR_INVALID_ARG;
    }
    if (req->expect_size != 0 && req->src->size != req->expect_size) {
        return XC_ERR_SOURCE_SIZE; /* archiveorg.md §8: before anything is written */
    }
    job *j = calloc(1, sizeof *j);
    if (j == NULL) {
        return XC_ERR_NOMEM;
    }
    j->req = req;
    j->io = req->io;
    j->res = res;
    j->count.inner = req->src;
    j->src.read_at = counted_read_at;
    j->src.read_stream = req->src->read_stream != NULL ? counted_read_stream : NULL;
    j->src.ctx = &j->count;
    j->src.size = req->src->size;
    j->image = j->src;
    xc_err err = req->zip ? run_zip(j) : req->sevenz ? run_7z(j) : run_install(j);
    res->bytes_read = j->count.bytes;
    if (err != XC_OK && j->p.current != NULL) {
        (void)snprintf(res->current_file, sizeof res->current_file, "%s", j->p.current);
    }
    xc_zip_folder_close(j->zip_folder);
    xc_arc_folder_free(j->seven_folder);
    xc_7z_close(j->sevenz);
    xc_zip_listing_free(&j->folder_zl);
    xc_journal_state_free(&j->journal);
    xc_journal_zip_state_free(&j->zjournal);
    for (size_t i = 0; i < j->nzcks; i++) {
        free(j->zcks[i].blob);
    }
    free(j->zcks);
    for (size_t i = 0; i < j->ncrcs; i++) {
        free(j->crcs[i].path);
    }
    free(j->crcs);
    xc_xdvdfs_listing_free(&j->listing);
    free(j->done);
    free(j->crc);
    free(j->resume_bytes);
    free(j->resume_crc);
    xc_image_hash_free(j->hash);
    free(j);
    return err;
}
