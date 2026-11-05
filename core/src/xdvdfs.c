/*
 * XDVDFS reader. Implements docs/formats/xdvdfs.md; "§n" below refers to it.
 *
 * Memory: one fixed table buffer (MAX_TABLE_READ, allocated up front), a
 * visited map and a stack sized for the largest directory seen, plus the
 * growing output. Nothing recurses.
 */
#include "xcat/xdvdfs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"

#define SECTOR 2048U
#define VD_OFFSET 0x10000U
#define VD_ROOT_SECTOR 0x14U
#define VD_ROOT_SIZE 0x18U
#define VD_FILETIME 0x1CU
#define VD_TAIL_MAGIC 0x7ECU
#define MAGIC_LEN 20U
#define ENTRY_HEADER 14U
#define ATTR_DIRECTORY 0x10U
#define PAD16 0xFFFFU
/* §3 rule 1: u16 child offsets in 4-byte units can't reach past this. */
#define MAX_TABLE_READ (0x40000U + SECTOR)

static const char magic[MAGIC_LEN + 1] = "MICROSOFT*XBOX*MEDIA";

/* §1: probe order. */
static const struct {
    xc_xdvdfs_layout layout;
    uint64_t offset;
} layouts[] = {
    {XC_XDVDFS_XISO, 0},
    {XC_XDVDFS_XGD1, 0x18300000U},
    {XC_XDVDFS_XGD2, 0x0FD90000U},
    {XC_XDVDFS_XGD3, 0x02080000U},
};

void xc_xdvdfs_default_limits(xc_xdvdfs_limits *out)
{
    /* §6 */
    out->max_entries = 65536;
    out->max_depth = 64;
    out->max_path_len = 1024;
}

static uint64_t sector_offset(uint64_t partition, uint32_t sector)
{
    return partition + (uint64_t)sector * SECTOR;
}

static bool extent_ok(const xc_source *src, uint64_t offset, uint64_t len)
{
    return offset <= src->size && len <= src->size - offset;
}

xc_err xc_xdvdfs_open(const xc_source *src, xc_xdvdfs_volume *out)
{
    if (src == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        uint8_t vd[SECTOR];
        uint64_t at = layouts[i].offset + VD_OFFSET;
        if (!extent_ok(src, at, SECTOR)) {
            continue;
        }
        xc_err err = xc_source_read(src, at, vd, sizeof vd);
        if (err != XC_OK) {
            return err;
        }
        if (memcmp(vd, magic, MAGIC_LEN) != 0) {
            continue;
        }
        /* §2: a matching leading magic commits to this layout. */
        if (memcmp(vd + VD_TAIL_MAGIC, magic, MAGIC_LEN) != 0) {
            return XC_ERR_CORRUPT;
        }
        xc_xdvdfs_volume v;
        v.layout = layouts[i].layout;
        v.partition_offset = layouts[i].offset;
        if (!xc_rd_u32le(vd, sizeof vd, VD_ROOT_SECTOR, &v.root_sector) ||
            !xc_rd_u32le(vd, sizeof vd, VD_ROOT_SIZE, &v.root_size) ||
            !xc_rd_u64le(vd, sizeof vd, VD_FILETIME, &v.filetime)) {
            return XC_ERR_CORRUPT;
        }
        bool no_files = v.root_sector == 0 && v.root_size == 0;
        if (!no_files &&
            !extent_ok(src, sector_offset(v.partition_offset, v.root_sector), v.root_size)) {
            return XC_ERR_CORRUPT;
        }
        *out = v;
        return XC_OK;
    }
    return XC_ERR_NOT_XDVDFS;
}

/* --- listing ---------------------------------------------------------------- */

typedef struct item {
    size_t path_off; /* into builder.paths */
    uint64_t offset;
    uint32_t size;
    uint32_t sector;
    uint32_t depth; /* directories: depth of this directory */
    uint8_t attributes;
} item;

typedef struct name_ref {
    const uint8_t *p;
    size_t len;
} name_ref;

typedef struct builder {
    const xc_source *src;
    uint64_t partition;
    xc_xdvdfs_limits lim;
    item *files;
    size_t nfiles, cap_files;
    item *dirs; /* also the queue of directories still to walk */
    size_t ndirs, cap_dirs;
    char *paths;
    size_t paths_len, cap_paths;
    uint8_t *table; /* MAX_TABLE_READ bytes, allocated once */
    uint8_t *seen;  /* one byte per 4-byte slot of the current table */
    size_t cap_seen;
    uint32_t *stack;
    size_t cap_stack;
    name_ref *names; /* names in the current directory, for the duplicate check */
    size_t nnames, cap_names;
    uint64_t total;
    xc_xdvdfs_have_fn have; /* list_partial only */
    void *have_ctx;
    bool skipped; /* the last walk_dir found its table unavailable */
} builder;

/*
 * Returns `p` resized to hold at least `need` elements of `elem` bytes, updating
 * *cap; or NULL on overflow or allocation failure, leaving `p` and *cap untouched.
 * Callers assign the result back to their typed pointer.
 */
static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need == 0) {
        need = 1; /* so a non-NULL result always means "usable" */
    }
    if (need <= *cap) {
        return p;
    }
    size_t n = *cap ? *cap : 16;
    while (n < need) {
        if (n > SIZE_MAX / 2) {
            return NULL;
        }
        n *= 2;
    }
    if (n > SIZE_MAX / elem) {
        return NULL;
    }
    void *q = realloc(p, n * elem);
    if (q != NULL) {
        *cap = n;
    }
    return q;
}

static int fold(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static int name_cmp(const void *a, const void *b)
{
    const name_ref *x = a;
    const name_ref *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    for (size_t i = 0; i < n; i++) {
        int d = fold(x->p[i]) - fold(y->p[i]);
        if (d != 0) {
            return d;
        }
    }
    return (x->len > y->len) - (x->len < y->len);
}

/* §4 */
static bool name_ok(const uint8_t *n, size_t len)
{
    if (len == 0 || (len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (n[i] < 0x20 || n[i] == '/' || n[i] == '\\') {
            return false;
        }
    }
    return true;
}

static xc_err add_path(builder *b, size_t parent_off, size_t parent_len, const uint8_t *name,
                       size_t nlen, size_t *out_off)
{
    size_t len = parent_len ? parent_len + 1 + nlen : nlen;
    if (len > b->lim.max_path_len) {
        return XC_ERR_LIMIT;
    }
    char *paths = grow(b->paths, &b->cap_paths, b->paths_len + len + 1, 1);
    if (paths == NULL) {
        return XC_ERR_NOMEM;
    }
    b->paths = paths;
    char *dst = b->paths + b->paths_len;
    if (parent_len) {
        memcpy(dst, b->paths + parent_off, parent_len);
        dst[parent_len] = '/';
        dst += parent_len + 1;
    }
    memcpy(dst, name, nlen);
    dst[nlen] = '\0';
    *out_off = b->paths_len;
    b->paths_len += len + 1;
    return XC_OK;
}

static xc_err add_entry(builder *b, size_t parent_off, size_t parent_len, uint32_t depth,
                        const uint8_t *name, size_t nlen, uint32_t sector, uint32_t size,
                        uint8_t attr)
{
    if (b->nfiles + b->ndirs >= b->lim.max_entries) {
        return XC_ERR_LIMIT;
    }
    item it;
    it.offset = sector_offset(b->partition, sector);
    it.size = size;
    it.sector = sector;
    it.attributes = attr;
    it.depth = depth + 1;
    bool is_dir = (attr & ATTR_DIRECTORY) != 0;
    if (is_dir) {
        if (it.depth > b->lim.max_depth) {
            return XC_ERR_LIMIT; /* also stops tables that point back to an ancestor */
        }
    } else if (size > 0 && !extent_ok(b->src, it.offset, size)) {
        return XC_ERR_CORRUPT; /* §5: data past the end, e.g. a truncated image */
    }
    xc_err err = add_path(b, parent_off, parent_len, name, nlen, &it.path_off);
    if (err != XC_OK) {
        return err;
    }
    if (is_dir) {
        item *dirs = grow(b->dirs, &b->cap_dirs, b->ndirs + 1, sizeof *dirs);
        if (dirs == NULL) {
            return XC_ERR_NOMEM;
        }
        b->dirs = dirs;
        b->dirs[b->ndirs++] = it;
    } else {
        item *files = grow(b->files, &b->cap_files, b->nfiles + 1, sizeof *files);
        if (files == NULL) {
            return XC_ERR_NOMEM;
        }
        b->files = files;
        b->files[b->nfiles++] = it;
        b->total += size;
    }
    return XC_OK;
}

/* Reads one directory table (§3) and adds its entries; subdirectories are queued in b->dirs. */
static xc_err walk_dir(builder *b, uint32_t sector, uint32_t size, size_t path_off, size_t path_len,
                       uint32_t depth)
{
    if (size == 0) {
        return XC_OK; /* §3 rule 2 */
    }
    uint64_t at = sector_offset(b->partition, sector);
    if (!extent_ok(b->src, at, size)) {
        return XC_ERR_CORRUPT;
    }
    size_t tl = size < MAX_TABLE_READ ? size : MAX_TABLE_READ;
    if (b->have != NULL && !b->have(b->have_ctx, at, tl)) {
        b->skipped = true; /* §9: not read yet */
        return XC_OK;
    }
    xc_err err = xc_source_read(b->src, at, b->table, tl);
    if (err != XC_OK) {
        return err;
    }
    const uint8_t *t = b->table;
    uint16_t first = 0;
    if (!xc_rd_u16le(t, tl, 0, &first)) {
        return XC_ERR_CORRUPT;
    }
    if (first == PAD16) {
        return XC_OK; /* §3 rule 2: empty directory */
    }

    size_t slots = tl / 4 + 1;
    uint8_t *seen = grow(b->seen, &b->cap_seen, slots, 1);
    if (seen == NULL) {
        return XC_ERR_NOMEM;
    }
    b->seen = seen;
    uint32_t *stack = grow(b->stack, &b->cap_stack, 2 * slots + 1, sizeof *stack);
    if (stack == NULL) {
        return XC_ERR_NOMEM;
    }
    b->stack = stack;
    memset(b->seen, 0, slots);
    b->nnames = 0;

    /* Each slot is processed at most once and pushes at most two children, so the stack can't
       exceed 2 * slots. */
    size_t sp = 0;
    b->stack[sp++] = 0;
    while (sp > 0) {
        uint32_t slot = b->stack[--sp];
        if (slot >= slots || b->seen[slot]) {
            return XC_ERR_CORRUPT; /* §3 rule 4: reached twice */
        }
        b->seen[slot] = 1;
        size_t pos = (size_t)slot * 4U;
        uint16_t left = 0;
        uint16_t right = 0;
        uint32_t esector = 0;
        uint32_t esize = 0;
        uint8_t attr = 0;
        uint8_t nlen = 0;
        if (!xc_rd_u16le(t, tl, pos, &left) || !xc_rd_u16le(t, tl, pos + 2, &right) ||
            !xc_rd_u32le(t, tl, pos + 4, &esector) || !xc_rd_u32le(t, tl, pos + 8, &esize) ||
            !xc_rd_u8(t, tl, pos + 12, &attr) || !xc_rd_u8(t, tl, pos + 13, &nlen)) {
            return XC_ERR_CORRUPT; /* §3 rule 3 */
        }
        if (left == PAD16 || !xc_in_bounds(tl, pos + ENTRY_HEADER, nlen)) {
            return XC_ERR_CORRUPT; /* §3 rule 3: padding or past the table */
        }
        const uint8_t *name = t + pos + ENTRY_HEADER;
        if (!name_ok(name, nlen)) {
            return XC_ERR_CORRUPT;
        }
        name_ref *names = grow(b->names, &b->cap_names, b->nnames + 1, sizeof *names);
        if (names == NULL) {
            return XC_ERR_NOMEM;
        }
        b->names = names;
        b->names[b->nnames].p = name;
        b->names[b->nnames].len = nlen;
        b->nnames++;

        err = add_entry(b, path_off, path_len, depth, name, nlen, esector, esize, attr);
        if (err != XC_OK) {
            return err;
        }
        if (right != 0) {
            b->stack[sp++] = right;
        }
        if (left != 0) {
            b->stack[sp++] = left;
        }
    }

    /* §4: names equal after case folding would overwrite each other on extraction. */
    qsort(b->names, b->nnames, sizeof *b->names, name_cmp);
    for (size_t i = 1; i < b->nnames; i++) {
        if (name_cmp(&b->names[i - 1], &b->names[i]) == 0) {
            return XC_ERR_CORRUPT;
        }
    }
    return XC_OK;
}

static int file_cmp(const void *a, const void *b)
{
    const xc_xdvdfs_entry *x = a;
    const xc_xdvdfs_entry *y = b;
    if (x->offset != y->offset) {
        return x->offset < y->offset ? -1 : 1;
    }
    return strcmp(x->path, y->path);
}

static xc_err to_entries(const builder *b, const item *items, size_t n, xc_xdvdfs_entry **out)
{
    *out = NULL;
    if (n == 0) {
        return XC_OK;
    }
    if (n > SIZE_MAX / sizeof **out) {
        return XC_ERR_NOMEM;
    }
    xc_xdvdfs_entry *e = malloc(n * sizeof *e);
    if (e == NULL) {
        return XC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        e[i].path = b->paths + items[i].path_off;
        e[i].offset = items[i].offset;
        e[i].size = items[i].size;
        e[i].sector = items[i].sector;
        e[i].attributes = items[i].attributes;
    }
    *out = e;
    return XC_OK;
}

/* Grows a flag array to at least `need` entries, new ones false. */
static xc_err grow_flags(bool **flags, size_t *cap, size_t need)
{
    size_t old = *cap;
    bool *p = grow(*flags, cap, need, sizeof *p);
    if (p == NULL) {
        return XC_ERR_NOMEM;
    }
    memset(p + old, 0, (*cap - old) * sizeof *p);
    *flags = p;
    return XC_OK;
}

static xc_err list(const xc_source *src, const xc_xdvdfs_volume *vol,
                   const xc_xdvdfs_limits *limits, xc_xdvdfs_have_fn have, void *have_ctx,
                   xc_xdvdfs_listing *out)
{
    if (src == NULL || vol == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);

    builder b;
    memset(&b, 0, sizeof b);
    b.src = src;
    b.have = have;
    b.have_ctx = have_ctx;
    bool *pending = NULL;
    size_t cap_pending = 0;
    b.partition = vol->partition_offset;
    if (limits != NULL) {
        b.lim = *limits;
    } else {
        xc_xdvdfs_default_limits(&b.lim);
    }

    /* The root's path is empty: give it a terminator so offset 0 is a valid string. */
    b.table = malloc(MAX_TABLE_READ);
    b.paths = grow(NULL, &b.cap_paths, 1, 1);
    xc_err err = (b.table != NULL && b.paths != NULL) ? XC_OK : XC_ERR_NOMEM;
    if (err == XC_OK) {
        b.paths[0] = '\0';
        b.paths_len = 1;
        err = walk_dir(&b, vol->root_sector, vol->root_size, 0, 0, 0);
        if (b.skipped) {
            out->root_pending = true;
            out->pending_count = 1;
        }
    }
    /* Breadth-first: b.dirs grows while we walk it, so copy each item before use. */
    for (size_t i = 0; err == XC_OK && i < b.ndirs; i++) {
        item d = b.dirs[i];
        b.skipped = false;
        err = walk_dir(&b, d.sector, d.size, d.path_off, strlen(b.paths + d.path_off), d.depth);
        if (err == XC_OK && b.skipped) {
            err = grow_flags(&pending, &cap_pending, b.ndirs);
            if (err == XC_OK) {
                pending[i] = true;
                out->pending_count++;
            }
        }
    }
    if (err == XC_OK && pending != NULL) {
        /* one flag per directory, including those added after the last pending one */
        err = grow_flags(&pending, &cap_pending, b.ndirs);
    }
    if (err == XC_OK) {
        err = to_entries(&b, b.files, b.nfiles, &out->files);
    }
    if (err == XC_OK) {
        err = to_entries(&b, b.dirs, b.ndirs, &out->dirs);
    }

    free(b.files);
    free(b.dirs);
    free(b.table);
    free(b.seen);
    free(b.stack);
    free(b.names);
    if (err != XC_OK) {
        free(out->files);
        free(out->dirs);
        free(b.paths);
        free(pending);
        memset(out, 0, sizeof *out);
        return err;
    }
    out->dir_pending = pending;
    out->file_count = b.nfiles;
    out->dir_count = b.ndirs;
    out->total_file_bytes = b.total;
    out->paths_ = b.paths;
    if (out->files != NULL) {
        qsort(out->files, out->file_count, sizeof *out->files, file_cmp);
    }
    return XC_OK;
}

xc_err xc_xdvdfs_list(const xc_source *src, const xc_xdvdfs_volume *vol,
                      const xc_xdvdfs_limits *limits, xc_xdvdfs_listing *out)
{
    return list(src, vol, limits, NULL, NULL, out);
}

xc_err xc_xdvdfs_list_partial(const xc_source *src, const xc_xdvdfs_volume *vol,
                              const xc_xdvdfs_limits *limits, xc_xdvdfs_have_fn have,
                              void *have_ctx, xc_xdvdfs_listing *out)
{
    if (have == NULL) {
        if (out != NULL) {
            memset(out, 0, sizeof *out);
        }
        return XC_ERR_INVALID_ARG;
    }
    return list(src, vol, limits, have, have_ctx, out);
}

xc_err xc_xdvdfs_find_root(const xc_source *src, const xc_xdvdfs_volume *vol, const char *name,
                           xc_xdvdfs_entry *out, bool *found)
{
    memset(out, 0, sizeof *out);
    *found = false;
    if (src == NULL || vol == NULL || name == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    uint32_t size = vol->root_size;
    if (size == 0) {
        return XC_OK; /* §3 rule 2: nothing in the image */
    }
    uint64_t at = sector_offset(vol->partition_offset, vol->root_sector);
    if (!extent_ok(src, at, size)) {
        return XC_ERR_CORRUPT;
    }
    size_t tl = size < MAX_TABLE_READ ? size : MAX_TABLE_READ;
    size_t slots = tl / 4 + 1;
    uint8_t *t = malloc(tl);
    uint8_t *seen = calloc(slots, 1);
    uint32_t *stack = malloc((2 * slots + 1) * sizeof *stack);
    xc_err err =
        t != NULL && seen != NULL && stack != NULL ? xc_source_read(src, at, t, tl) : XC_ERR_NOMEM;
    uint16_t first = 0;
    if (err == XC_OK && !xc_rd_u16le(t, tl, 0, &first)) {
        err = XC_ERR_CORRUPT;
    }
    size_t want = strlen(name);
    size_t sp = 0;
    if (err == XC_OK && first != PAD16) {
        stack[sp++] = 0;
    }
    /* As walk_dir: every slot reached once, at most two pushed each. */
    while (err == XC_OK && sp > 0 && !*found) {
        uint32_t slot = stack[--sp];
        if (slot >= slots || seen[slot]) {
            err = XC_ERR_CORRUPT;
            break;
        }
        seen[slot] = 1;
        size_t pos = (size_t)slot * 4U;
        uint16_t left = 0;
        uint16_t right = 0;
        uint32_t esector = 0;
        uint32_t esize = 0;
        uint8_t attr = 0;
        uint8_t nlen = 0;
        if (!xc_rd_u16le(t, tl, pos, &left) || !xc_rd_u16le(t, tl, pos + 2, &right) ||
            !xc_rd_u32le(t, tl, pos + 4, &esector) || !xc_rd_u32le(t, tl, pos + 8, &esize) ||
            !xc_rd_u8(t, tl, pos + 12, &attr) || !xc_rd_u8(t, tl, pos + 13, &nlen) ||
            left == PAD16 || !xc_in_bounds(tl, pos + ENTRY_HEADER, nlen)) {
            err = XC_ERR_CORRUPT;
            break;
        }
        const uint8_t *n = t + pos + ENTRY_HEADER;
        bool same = nlen == want;
        for (size_t i = 0; same && i < want; i++) {
            same = fold(n[i]) == fold((uint8_t)name[i]);
        }
        if (same) {
            out->offset = sector_offset(vol->partition_offset, esector);
            out->size = esize;
            out->sector = esector;
            out->attributes = attr;
            *found = true;
        }
        if (right != 0) {
            stack[sp++] = right;
        }
        if (left != 0) {
            stack[sp++] = left;
        }
    }
    free(t);
    free(seen);
    free(stack);
    return err;
}

void xc_xdvdfs_listing_free(xc_xdvdfs_listing *listing)
{
    if (listing == NULL) {
        return;
    }
    free(listing->files);
    free(listing->dirs);
    free(listing->dir_pending);
    free(listing->paths_);
    memset(listing, 0, sizeof *listing);
}

/* --- stream order (§8) ------------------------------------------------------------ */

typedef struct dir_ref {
    const char *path;
    size_t len;
    size_t index; /* into listing->dirs */
} dir_ref;

static int dir_ref_cmp(const void *a, const void *b)
{
    const dir_ref *x = a;
    const dir_ref *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->path, y->path, n);
    if (c != 0) {
        return c;
    }
    return (x->len > y->len) - (x->len < y->len);
}

/* Length of the parent directory's path ("a/b" for "a/b/c"), 0 for the root. */
static size_t parent_len(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? (size_t)(slash - path) : 0;
}

/* When the parent of `path` is fully known: the root's, or a directory's from `ready`. */
static uint64_t parent_ready(const char *path, const dir_ref *refs, size_t n, const uint64_t *ready,
                             uint64_t root_ready)
{
    size_t len = parent_len(path);
    if (len == 0 || refs == NULL) {
        return root_ready;
    }
    dir_ref key = {path, len, 0};
    const dir_ref *hit = bsearch(&key, refs, n, sizeof *refs, dir_ref_cmp);
    return hit != NULL ? ready[hit->index] : root_ready;
}

static void note_table(xc_xdvdfs_order *o, uint64_t start, uint64_t end)
{
    o->tables++;
    if (start < o->tables_start) {
        o->tables_start = start;
    }
    if (end > o->tables_end) {
        o->tables_end = end;
    }
}

/* When each directory becomes fully known in a sequential read (§8). */
typedef struct known_map {
    dir_ref *refs; /* sorted by path */
    uint64_t *ready;
    size_t n;
    uint64_t root_ready;
    uint64_t all_ready; /* when every table has been read */
} known_map;

static void known_map_free(known_map *k)
{
    free(k->refs);
    free(k->ready);
    memset(k, 0, sizeof *k);
}

/* Fills `k`; also notes every table in `o` unless it is NULL. */
static xc_err known_map_build(const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                              known_map *k, xc_xdvdfs_order *o)
{
    memset(k, 0, sizeof *k);
    if (vol->root_size > 0) {
        uint64_t start = sector_offset(vol->partition_offset, vol->root_sector);
        k->root_ready = start + vol->root_size;
        if (o != NULL) {
            note_table(o, start, k->root_ready);
        }
    }
    k->all_ready = k->root_ready;

    /* A directory is fully known once its own table and every ancestor's table have been
       read. Directories come parents-first (§7), so each parent is done before its children. */
    size_t n = listing->dir_count;
    if (n == 0) {
        return XC_OK;
    }
    if (n > SIZE_MAX / sizeof *k->refs || n > SIZE_MAX / sizeof *k->ready) {
        return XC_ERR_NOMEM;
    }
    k->refs = malloc(n * sizeof *k->refs);
    k->ready = malloc(n * sizeof *k->ready);
    if (k->refs == NULL || k->ready == NULL) {
        known_map_free(k);
        return XC_ERR_NOMEM;
    }
    k->n = n;
    for (size_t i = 0; i < n; i++) {
        k->refs[i].path = listing->dirs[i].path;
        k->refs[i].len = strlen(listing->dirs[i].path);
        k->refs[i].index = i;
    }
    qsort(k->refs, n, sizeof *k->refs, dir_ref_cmp);
    for (size_t i = 0; i < n; i++) {
        const xc_xdvdfs_entry *d = &listing->dirs[i];
        uint64_t known = parent_ready(d->path, k->refs, n, k->ready, k->root_ready);
        if (d->size > 0) {
            uint64_t end = d->offset + d->size;
            if (o != NULL) {
                note_table(o, d->offset, end);
            }
            if (end > known) {
                known = end;
            }
        }
        k->ready[i] = known;
        if (known > k->all_ready) {
            k->all_ready = known;
        }
    }
    return XC_OK;
}

/* When the directory holding `path` is fully known. */
static uint64_t known_map_parent(const known_map *k, const char *path)
{
    return parent_ready(path, k->refs, k->n, k->ready, k->root_ready);
}

xc_err xc_xdvdfs_stream_order(const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                              xc_xdvdfs_order *out)
{
    if (vol == NULL || listing == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->tables_start = UINT64_MAX;
    out->data_start = UINT64_MAX;
    out->first_late = UINT64_MAX;

    known_map k;
    xc_err err = known_map_build(vol, listing, &k, out);
    if (err != XC_OK) {
        return err;
    }
    for (size_t i = 0; i < listing->file_count; i++) {
        const xc_xdvdfs_entry *f = &listing->files[i];
        if (f->size == 0) {
            continue; /* nothing to read; its sector isn't even checked (§5) */
        }
        if (f->offset < out->data_start) {
            out->data_start = f->offset;
        }
        if (f->offset + f->size > out->data_end) {
            out->data_end = f->offset + f->size;
        }
        uint64_t known = known_map_parent(&k, f->path);
        if (known > f->offset) {
            out->late_files++;
            out->late_bytes += f->size;
            if (f->offset < out->first_late) {
                out->first_late = f->offset;
            }
            if (known > out->last_table_end_needed) {
                out->last_table_end_needed = known;
            }
        }
    }
    known_map_free(&k);
    return XC_OK;
}

/* --- spool plan (§8.1) ------------------------------------------------------- */

typedef struct span {
    uint64_t start;
    uint64_t end;
} span;

static int span_cmp(const void *a, const void *b)
{
    const span *x = a;
    const span *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

/* Sorts and merges `s` in place; returns the new count. */
static size_t spans_merge(span *s, size_t n)
{
    if (n == 0) {
        return 0;
    }
    qsort(s, n, sizeof *s, span_cmp);
    size_t m = 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i].start <= s[m].end) {
            if (s[i].end > s[m].end) {
                s[m].end = s[i].end;
            }
        } else {
            s[++m] = s[i];
        }
    }
    return m + 1;
}

/* Appends [start, end) clipped to [lo, hi), if anything is left. */
static void span_add(span *s, size_t *n, uint64_t start, uint64_t end, uint64_t lo, uint64_t hi)
{
    if (start < lo) {
        start = lo;
    }
    if (end > hi) {
        end = hi;
    }
    if (start < end) {
        s[*n].start = start;
        s[*n].end = end;
        (*n)++;
    }
}

/*
 * Walks the holes between the merged spans `known` within [lo, hi) in order. Returns their
 * total size, or stops once `target` bytes have been walked and sets *at to where that is.
 */
static uint64_t holes_until(const span *known, size_t nk, uint64_t lo, uint64_t hi, uint64_t target,
                            uint64_t *at)
{
    uint64_t total = 0;
    uint64_t pos = lo;
    for (size_t i = 0; i <= nk && pos < hi; i++) {
        uint64_t hole_end = i < nk ? known[i].start : hi;
        if (hole_end > pos) {
            uint64_t len = hole_end - pos;
            if (total + len >= target && at != NULL) {
                *at = pos + (target - total);
                return target;
            }
            total += len;
        }
        if (i < nk && known[i].end > pos) {
            pos = known[i].end;
        }
    }
    if (at != NULL) {
        *at = hi;
    }
    return total;
}

xc_err xc_xdvdfs_spool_plan(const xc_xdvdfs_volume *vol, const xc_xdvdfs_listing *listing,
                            uint64_t budget, uint64_t checkpoint_bytes, xc_xdvdfs_spool *out)
{
    if (vol == NULL || listing == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->reread_start = UINT64_MAX;
    out->reread_from = UINT64_MAX;

    known_map k;
    xc_err err = known_map_build(vol, listing, &k, NULL);
    if (err != XC_OK) {
        return err;
    }
    uint64_t lo = vol->partition_offset; /* nothing before the partition is game data */
    uint64_t hi = k.all_ready;           /* from here on every byte is identified */
    out->all_known = hi;

    /* Identified bytes (tables, and file bytes once their directory is known) and late
       bytes (file bytes that pass before it is), both clipped to [lo, hi). */
    size_t cap = listing->file_count + listing->dir_count + 1;
    span *known = NULL;
    span *late = NULL;
    if (cap <= SIZE_MAX / sizeof *known) {
        known = malloc(cap * sizeof *known);
        late = malloc(cap * sizeof *late);
    }
    if (known == NULL || late == NULL) {
        free(known);
        free(late);
        known_map_free(&k);
        return XC_ERR_NOMEM;
    }
    size_t nk = 0;
    size_t nl = 0;
    if (vol->root_size > 0) {
        span_add(known, &nk, k.root_ready - vol->root_size, k.root_ready, lo, hi);
    }
    for (size_t i = 0; i < listing->dir_count; i++) {
        const xc_xdvdfs_entry *d = &listing->dirs[i];
        span_add(known, &nk, d->offset, d->offset + d->size, lo, hi);
    }
    for (size_t i = 0; i < listing->file_count; i++) {
        const xc_xdvdfs_entry *f = &listing->files[i];
        uint64_t end = f->offset + f->size;
        uint64_t ready = known_map_parent(&k, f->path);
        uint64_t split = ready < f->offset ? f->offset : (ready < end ? ready : end);
        span_add(late, &nl, f->offset, split, lo, hi);
        span_add(known, &nk, split, end, lo, hi);
    }
    known_map_free(&k);
    nk = spans_merge(known, nk);
    nl = spans_merge(late, nl);

    /* The spool keeps the latest `budget` unidentified bytes: when it is full, its oldest
       bytes are dropped. So the first unidentified - budget of them are dropped, and `stop` is
       where the last dropped byte ends. */
    out->unidentified = holes_until(known, nk, lo, hi, UINT64_MAX, NULL);
    uint64_t dropped = budget < out->unidentified ? out->unidentified - budget : 0;
    out->spooled = out->unidentified - dropped;
    uint64_t stop = lo;
    (void)holes_until(known, nk, lo, hi, dropped, &stop);

    /* Late bytes before `stop` were neither written nor kept: pass 2 reads them again. */
    for (size_t i = 0; i < nl; i++) {
        out->late_bytes += late[i].end - late[i].start;
        if (late[i].start < stop) {
            if (out->reread_start == UINT64_MAX) {
                out->reread_start = late[i].start;
            }
            out->reread_end = late[i].end < stop ? late[i].end : stop;
        }
    }
    if (out->reread_start != UINT64_MAX) {
        out->reread_from = checkpoint_bytes > 0
                               ? out->reread_start - out->reread_start % checkpoint_bytes
                               : out->reread_start;
    }
    free(known);
    free(late);
    return XC_OK;
}
