/*
 * xcat-cli: run libxcat on a PC.
 *
 *   xcat-cli list <image>               print the layout and every file (offset order)
 *   xcat-cli extract <image> <out-dir>  extract the image into out-dir
 *   xcat-cli check <image> [title]      can it be installed on the Xbox HDD? (ADR 0008)
 *   xcat-cli layout <image>             where directory tables sit vs. file data (ADR 0009)
 *   xcat-cli get [options] <url> <file> download a file over HTTP with Range (docs/formats/http.md)
 *   xcat-cli map [options] <map-file>   check a map file and list its entries (docs/formats/map.md)
 *   xcat-cli install [options] <url|image> <games-dir> [title]
 *                                       install like the console: resumable, verified
 * (xcat/install.h); a name ending in .zip installs the zip's image (docs/formats/zip.md)
 *   xcat-cli ntp [options] [server]     ask a time server like the console (docs/formats/sntp.md)
 *   xcat-cli zip [options] <zip|url>    list a zip and check its image (docs/formats/zip.md)
 *   xcat-cli catalog <file> [query | --title <id>[@variant] | --name <name>]
 *                                       check a catalog, list or look up games (catalog.md)
 *
 * Exit status: 0 success, 1 failure (for map: any error line; for catalog: also a lookup
 * that finds nothing), 2 usage error, 3 not installable (check).
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xcat/catalog.h"
#include "xcat/crc32.h"
#include "xcat/credentials.h"
#include "xcat/extract.h"
#include "xcat/fatx.h"
#include "xcat/host.h"
#include "xcat/http_client.h"
#include "xcat/install.h"
#include "xcat/map.h"
#include "xcat/sevenz.h"
#include "xcat/sntp.h"
#include "xcat/version.h"
#include "xcat/xdvdfs.h"
#include "xcat/zip.h"

#ifdef XCAT_SMB
#include "smb_cmds.h"
#endif

#define IO_BUFFER_SIZE ((size_t)1024 * 1024)

static int usage(void)
{
    (void)fprintf(
        stderr,
        "xcat-cli %s\n"
        "usage: xcat-cli list <image>\n"
        "       xcat-cli extract <image> <out-dir>\n"
        "       xcat-cli check <image> [title]\n"
        "       xcat-cli layout <image>\n"
        "       xcat-cli get [net options] <url> <file>\n"
        "       xcat-cli map [net options] <map-file>\n"
        "       xcat-cli install [net options] [--checkpoint <bytes>] <url|image> <games-dir>\n"
        "                        [title]\n"
        "       xcat-cli ntp [--port <n>] [--timeout <ms>] [--floor <unix-seconds>] [server]\n"
        "       xcat-cli zip [net options] <zip|url>\n"
        "       xcat-cli catalog <catalog.bin> [query | --title <id>[@variant] | --name <name>]\n"
        "net options: --timeout <ms>, --attempts <n>, --credentials <file>,\n"
        "             --ca-file <pem> (trust these CAs instead of the built-in ones),\n"
        "             --now <unix-seconds> (for tests: the time certificates are checked at)\n"
        "install options: --md5 <hex>, --sha1 <hex> (the image's expected hashes),\n"
        "                 --max-dict <MiB> (the largest 7z dictionary to unpack, as on a "
        "console)\n"
        "install: a <url|image> ending in .zip or .7z installs the archive's image, or its game "
        "or app folder\n",
        xc_version_string());
#ifdef XCAT_SMB
    (void)fprintf(stderr,
                  "%s"
                  "map and install: smb:// addresses and @smb folders log in with --credentials\n",
                  xcat_cli_smb_usage());
#endif
    return 2;
}

static int fail(const char *what, xc_err err)
{
    (void)fprintf(stderr, "xcat-cli: %s: %s\n", what, xc_err_str(err));
    return 1;
}

static const char *layout_name(xc_xdvdfs_layout l)
{
    switch (l) {
    case XC_XDVDFS_XISO:
        return "xiso";
    case XC_XDVDFS_XGD1:
        return "xgd1";
    case XC_XDVDFS_XGD2:
        return "xgd2";
    case XC_XDVDFS_XGD3:
        return "xgd3";
    }
    return "?";
}

/* Opens the image and lists it; on success the caller closes `hf` and frees `l`. */
static int load(const char *image, xc_source *src, xc_host_file *hf, xc_xdvdfs_volume *vol,
                xc_xdvdfs_listing *l)
{
    xc_err err = xc_host_file_open(src, hf, image);
    if (err != XC_OK) {
        return fail(image, err);
    }
    err = xc_xdvdfs_open(src, vol);
    if (err == XC_OK) {
        err = xc_xdvdfs_list(src, vol, NULL, l);
    }
    if (err != XC_OK) {
        xc_host_file_close(hf);
        return fail(image, err);
    }
    return 0;
}

static int cmd_list(const char *image)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    (void)printf("layout %s, partition at %" PRIu64 ", %zu files, %zu dirs, %" PRIu64 " bytes\n",
                 layout_name(vol.layout), vol.partition_offset, l.file_count, l.dir_count,
                 l.total_file_bytes);
    for (size_t i = 0; i < l.file_count; i++) {
        (void)printf("%12" PRIu64 " %10" PRIu32 "  %s\n", l.files[i].offset, l.files[i].size,
                     l.files[i].path);
    }
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    return 0;
}

static int cmd_extract(const char *image, const char *out)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    uint8_t *buf = malloc(IO_BUFFER_SIZE);
    xc_sink sink;
    xc_host_dir_sink ds;
    xc_err err = buf != NULL ? xc_host_dir_sink_open(&sink, &ds, out) : XC_ERR_NOMEM;
    xc_extract_stats st;
    memset(&st, 0, sizeof st);
    if (err == XC_OK) {
        xc_extract_opts opts;
        memset(&opts, 0, sizeof opts);
        opts.buf = buf;
        opts.buf_size = IO_BUFFER_SIZE;
        opts.coalesce_gap = XC_EXTRACT_DEFAULT_GAP;
        err = xc_extract(&src, &l, &sink, &opts, &st);
        xc_host_dir_sink_close(&ds);
    }
    free(buf);
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    if (err != XC_OK) {
        return fail(out, err);
    }
    (void)printf("extracted %zu files, %zu dirs, %" PRIu64 " bytes (%" PRIu64
                 " read in %zu spans)\n",
                 st.files_written, st.dirs_created, st.bytes_written, st.bytes_read, st.spans);
    return 0;
}

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[len - n + i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

/* "dir/Halo (USA).xiso.iso" -> "Halo (USA)" (at most `size` - 1 bytes). */
static void title_from_path(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t len = strlen(base);
    if (ends_with_ci(base, len, ".zip")) {
        len -= 4;
    } else if (ends_with_ci(base, len, ".7z")) {
        len -= 3;
    }
    if (ends_with_ci(base, len, ".iso")) {
        len -= 4;
    }
    if (ends_with_ci(base, len, ".xiso")) {
        len -= 5;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

static const char *name_problem(xc_fatx_name_check c)
{
    switch (c) {
    case XC_FATX_NAME_EMPTY:
        return "empty name";
    case XC_FATX_NAME_TOO_LONG:
        return "name longer than 42 characters";
    case XC_FATX_NAME_BAD_BYTE:
        return "character not allowed on FATX";
    case XC_FATX_NAME_DOTS:
        return "name is . or ..";
    case XC_FATX_NAME_OK:
        break;
    }
    return "invalid name";
}

#define MAX_REPORTED 20

static int cmd_check(const char *image, const char *title)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    char from_path[256];
    if (title == NULL) {
        title_from_path(image, from_path, sizeof from_path);
        title = from_path;
    }
    char folder[XC_FATX_MAX_NAME + 1];
    size_t folder_len = xc_fatx_title_name(title, folder, XC_FATX_MAX_NAME);
    size_t install_dir_len = strlen("\\Games\\") + folder_len;
    xc_fatx_problem problems[MAX_REPORTED];
    size_t count = 0;
    xc_err err = xc_fatx_check_listing(&l, install_dir_len, problems, MAX_REPORTED, &count);
    if (err != XC_OK) {
        xc_xdvdfs_listing_free(&l);
        xc_host_file_close(&hf);
        return fail(image, err);
    }
    (void)printf("install folder: E:\\Games\\%s\n", folder);
    for (size_t i = 0; i < count && i < MAX_REPORTED; i++) {
        const xc_fatx_problem *p = &problems[i];
        switch (p->kind) {
        case XC_FATX_PROBLEM_NAME:
            (void)printf("  %s: %s\n", p->path, name_problem(p->name));
            break;
        case XC_FATX_PROBLEM_PATH_TOO_LONG:
            (void)printf("  %s: path would be %zu characters (limit %d)\n", p->path, p->value,
                         XC_FATX_MAX_PATH);
            break;
        case XC_FATX_PROBLEM_TOO_MANY_ENTRIES:
            (void)printf("  %s/: %zu entries in one folder (limit %d)\n",
                         p->path[0] != '\0' ? p->path : ".", p->value, XC_FATX_MAX_DIR_ENTRIES);
            break;
        case XC_FATX_PROBLEM_FILE_TOO_BIG:
            (void)printf("  %s: %zu bytes; FATX stores at most %u per file\n", p->path, p->value,
                         XC_FATX_MAX_FILE_SIZE);
            break;
        }
    }
    if (count > MAX_REPORTED) {
        (void)printf("  ... and %zu more\n", count - MAX_REPORTED);
    }
    size_t files = l.file_count;
    uint64_t bytes = l.total_file_bytes;
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    if (count > 0) {
        (void)printf("not installable: %s (%zu problem%s)\n", xc_err_str(XC_ERR_NOT_INSTALLABLE),
                     count, count == 1 ? "" : "s");
        return 3;
    }
    (void)printf("installable: %zu files, %" PRIu64 " bytes\n", files, bytes);
    return 0;
}

/* Bytes as MiB with one decimal, for reading. */
static double mib(uint64_t bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

/* The spool plan (xdvdfs.md §8.1) for a few spool sizes, to size ADR 0009's hybrid. */
static int print_spool_plan(const char *image, const xc_xdvdfs_volume *vol,
                            const xc_xdvdfs_listing *l)
{
    static const uint64_t budgets_mib[] = {0, 256, 512, 1024, 2048};
    xc_xdvdfs_spool s;
    xc_err err = xc_xdvdfs_spool_plan(vol, l, 0, (uint64_t)XC_INSTALL_CHECKPOINT, &s);
    if (err != XC_OK) {
        return fail(image, err);
    }
    (void)printf("spool: %.1f MiB unidentified before every table is read (at %.1f MiB); "
                 "%.1f MiB of it is late file data\n",
                 mib(s.unidentified), mib(s.all_known), mib(s.late_bytes));
    (void)printf("  spool up to   pass 2 re-reads (decompressed, from a %u MiB checkpoint)\n",
                 (unsigned)(XC_INSTALL_CHECKPOINT / (1024U * 1024U)));
    for (size_t i = 0; i < sizeof budgets_mib / sizeof budgets_mib[0]; i++) {
        uint64_t budget = budgets_mib[i] * 1024U * 1024U;
        bool all = budget >= s.unidentified;
        if (all) {
            budget = s.unidentified;
        }
        err = xc_xdvdfs_spool_plan(vol, l, budget, (uint64_t)XC_INSTALL_CHECKPOINT, &s);
        if (err != XC_OK) {
            return fail(image, err);
        }
        if (all) {
            (void)printf("  %8.1f MiB  nothing (spools everything)\n", mib(budget));
            break;
        }
        if (s.reread_start == UINT64_MAX) {
            (void)printf("  %8.1f MiB  nothing\n", mib(budget));
        } else {
            (void)printf("  %8.1f MiB  %.1f MiB: %.1f to %.1f MiB\n", mib(budget),
                         mib(s.reread_end - s.reread_from), mib(s.reread_from), mib(s.reread_end));
        }
    }
    return 0;
}

static int cmd_layout(const char *image)
{
    xc_source src;
    xc_host_file hf = {NULL};
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    int rc = load(image, &src, &hf, &vol, &l);
    if (rc != 0) {
        return rc;
    }
    xc_xdvdfs_order o;
    xc_err err = xc_xdvdfs_stream_order(&vol, &l, &o);
    if (err != XC_OK) {
        xc_xdvdfs_listing_free(&l);
        xc_host_file_close(&hf);
        return fail(image, err);
    }
    (void)printf("layout %s, partition at %" PRIu64 " (%.1f MiB), image %" PRIu64 " bytes\n",
                 layout_name(vol.layout), vol.partition_offset, mib(vol.partition_offset),
                 src.size);
    if (o.tables > 0) {
        (void)printf("directory tables: %zu (incl. root), from %" PRIu64 " to %" PRIu64
                     " (%.1f to %.1f MiB)\n",
                     o.tables, o.tables_start, o.tables_end, mib(o.tables_start),
                     mib(o.tables_end));
    }
    if (o.data_start != UINT64_MAX) {
        (void)printf("file data: %zu files, %.1f MiB, from %" PRIu64 " to %" PRIu64
                     " (%.1f to %.1f MiB)\n",
                     l.file_count, mib(l.total_file_bytes), o.data_start, o.data_end,
                     mib(o.data_start), mib(o.data_end));
    }
    if (o.late_files == 0) {
        (void)printf("order: every directory table comes before the files it describes;\n"
                     "       one sequential pass can install it (e.g. from a Deflate zip)\n");
    } else {
        (void)printf("order: %zu files (%.1f MiB) start before a directory table on their path "
                     "has been read\n",
                     o.late_files, mib(o.late_bytes));
        (void)printf("       earliest at %" PRIu64 " (%.1f MiB); the last table they wait for "
                     "ends at %" PRIu64 " (%.1f MiB)\n",
                     o.first_late, mib(o.first_late), o.last_table_end_needed,
                     mib(o.last_table_end_needed));
        (void)printf("       one sequential pass is not enough: a stream would have to be re-read "
                     "from %.1f MiB\n",
                     mib(o.first_late));
    }
    rc = print_spool_plan(image, &vol, &l);
    xc_xdvdfs_listing_free(&l);
    xc_host_file_close(&hf);
    return rc;
}

/* --- get ------------------------------------------------------------------------------- */

typedef struct get_ctx {
    FILE *f;
    const xc_clock *clock;
    uint64_t size;
    uint64_t done;
    uint32_t crc;
    uint64_t start_ms;
    uint64_t next_report_ms;
} get_ctx;

/* Decimal megabytes per second, the unit docs/testing.md records. */
static double mb_per_s(uint64_t bytes, uint64_t ms)
{
    return ms > 0 ? (double)bytes / 1000.0 / (double)ms : 0.0;
}

static xc_err get_write(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    get_ctx *g = ctx;
    (void)offset; /* always g->done: the client delivers bytes in order, each once */
    if (fwrite(data, 1, len, g->f) != len) {
        return XC_ERR_IO;
    }
    g->crc = xc_crc32(g->crc, data, len);
    g->done += len;
    uint64_t now = g->clock->now_ms(g->clock->ctx);
    if (now >= g->next_report_ms) {
        g->next_report_ms = now + 2000U;
        (void)fprintf(stderr, "  %.1f of %.1f MiB, %.1f MB/s\n", mib(g->done), mib(g->size),
                      mb_per_s(g->done, now - g->start_ms));
    }
    return XC_OK;
}

static bool get_retry(void *ctx, const xc_http_retry *r)
{
    (void)ctx;
    if (r->status != 0) {
        (void)fprintf(stderr, "  HTTP %d, ", r->status);
    } else {
        (void)fprintf(stderr, "  %s, ", xc_err_str(r->err));
    }
    (void)fprintf(stderr, "retrying in %.1f s (failure %u of %u)\n", (double)r->delay_ms / 1000.0,
                  r->failures, r->max_attempts);
    return true;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    if (s[0] < '0' || s[0] > '9') {
        return false;
    }
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/*
 * Loads a credentials file (docs/formats/credentials.md). Per §7 a bad or
 * unreadable file is reported and the download goes on without credentials;
 * returns true only if `c` holds credentials to use.
 */
static bool load_credentials(const char *path, xc_credentials *c)
{
    memset(c, 0, sizeof *c);
    FILE *f = xc_host_fopen(path, "rb");
    if (f == NULL) {
        (void)fprintf(stderr, "xcat-cli: %s: can't read it; continuing without credentials\n",
                      path);
        return false;
    }
    char *data = malloc(XC_CRED_MAX_FILE + 1);
    size_t len = data != NULL ? fread(data, 1, XC_CRED_MAX_FILE + 1, f) : 0;
    bool read_ok = data != NULL && ferror(f) == 0;
    (void)fclose(f);
    xc_cred_error cerr;
    xc_err err = read_ok ? xc_cred_parse(data, len, c, &cerr) : XC_ERR_IO;
    free(data);
    if (err == XC_ERR_CREDENTIALS) {
        (void)fprintf(stderr, "xcat-cli: %s line %zu: %s; continuing without credentials\n", path,
                      cerr.line, cerr.message);
    } else if (err != XC_OK) {
        (void)fprintf(stderr, "xcat-cli: %s: %s; continuing without credentials\n", path,
                      xc_err_str(err));
    }
    return err == XC_OK;
}

static int get_fail(const xc_http_client *c, const xc_tls *tls, const char *url, xc_err err)
{
    if (err >= XC_ERR_TLS && err <= XC_ERR_TLS_NOT_TLS && xc_tls_last_error(tls) != 0) {
        (void)fprintf(stderr, "xcat-cli: %s: %s (TLS error %d)\n", url, xc_err_str(err),
                      xc_tls_last_error(tls));
        return 1;
    }
    if (err == XC_ERR_HTTP_DENIED && c->last_section != NULL) {
        /* credentials.md §7: name the section, never its values. */
        (void)fprintf(stderr, "xcat-cli: %s: login missing or expired for [%s] (HTTP %d)\n", url,
                      c->last_section, c->last_status);
        return 1;
    }
    if (err == XC_ERR_HTTP_STATUS || err == XC_ERR_HTTP_DENIED || err == XC_ERR_HTTP_NOT_FOUND) {
        (void)fprintf(stderr, "xcat-cli: %s: %s (HTTP %d)\n", url, xc_err_str(err), c->last_status);
        return 1;
    }
    return fail(url, err);
}

/* Options shared by the commands that use the network. */
typedef struct net_opts {
    unsigned long timeout_ms;
    unsigned long attempts;
    unsigned long checkpoint; /* install only: bytes between checkpoints (ADR 0011), 0 = default */
    unsigned long
        max_dict_mib; /* install only: the largest 7z dictionary (sevenz.md §4), 0 = any */
    const char *cred_path;
    const char *ca_path; /* replaces the built-in CA bundle (ADR 0012 §4) */
    bool has_now;        /* tests only: check certificates at this time instead of the clock's */
    unsigned long now;
    /* install only: the image's expected hashes (journal.md §7) */
    bool has_md5;
    uint8_t md5[XC_MD5_BYTES];
    bool has_sha1;
    uint8_t sha1[XC_SHA1_BYTES];
} net_opts;

/* Parses leading "--name value" options; returns the index of the first other argument or -1. */
static int parse_net_opts(int argc, char **argv, net_opts *o)
{
    memset(o, 0, sizeof *o);
    int i = 0;
    for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        bool ok = false;
        if (strcmp(argv[i], "--credentials") == 0 && o->cred_path == NULL) {
            o->cred_path = argv[i + 1];
            ok = true;
        } else if (strcmp(argv[i], "--timeout") == 0) {
            ok = parse_uint(argv[i + 1], 3600000UL, &o->timeout_ms) && o->timeout_ms > 0;
        } else if (strcmp(argv[i], "--attempts") == 0) {
            ok = parse_uint(argv[i + 1], 1000UL, &o->attempts) && o->attempts > 0;
        } else if (strcmp(argv[i], "--ca-file") == 0 && o->ca_path == NULL) {
            o->ca_path = argv[i + 1];
            ok = true;
        } else if (strcmp(argv[i], "--now") == 0 && !o->has_now) {
            ok = parse_uint(argv[i + 1], 0xFFFFFFFFUL, &o->now);
            o->has_now = true;
        } else if (strcmp(argv[i], "--max-dict") == 0) {
            ok = parse_uint(argv[i + 1], 4096UL, &o->max_dict_mib) && o->max_dict_mib > 0;
        } else if (strcmp(argv[i], "--checkpoint") == 0) {
            ok = parse_uint(argv[i + 1], 0x7FFFFFFFUL, &o->checkpoint) && o->checkpoint >= 4096U;
        } else if (strcmp(argv[i], "--md5") == 0 && !o->has_md5) {
            ok = o->has_md5 = xc_hex_parse(argv[i + 1], o->md5, XC_MD5_BYTES);
        } else if (strcmp(argv[i], "--sha1") == 0 && !o->has_sha1) {
            ok = o->has_sha1 = xc_hex_parse(argv[i + 1], o->sha1, XC_SHA1_BYTES);
        }
        if (!ok) {
            return -1;
        }
    }
    return i;
}

/* Sockets, clock, TLS, credentials and an HTTP client, set up together. */
typedef struct http_env {
    xc_net net;
    xc_clock clock;
    uint8_t *buf;
    xc_credentials creds;
    xc_http_auth auth;
    xc_http_client client;
    bool net_started;
    /* https (xcat/tls.h) */
    xc_tls_anchors *anchors;
    xc_tls *tls;
    xc_net tls_net;
    xc_entropy entropy;
    xc_wallclock wallclock;
    int64_t fixed_now; /* --now */
} http_env;

static xc_err fixed_now(void *ctx, int64_t *unix_s)
{
    *unix_s = *(const int64_t *)ctx;
    return XC_OK;
}

/* The CA bundle: --ca-file replaces the built-in one, as E:\Apps\xcat\cacert.pem does on the
 * console (ADR 0012 §4). A file that can't be used is an error, never a silent fallback. */
static xc_err read_local(const char *path, size_t max_len, char **data, size_t *len);

static xc_err load_anchors(const char *path, xc_tls_anchors **out)
{
    if (path == NULL) {
        return xc_tls_anchors_builtin(out);
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(path, XC_TLS_MAX_ANCHOR_FILE, &data, &len);
    size_t bad = 0;
    if (err == XC_OK) {
        err = xc_tls_anchors_parse(data, len, out, &bad);
        free(data);
    }
    if (err != XC_OK) {
        const char *why = err == XC_ERR_IO      ? "can't read it"
                          : err == XC_ERR_LIMIT ? "too large, or too many certificates"
                          : err == XC_ERR_NOMEM ? xc_err_str(err)
                                                : "not a valid PEM certificate bundle";
        if (bad > 0) {
            (void)fprintf(stderr, "xcat-cli: %s: certificate %zu: %s\n", path, bad, why);
        } else {
            (void)fprintf(stderr, "xcat-cli: %s: %s\n", path, why);
        }
    }
    return err;
}

static void env_close(http_env *e)
{
    xc_http_client_close(&e->client);
    xc_tls_destroy(e->tls);
    e->tls = NULL;
    xc_tls_anchors_free(e->anchors);
    e->anchors = NULL;
    xc_cred_free(&e->creds);
    free(e->buf);
    e->buf = NULL;
    if (e->net_started) {
        xc_host_net_cleanup();
        e->net_started = false;
    }
}

static xc_err env_open(http_env *e, const net_opts *o)
{
    memset(e, 0, sizeof *e);
    xc_err err = xc_host_net_startup();
    if (err != XC_OK) {
        return err;
    }
    e->net_started = true;
    xc_host_net(&e->net);
    xc_host_clock(&e->clock);
    e->buf = malloc(IO_BUFFER_SIZE);
    if (e->buf == NULL) {
        env_close(e);
        return XC_ERR_NOMEM;
    }
    err = load_anchors(o->ca_path, &e->anchors);
    if (err != XC_OK) {
        env_close(e);
        return XC_ERR_TLS; /* the reason is printed above */
    }
    xc_host_entropy(&e->entropy);
    xc_host_wallclock(&e->wallclock);
    if (o->has_now) {
        e->fixed_now = (int64_t)o->now;
        e->wallclock.now = fixed_now;
        e->wallclock.ctx = &e->fixed_now;
    }
    xc_tls_config tcfg = {&e->net, e->anchors, &e->entropy, &e->wallclock};
    err = xc_tls_create(&tcfg, &e->tls, &e->tls_net);
    if (err != XC_OK) {
        env_close(e);
        return err;
    }
    xc_http_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.net = &e->net;
    cfg.tls = &e->tls_net;
    cfg.clock = &e->clock;
    cfg.buf = e->buf;
    cfg.buf_size = IO_BUFFER_SIZE;
    cfg.timeout_ms = (uint32_t)o->timeout_ms;
    cfg.max_attempts = (unsigned)o->attempts;
    cfg.seed = (uint32_t)e->clock.now_ms(e->clock.ctx);
    cfg.on_retry = get_retry;
    if (o->cred_path != NULL && load_credentials(o->cred_path, &e->creds)) {
        xc_cred_http_auth(&e->auth, &e->creds);
        cfg.auth = &e->auth;
    }
    err = xc_http_client_init(&e->client, &cfg);
    if (err != XC_OK) {
        env_close(e);
    }
    return err;
}

static int cmd_get(int argc, char **argv)
{
    net_opts o;
    int i = parse_net_opts(argc, argv, &o);
    if (i < 0 || argc - i != 2) {
        return usage();
    }
    const char *url = argv[i];
    const char *out = argv[i + 1];

    xc_http_resource res;
    xc_err err = xc_http_resource_init(&res, url, strlen(url));
    if (err != XC_OK) {
        return fail(url, err);
    }
    http_env env;
    err = env_open(&env, &o);
    if (err != XC_OK) {
        return fail("network", err);
    }
    get_ctx g;
    memset(&g, 0, sizeof g);
    g.clock = &env.clock;
    g.start_ms = env.clock.now_ms(env.clock.ctx);
    g.next_report_ms = g.start_ms + 2000U;
    err = xc_http_probe(&env.client, &res);
    if (err == XC_OK) {
        g.size = res.size;
        g.f = xc_host_fopen(out, "wb");
        err = g.f != NULL ? XC_OK : XC_ERR_IO;
    }
    if (err == XC_OK) {
        err = xc_http_read(&env.client, &res, 0, res.size, get_write, &g);
        if (fclose(g.f) != 0 && err == XC_OK) {
            err = XC_ERR_IO;
        }
        if (err != XC_OK) {
            (void)xc_host_remove(out); /* no partial files left behind */
        }
    }
    uint64_t ms = env.clock.now_ms(env.clock.ctx) - g.start_ms;
    int rc = err == XC_OK ? 0 : get_fail(&env.client, env.tls, url, err);
    xc_tls_info tls;
    xc_tls_last_info(env.tls, &tls);
    env_close(&env);
    if (rc == 0) {
        (void)printf("downloaded %" PRIu64 " bytes in %.1f s (%.1f MB/s), crc32 %08" PRIx32 "\n",
                     g.done, (double)ms / 1000.0, mb_per_s(g.done, ms), g.crc);
        if (tls.suite_name != NULL) {
            /* ADR 0005: record what servers negotiate, to notice a change early. */
            (void)printf("tls: TLS %u.%u, %s\n", (tls.version >> 8) - 2U,
                         (tls.version & 0xFFU) - 1U, tls.suite_name);
        }
    }
    return rc;
}

/* --- map ------------------------------------------------------------------------------- */

/* Reads at most max_len + 1 bytes of a local file (one more than allowed, to detect "too big"). */
static xc_err read_local(const char *path, size_t max_len, char **data, size_t *len)
{
    FILE *f = xc_host_fopen(path, "rb");
    if (f == NULL) {
        return XC_ERR_IO;
    }
    char *buf = malloc(max_len + 1);
    size_t n = buf != NULL ? fread(buf, 1, max_len + 1, f) : 0;
    bool ok = buf != NULL && ferror(f) == 0;
    (void)fclose(f);
    if (!ok) {
        free(buf);
        return buf == NULL ? XC_ERR_NOMEM : XC_ERR_IO;
    }
    if (n > max_len) {
        free(buf);
        return XC_ERR_LIMIT;
    }
    *data = buf;
    *len = n;
    return XC_OK;
}

typedef struct mem_ctx {
    char *buf;
    size_t len;
    size_t cap;
} mem_ctx;

/* xc_http_data_fn into a growing buffer, kept one byte larger than the body. A body that
 * starts over at 0 (xc_http_get after a retry) replaces what came before. */
static xc_err to_mem(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    mem_ctx *m = ctx;
    m->len = (size_t)offset;
    if (m->cap - m->len <= len) {
        size_t cap = m->cap == 0 ? (size_t)64 * 1024 : m->cap;
        while (cap - m->len <= len) {
            cap *= 2;
        }
        char *grown = realloc(m->buf, cap);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        m->buf = grown;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return XC_OK;
}

typedef struct map_fetch_ctx {
    const net_opts *opts;
    http_env env;
    bool env_ready;
    const xc_credentials *smb_creds; /* @smb logins (smb.md §4), or NULL */
} map_fetch_ctx;

#ifdef XCAT_SMB
/* xc_map_smb_list_fn: the loader's ctx is the map_fetch_ctx. */
static xc_err map_list_smb(void *ctx, const xc_smb_url *folder, xc_smb_listing *out)
{
    const map_fetch_ctx *fc = ctx;
    return xcat_cli_smb_list((void *)(uintptr_t)fc->smb_creds, folder, out);
}
#endif

/* xc_map_fetch_fn: local files directly, http(s) includes with the HTTP client. */
static xc_err map_fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **data,
                        size_t *len)
{
    map_fetch_ctx *fc = ctx;
    if (!is_url) {
        return read_local(location, max_len, data, len);
    }
    if (!fc->env_ready) {
        xc_err err = env_open(&fc->env, fc->opts);
        if (err != XC_OK) {
            return err;
        }
        fc->env_ready = true;
    }
    static xc_http_resource res; /* large; one fetch at a time */
    xc_err err = xc_http_resource_init(&res, location, strlen(location));
    mem_ctx m = {NULL, 0, 0};
    if (err == XC_OK) {
        err = xc_http_get(&fc->env.client, &res, max_len, to_mem, &m);
    }
    if (err == XC_OK && m.buf == NULL) {
        m.buf = malloc(1); /* an empty body */
        err = m.buf != NULL ? XC_OK : XC_ERR_NOMEM;
    }
    if (err != XC_OK) {
        free(m.buf);
        return err;
    }
    *data = m.buf;
    *len = m.len;
    return XC_OK;
}

static int cmd_map(int argc, char **argv)
{
    net_opts o;
    int i = parse_net_opts(argc, argv, &o);
    if (i < 0 || argc - i != 1) {
        return usage();
    }
    const char *path = argv[i];
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(path, XC_MAP_MAX_FILE, &data, &len);
    if (err == XC_ERR_LIMIT) {
        (void)fprintf(stderr, "%s:0: error: file larger than 1 MiB\n", path); /* map.md §6 */
        return 1;
    }
    if (err != XC_OK) {
        return fail(path, err);
    }
    map_fetch_ctx fc;
    memset(&fc, 0, sizeof fc);
    fc.opts = &o;
    xc_map_loader loader = {map_fetch, &fc, NULL};
#ifdef XCAT_SMB
    /* @smb folders are listed with the --credentials logins (smb.md §4). */
    static xc_credentials smb_creds;
    bool have_smb_creds = o.cred_path != NULL && load_credentials(o.cred_path, &smb_creds);
    loader.list_smb = map_list_smb;
    fc.smb_creds = have_smb_creds ? &smb_creds : NULL;
#endif
    xc_map map;
    err = xc_map_load(data != NULL ? data : "", data != NULL ? len : 0, path, &loader, &map);
    free(data);
#ifdef XCAT_SMB
    if (have_smb_creds) {
        xc_cred_free(&smb_creds);
    }
#endif
    if (fc.env_ready) {
        env_close(&fc.env);
    }
    if (err == XC_ERR_NOMEM) {
        xc_map_free(&map);
        return fail(path, err);
    }
    for (size_t k = 0; k < map.entry_count; k++) {
        const xc_map_entry *e = &map.entries[k];
        if (e->name != NULL) {
            /* Generated by @archiveorg or @smb: keyed by name, with the listing's size (and
               archive.org's hashes). */
            (void)printf("\"%s\" %s  [%" PRIu64 " bytes%s%s%s]  (%s:%zu)\n", e->name, e->url,
                         e->size, e->has_md5 ? ", md5" : "", e->has_sha1 ? ", sha1" : "",
                         e->has_crc32 ? ", crc32" : "", map.sources[e->source], e->line);
            continue;
        }
        (void)printf("%08" PRIX32 "%s%-17s %s  (%s:%zu)\n", e->title_id,
                     e->variant[0] != '\0' ? "@" : " ", e->variant, e->url, map.sources[e->source],
                     e->line);
    }
    for (size_t k = 0; k < map.problem_count; k++) {
        const xc_map_problem *p = &map.problems[k];
        (void)fprintf(stderr, "%s:%zu: %s: %s\n", map.sources[p->source], p->line,
                      p->severity == XC_MAP_ERROR ? "error" : "warning", p->message);
    }
    if (map.problems_dropped > 0) {
        (void)fprintf(stderr, "... and %zu more problems\n", map.problems_dropped);
    }
    (void)printf("%zu entries from %zu file%s, %zu error%s\n", map.entry_count, map.source_count,
                 map.source_count == 1 ? "" : "s", map.error_count,
                 map.error_count == 1 ? "" : "s");
    int rc = map.error_count == 0 ? 0 : 1;
    xc_map_free(&map);
    return rc;
}

/* --- install --------------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* A title from a URL's file name: "http://h/x/Game%20(E).xiso.iso?a" -> "Game (E)". */
static void title_from_url(const char *url, char *out, size_t size)
{
    char name[512] = "";
    const char *end = url + strcspn(url, "?#");
    while (end > url && end[-1] == '/') {
        end--; /* a game folder's address (smb.md §3.1): named after the folder */
    }
    const char *p = end;
    while (p > url && p[-1] != '/') {
        p--;
    }
    char *o = name;
    while (p < end && o < name + sizeof name - 1) {
        int hi = p[0] == '%' && end - p >= 3 ? hexval(p[1]) : -1;
        int lo = hi >= 0 ? hexval(p[2]) : -1;
        if (lo >= 0) {
            *o++ = (char)(hi * 16 + lo);
            p += 3;
        } else {
            /* p < end, and end = url + strcspn(url, ...) never passes the terminator; the
               analyzer only sees that the offset came from argv. */
            *o++ = *p++; /* NOLINT(clang-analyzer-security.ArrayBound) */
        }
    }
    *o = '\0';
    title_from_path(name, out, size);
}

typedef struct install_ui {
    const xc_clock *clock;
    uint64_t start_ms;
    uint64_t next_ms;
    int phase; /* the last xc_install_phase shown, -1 before the first */
} install_ui;

static bool install_progress(void *ctx, const xc_install_progress *p)
{
    install_ui *ui = ctx;
    uint64_t now = ui->clock->now_ms(ui->clock->ctx);
    if ((int)p->phase != ui->phase) {
        ui->phase = (int)p->phase;
        ui->next_ms = 0;
        (void)fprintf(stderr, "%s\n",
                      p->phase == XC_INSTALL_WRITING     ? "  writing files"
                      : p->phase == XC_INSTALL_VERIFYING ? "  verifying (read-back)"
                                                         : "  reading tables");
    }
    if (now >= ui->next_ms && p->bytes_total > 0) {
        ui->next_ms = now + 2000U;
        /* the file being worked on (1-based), as on the console */
        size_t current = p->files_done < p->file_count ? p->files_done + 1 : p->file_count;
        (void)fprintf(stderr, "  %.1f of %.1f MiB, file %zu of %zu\n", mib(p->bytes_done),
                      mib(p->bytes_total), current, p->file_count);
    }
    return true;
}

static int install_fail(const xc_install_result *r, const http_env *env, const char *what,
                        xc_err err)
{
    if (r->folder_why[0] != '\0') {
        (void)fprintf(stderr, "xcat-cli: %s: %s\n", what, r->folder_why);
    }
    switch (err) {
    case XC_ERR_NOT_INSTALLABLE:
        (void)printf("not installable: %s (%zu problem%s)\n", xc_err_str(err), r->problem_count,
                     r->problem_count == 1 ? "" : "s");
        for (size_t i = 0; i < r->problem_count && i < XC_INSTALL_MAX_PROBLEMS; i++) {
            const xc_fatx_problem *p = &r->problems[i];
            (void)printf("  %s: %s\n", p->path,
                         p->kind == XC_FATX_PROBLEM_NAME            ? name_problem(p->name)
                         : p->kind == XC_FATX_PROBLEM_PATH_TOO_LONG ? "path too long"
                         : p->kind == XC_FATX_PROBLEM_FILE_TOO_BIG  ? "file too big for FATX"
                                                                    : "too many entries");
        }
        return 3;
    case XC_ERR_NO_SPACE:
        (void)fprintf(stderr, "xcat-cli: %s: needs %.1f MiB, %.1f MiB free\n", r->folder,
                      mib(r->space_needed), mib(r->space_free));
        return 1;
    case XC_ERR_ZIP_UNSUPPORTED:
        if (r->zip_member[0] != '\0' && r->folder_why[0] == '\0') {
            const char *m = xc_zip_method_name(r->zip_method);
            (void)fprintf(stderr, "xcat-cli: %s: %s in the zip is %s (method %u)\n", what,
                          r->zip_member, m != NULL ? m : "compressed with an unknown method",
                          (unsigned)r->zip_method);
        }
        break;
    case XC_ERR_VERIFY:
        (void)fprintf(stderr,
                      "xcat-cli: %s: %s; it was removed, so running the install again "
                      "rewrites it\n",
                      r->bad_file, xc_err_str(err));
        return 1;
    default:
        break;
    }
    if (env != NULL) {
        return get_fail(&env->client, env->tls, what, err);
    }
    return fail(what, err);
}

static bool is_url(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0 ||
           strncmp(s, "HTTP://", 7) == 0 || strncmp(s, "HTTPS://", 8) == 0;
}

/* journal.md §7: the whole-image hashes, or why there are none. */
static void print_image_hash(const xc_install_result *r)
{
    if (r->hash == XC_INSTALL_HASH_NONE) {
        (void)printf("image not hashed: %s\n", r->hash_note != NULL ? r->hash_note : "?");
        return;
    }
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    xc_hex_lower(r->md5, XC_MD5_BYTES, md5);
    xc_hex_lower(r->sha1, XC_SHA1_BYTES, sha1);
    (void)printf("image md5 %s sha1 %s (%s%s)\n", md5, sha1,
                 r->hash == XC_INSTALL_HASH_MATCHED ? "matches what was expected" : "computed",
                 r->hash_restarted ? "; hashed from the start again" : "");
}

static int cmd_install(int argc, char **argv)
{
    net_opts o;
    int i = parse_net_opts(argc, argv, &o);
    if (i < 0 || (argc - i != 2 && argc - i != 3)) {
        return usage();
    }
    const char *image = argv[i];
    const char *games = argv[i + 1];
    char title[256];
    if (argc - i == 3) {
        (void)snprintf(title, sizeof title, "%s", argv[i + 2]);
    } else if (is_url(image) || xc_smb_is_url(image)) {
        title_from_url(image, title, sizeof title);
    } else {
        title_from_path(image, title, sizeof title);
    }

    static http_env env; /* large; one install per run */
    static xc_http_resource res;
    static xc_http_source hs;
    xc_host_file hf = {NULL};
    xc_source src;
    xc_clock clock;
    xc_host_clock(&clock);
    bool remote = is_url(image);
    xc_err err = XC_OK;
#ifdef XCAT_SMB
    static xc_credentials smb_creds;
    bool have_smb_creds = false;
    void *smb = NULL;
#endif
    if (xc_smb_is_url(image)) {
#ifdef XCAT_SMB
        have_smb_creds = o.cred_path != NULL && load_credentials(o.cred_path, &smb_creds);
        err = xcat_cli_smb_open(image, have_smb_creds ? &smb_creds : NULL, &src, &smb);
        if (err != XC_OK) {
            if (have_smb_creds) {
                xc_cred_free(&smb_creds);
            }
            return fail(image, err);
        }
#else
        return fail(image, XC_ERR_UNSUPPORTED);
#endif
    } else if (remote) {
        err = xc_http_resource_init(&res, image, strlen(image));
        if (err != XC_OK) {
            return fail(image, err);
        }
        err = env_open(&env, &o);
        if (err != XC_OK) {
            return fail("network", err);
        }
        err = xc_http_source_open(&src, &hs, &env.client, &res);
        if (err != XC_OK) {
            int rc = get_fail(&env.client, env.tls, image, err);
            env_close(&env);
            return rc;
        }
    } else {
        err = xc_host_file_open(&src, &hf, image);
        if (err != XC_OK) {
            return fail(image, err);
        }
    }

    xc_install_io io;
    xc_host_install_io(&io);
    uint8_t *buf = malloc(IO_BUFFER_SIZE);
    install_ui ui = {&clock, clock.now_ms(clock.ctx), 0, -1};
    xc_install_req req;
    memset(&req, 0, sizeof req);
    req.src = &src;
    req.etag = remote && res.etag[0] != '\0' ? res.etag : NULL;
    req.last_modified = remote && res.last_modified[0] != '\0' ? res.last_modified : NULL;
    req.source_name = image;
    req.games_dir = games;
    req.title = title;
    req.io = &io;
    req.buf = buf;
    req.buf_size = IO_BUFFER_SIZE;
    req.cluster_size = 16384U; /* a retail E: partition, like the console's estimate */
    req.checkpoint_bytes = (uint32_t)o.checkpoint;
    req.progress = install_progress;
    req.progress_ctx = &ui;
    req.zip = xc_zip_is_zip_name(image);
    req.sevenz = xc_7z_is_7z_name(image);
    req.max_dict = (size_t)o.max_dict_mib << 20U; /* 0: no bound */
    req.expect_md5 = o.has_md5 ? o.md5 : NULL;
    req.expect_sha1 = o.has_sha1 ? o.sha1 : NULL;
#ifdef XCAT_SMB
    if (xcat_cli_smb_validator(smb) != NULL) {
        /* A game folder: no image to hash (folder-image.md §5), its listing is the ETag. */
        req.etag = xcat_cli_smb_validator(smb);
        req.no_hash = true;
    }
#endif
    static xc_install_result r;
    err = buf != NULL ? xc_install(&req, &r) : XC_ERR_NOMEM;
    uint64_t ms = clock.now_ms(clock.ctx) - ui.start_ms;
    int rc = 0;
    if (r.other_folder_reason != NULL) {
        (void)fprintf(stderr, "  not using the existing folder for \"%s\": %s\n", title,
                      r.other_folder_reason);
    }
    if (err == XC_OK) {
        (void)printf("installed %zu files into %s (%zu written, %zu kept from an earlier attempt%s)"
                     "\n%" PRIu64 " bytes written, %" PRIu64 " read in %.1f s (%.1f MB/s), "
                     "verified\n",
                     r.file_count, r.folder, r.files_written, r.files_skipped,
                     r.resumed ? ", resumed" : "", r.bytes_written, r.bytes_read,
                     (double)ms / 1000.0, mb_per_s(r.bytes_read, ms));
        if (r.files_continued > 0) {
            (void)printf("%zu file%s continued from a checkpoint (%" PRIu64 " bytes kept)\n",
                         r.files_continued, r.files_continued == 1 ? "" : "s", r.bytes_continued);
        }
        if (r.archive_folder) {
            (void)printf("from the archive's game folder: %s\n",
                         strcmp(r.zip_member, "/") == 0 ? "its top" : r.zip_member);
        } else if (req.zip) {
            (void)printf("from the zip: %s, %s", r.zip_member,
                         r.zip_method == 0 ? "stored" : "Deflate");
            if (r.zip_method != 0) {
                (void)printf(", %u pass%s, %.1f MiB read again, %zu file%s from a %.1f MiB "
                             "spool",
                             r.passes, r.passes == 1 ? "" : "es", mib(r.reread_bytes),
                             r.files_from_spool, r.files_from_spool == 1 ? "" : "s",
                             mib(r.spool_peak));
                if (r.zip_resumed_at > 0) {
                    (void)printf(", continued %.1f MiB into the image", mib(r.zip_resumed_at));
                }
            }
            (void)printf("\n");
        }
        print_image_hash(&r);
        if (r.has_title_id) {
            (void)printf("title id %08" PRIX32 " \"%s\" (default.xbe)\n", r.title_id, r.title_name);
        } else {
            (void)printf("no title id: %s\n", r.title_note != NULL ? r.title_note : "?");
        }
    } else {
        if (err == XC_ERR_IMAGE_HASH) {
            print_image_hash(&r);
        }
        if (r.resumed || r.files_written > 0) {
            (void)fprintf(stderr,
                          "  %zu files are done in %s; running the same install again resumes\n",
                          r.files_written + r.files_skipped, r.folder);
        }
        rc = install_fail(&r, remote ? &env : NULL, image, err);
    }
    free(buf);
    if (remote) {
        env_close(&env);
#ifdef XCAT_SMB
    } else if (smb != NULL) {
        xcat_cli_smb_close(smb);
        if (have_smb_creds) {
            xc_cred_free(&smb_creds);
        }
#endif
    } else {
        xc_host_file_close(&hf);
    }
    return rc;
}

/* --- zip ------------------------------------------------------------------------------- */

static const char *method_label(uint16_t method, char *buf, size_t size)
{
    const char *name = xc_zip_method_name(method);
    if (name != NULL) {
        return name;
    }
    (void)snprintf(buf, size, "method %u", (unsigned)method);
    return buf;
}

/* Lists a zip's entries and checks its image like the installer will (docs/formats/zip.md). */
static int zip_report(const char *where, const xc_source *src)
{
    xc_zip_listing l;
    xc_err err = xc_zip_list(src, &l);
    if (err != XC_OK) {
        return fail(where, err);
    }
    (void)printf("zip: %zu entr%s%s, central directory at %" PRIu64 "\n", l.count,
                 l.count == 1 ? "y" : "ies", l.zip64 ? ", Zip64" : "", l.cd_offset);
    (void)printf("  %-13s %15s %15s %-8s  name\n", "method", "size", "compressed", "crc32");
    for (size_t i = 0; i < l.count; i++) {
        const xc_zip_entry *e = &l.entries[i];
        char mbuf[32];
        (void)printf("  %-13s %15" PRIu64 " %15" PRIu64 " %08" PRIx32 "  %s\n",
                     method_label(e->method, mbuf, sizeof mbuf), e->size, e->compressed_size,
                     e->crc32, e->name);
    }
    size_t pick = 0;
    err = xc_zip_pick_image(&l, &pick);
    if (err != XC_OK) {
        xc_zip_listing_free(&l);
        return fail(where, err);
    }
    xc_zip_member m;
    err = xc_zip_open_member(src, &l, pick, &m);
    const xc_zip_entry *e = &l.entries[pick];
    if (err == XC_ERR_ZIP_UNSUPPORTED) {
        char mbuf[32];
        (void)fprintf(stderr,
                      "xcat-cli: %s: %s is %s%s; only stored and Deflate members can be "
                      "installed\n",
                      where, e->name, method_label(e->method, mbuf, sizeof mbuf),
                      (e->flags & 0x2041U) != 0 ? ", encrypted" : "");
        xc_zip_listing_free(&l);
        return 1;
    }
    if (err != XC_OK) {
        xc_zip_listing_free(&l);
        return fail(where, err);
    }
    (void)printf(
        "image: %s, %s, %" PRIu64 " bytes (%" PRIu64 " compressed, %.1f%%), data at "
        "%" PRIu64 "\n",
        e->name, m.method == XC_ZIP_STORED ? "stored" : "Deflate", m.size, m.compressed_size,
        m.size > 0 ? 100.0 * (double)m.compressed_size / (double)m.size : 100.0, m.data_offset);
    xc_zip_listing_free(&l);
    return 0;
}

/* xcat-cli zip [net options] <zip|url>: the entries and the image, read with Range. */
static int cmd_zip(int argc, char **argv)
{
    net_opts o;
    int i = parse_net_opts(argc, argv, &o);
    if (i < 0 || argc - i != 1) {
        return usage();
    }
    const char *where = argv[i];
    if (!is_url(where)) {
        xc_host_file hf = {NULL};
        xc_source src;
        xc_err err = xc_host_file_open(&src, &hf, where);
        if (err != XC_OK) {
            return fail(where, err);
        }
        int rc = zip_report(where, &src);
        xc_host_file_close(&hf);
        return rc;
    }
    static http_env env;
    static xc_http_resource res;
    static xc_http_source hs;
    xc_source src;
    xc_err err = xc_http_resource_init(&res, where, strlen(where));
    if (err != XC_OK) {
        return fail(where, err);
    }
    err = env_open(&env, &o);
    if (err != XC_OK) {
        return fail("network", err);
    }
    err = xc_http_source_open(&src, &hs, &env.client, &res);
    int rc = err == XC_OK ? zip_report(where, &src) : get_fail(&env.client, env.tls, where, err);
    env_close(&env);
    return rc;
}

/*
 * xcat-cli ntp [--port <n>] [--timeout <ms>] [--floor <unix-seconds>] [server]: asks a time
 * server like the console does (docs/formats/sntp.md) and says how far this PC's clock is off.
 */
static int cmd_ntp(int argc, char **argv)
{
    const char *server = XC_SNTP_DEFAULT_SERVER;
    unsigned long port = XC_SNTP_PORT;
    unsigned long timeout = XC_SNTP_DEFAULT_TIMEOUT_MS;
    unsigned long floor_s = 0;
    bool have_server = false;
    for (int i = 0; i < argc; i++) {
        bool more = i + 1 < argc;
        if (strcmp(argv[i], "--port") == 0 && more) {
            if (!parse_uint(argv[++i], 65535UL, &port) || port == 0) {
                return usage();
            }
        } else if (strcmp(argv[i], "--timeout") == 0 && more) {
            if (!parse_uint(argv[++i], 600000UL, &timeout)) {
                return usage();
            }
        } else if (strcmp(argv[i], "--floor") == 0 && more) {
            if (!parse_uint(argv[++i], 0xFFFFFFFFUL, &floor_s)) {
                return usage();
            }
        } else if (argv[i][0] != '-' && !have_server) {
            server = argv[i];
            have_server = true;
        } else {
            return usage();
        }
    }
    xc_err err = xc_host_net_startup();
    if (err != XC_OK) {
        return fail("network", err);
    }
    xc_udp udp;
    xc_clock clock;
    xc_entropy entropy;
    xc_host_udp(&udp);
    xc_host_clock(&clock);
    xc_host_entropy(&entropy);
    xc_sntp_result r;
    xc_sntp_failure f;
    memset(&r, 0, sizeof r);
    memset(&f, 0, sizeof f);
    err = xc_sntp_query(&udp, &clock, &entropy, server, (uint16_t)port, (uint32_t)timeout,
                        (int64_t)floor_s, &r, &f);
    /* This PC's clock and the answer's time carried forward on the monotonic clock, together. */
    struct timespec pc;
    if (timespec_get(&pc, TIME_UTC) != TIME_UTC) {
        pc.tv_sec = time(NULL);
        pc.tv_nsec = 0;
    }
    int64_t pc_ms = (int64_t)pc.tv_sec * 1000 + (int64_t)pc.tv_nsec / 1000000;
    int64_t ntp_ms = r.unix_ms + (int64_t)(clock.now_ms(clock.ctx) - r.mono_ms);
    xc_host_net_cleanup();
    if (err != XC_OK) {
        (void)fprintf(stderr, "xcat-cli: NTP %s: %s%s%s%s%s (%u request%s)\n", server,
                      xc_err_str(err), f.why != NULL ? ": " : "", f.why != NULL ? f.why : "",
                      f.kiss[0] != '\0' ? " " : "", f.kiss, f.attempts, f.attempts == 1 ? "" : "s");
        return 1;
    }
    time_t ntp_s = (time_t)(ntp_ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    (void)gmtime_s(&tm, &ntp_s);
#else
    (void)gmtime_r(&ntp_s, &tm);
#endif
    char date[32];
    (void)strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
    (void)printf("NTP %s: %s UTC, stratum %u, round trip %" PRIu32 " ms, %u request%s\n", server,
                 date, (unsigned)r.stratum, r.rtt_ms, r.attempts, r.attempts == 1 ? "" : "s");
    (void)printf("This PC's clock is %+.1f s off\n", (double)(pc_ms - ntp_ms) / 1000.0);
    return 0;
}

/* --- catalog --------------------------------------------------------------------------- */

static void print_hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        (void)printf("%02x", p[i]);
    }
}

static void print_game(const xc_catalog_game *g)
{
    (void)printf("%s", g->title);
    if (g->year != 0) {
        (void)printf(" (%u)", (unsigned)g->year);
    }
    if (g->developer[0] != '\0' || g->publisher[0] != '\0') {
        (void)printf(" - %s / %s", g->developer[0] != '\0' ? g->developer : "?",
                     g->publisher[0] != '\0' ? g->publisher : "?");
    }
    if (g->genres[0] != '\0') {
        (void)printf(" [%s]", g->genres);
    }
    if (g->wikidata != 0) {
        (void)printf(" Q%" PRIu32, g->wikidata);
    }
    (void)printf("\n");
}

static void print_release(const xc_catalog_release *r, bool details)
{
    (void)printf("  %08" PRIX32 "@%-16s %s  %" PRIu64 " bytes", r->title_id, r->variant, r->name,
                 r->size);
    if (r->redump_id != 0) {
        (void)printf("  redump %" PRIu32, r->redump_id);
    }
    (void)printf("\n");
    if (details) {
        (void)printf("    region: %s\n    crc32 %08" PRIx32 "  md5 ",
                     r->region[0] != '\0' ? r->region : "?", r->crc32);
        print_hex(r->md5, sizeof r->md5);
        (void)printf("  sha1 ");
        print_hex(r->sha1, sizeof r->sha1);
        (void)printf("\n");
    }
}

/* A game and every release of it (releases are sorted by title ID, not game). */
static void print_game_releases(const xc_catalog *cat, uint32_t game,
                                const xc_catalog_release *only)
{
    xc_catalog_game g;
    (void)xc_catalog_game_at(cat, game, &g);
    print_game(&g);
    if (only != NULL) {
        print_release(only, true);
        return;
    }
    for (size_t i = 0; i < cat->release_count; i++) {
        xc_catalog_release r;
        (void)xc_catalog_release_at(cat, i, &r);
        if (r.game == game) {
            print_release(&r, false);
        }
    }
}

/* "<8 hex digits>[@<variant>]" (map.md §4.1). */
static bool parse_title_key(const char *s, uint32_t *id, const char **variant)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        int c = (unsigned char)s[i];
        if (!isxdigit(c)) {
            return false;
        }
        v = v << 4U | (uint32_t)(isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
    }
    if (s[8] != '\0' && s[8] != '@') {
        return false;
    }
    *id = v;
    *variant = s[8] == '@' ? s + 9 : NULL;
    return true;
}

/*
 * xcat-cli catalog <catalog.bin> [--title <id>[@variant] | --name <name> | <query>]: checks a
 * catalog (docs/formats/catalog.md) and lists its games, or looks one release up the way a
 * map entry is (by title ID and variant, or by name).
 */
static int cmd_catalog(int argc, char **argv)
{
    bool by_name = argc == 3 && strcmp(argv[1], "--name") == 0;
    bool by_title = argc == 3 && strcmp(argv[1], "--title") == 0;
    uint32_t id = 0;
    const char *variant = NULL;
    if (argc < 1 || argc > 3 || (argc == 3 && !by_name && !by_title) ||
        (by_title && !parse_title_key(argv[2], &id, &variant))) {
        return usage();
    }
    const char *path = argv[0];
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(path, XC_CATALOG_MAX_SIZE, &data, &len);
    xc_catalog cat = {0};
    if (err == XC_OK) {
        err = xc_catalog_open(&cat, data, len);
    }
    if (err != XC_OK) {
        free(data);
        return fail(path, err);
    }
    (void)printf("catalog %s: version 1.%u, %" PRIu32 " games, %" PRIu32
                 " releases, snapshot: %s\n",
                 path, (unsigned)cat.minor, cat.game_count, cat.release_count,
                 cat.snapshot[0] != '\0' ? cat.snapshot : "?");
    int rc = 0;
    if (argc == 3) {
        size_t found = XC_CATALOG_NONE;
        size_t count = 0;
        if (by_name) {
            found = xc_catalog_find_name(&cat, argv[2]);
            count = found != XC_CATALOG_NONE ? 1 : 0;
        } else if (variant != NULL) {
            found = xc_catalog_find_release(&cat, id, variant);
            count = found != XC_CATALOG_NONE ? 1 : 0;
        } else {
            found = xc_catalog_find_title(&cat, id, &count); /* [found, found + count) */
        }
        for (size_t i = 0; i < count; i++) {
            xc_catalog_release r;
            (void)xc_catalog_release_at(&cat, found + i, &r);
            print_game_releases(&cat, r.game, &r);
        }
        if (count == 0) {
            (void)printf("%s: not in catalog\n", argv[2]);
            rc = 1;
        }
    } else {
        const char *query = argc == 2 ? argv[1] : "";
        size_t shown = 0;
        for (size_t i = 0; i < cat.game_count; i++) {
            if (xc_catalog_game_matches(&cat, i, query)) {
                print_game_releases(&cat, (uint32_t)i, NULL);
                shown++;
            }
        }
        (void)printf("%zu game%s\n", shown, shown == 1 ? "" : "s");
    }
    free(data);
    return rc;
}

int main(int argc, char **argv)
{
#ifdef XCAT_SMB
    bool handled = false;
    int smb_rc = xcat_cli_smb(argc, argv, &handled);
    if (handled) {
        return smb_rc == 2 ? usage() : smb_rc;
    }
#endif
    if (argc >= 2 && strcmp(argv[1], "catalog") == 0) {
        return cmd_catalog(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "ntp") == 0) {
        return cmd_ntp(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        return cmd_get(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "install") == 0) {
        return cmd_install(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "zip") == 0) {
        return cmd_zip(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "map") == 0) {
        return cmd_map(argc - 2, argv + 2);
    }
    if (argc == 3 && strcmp(argv[1], "layout") == 0) {
        return cmd_layout(argv[2]);
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "check") == 0) {
        return cmd_check(argv[2], argc == 4 ? argv[3] : NULL);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return cmd_list(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "extract") == 0) {
        return cmd_extract(argv[2], argv[3]);
    }
    return usage();
}
