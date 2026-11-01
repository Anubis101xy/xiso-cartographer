/*
 * An extracted game folder as a trimmed xiso (xcat/folder_image.h). Implements
 * docs/formats/folder-image.md; "§n" below refers to it. The tables it writes are read back by
 * core/src/xdvdfs.c (docs/formats/xdvdfs.md), so every rule that reader enforces is met here.
 */
#include "xcat/folder_image.h"

#include <stdlib.h>
#include <string.h>

#include "xcat/hash.h"

#define SECTOR 2048U
#define VD_SECTOR 32U /* the volume descriptor (xdvdfs.md §2) */
#define HEAD_START ((uint64_t)VD_SECTOR * SECTOR)
#define VD_TAIL_MAGIC 0x7ECU
#define ENTRY_HEADER 14U
#define ATTR_DIRECTORY 0x10U
#define ATTR_FILE 0x20U   /* "archive", as Microsoft's tools write files */
#define MAX_CHILD 0xFFFEU /* an entry offset in 4-byte units; 0xFFFF reads as padding */
#define NONE UINT32_MAX

static const char magic[] = "MICROSOFT*XBOX*MEDIA"; /* 20 bytes, written without the NUL */

typedef struct extent {
    uint64_t start; /* in the image */
    uint64_t size;
    size_t item; /* the caller's index */
} extent;

struct xc_fimg {
    uint8_t *head; /* the image's bytes [HEAD_START, head_end): descriptor and tables */
    uint64_t head_end;
    uint64_t size;
    extent *files; /* non-empty files, by start */
    size_t file_count;
    size_t all_files; /* empty ones too */
    uint64_t file_bytes;
    xc_fimg_read_fn read;
    void *ctx;
    char validator[XC_FIMG_VALIDATOR_HEX + 1];
};

/* One item while building, in path order; the root folder is node `count`. */
typedef struct node {
    const char *path;
    const char *name; /* its last name, inside path */
    uint32_t name_len;
    uint32_t item;
    uint32_t parent;
    bool dir;
    uint64_t size;
    uint64_t mtime;
    uint64_t order;
    uint32_t path_rank; /* its place in path order, for ties of `order` */
    /* in its parent's table (§3) */
    uint32_t entry_off;
    uint32_t left;
    uint32_t right;
    /* folders: their table; files: their data */
    uint32_t sector;
    uint32_t table_size;
} node;

typedef struct builder {
    node *nodes;         /* count + 1 */
    uint32_t *kids;      /* every node but the root, grouped by parent in table order */
    uint32_t *kid_start; /* count + 2: kids of node d are kids[kid_start[d], kid_start[d + 1]) */
    size_t count;
    xc_fimg_problem *problem;
} builder;

static xc_err refuse(builder *b, xc_err err, size_t item, const char *why)
{
    if (b->problem != NULL) {
        b->problem->item = item;
        b->problem->why = why;
    }
    return err;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t round_up(uint64_t v)
{
    return (v + SECTOR - 1U) / SECTOR * SECTOR;
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const node *)a)->path, ((const node *)b)->path);
}

/* §4: where files' data goes. */
static int by_data_order(const void *a, const void *b)
{
    const node *x = *(const node *const *)a;
    const node *y = *(const node *const *)b;
    if (x->order != y->order) {
        return x->order < y->order ? -1 : 1;
    }
    return x->path_rank < y->path_rank ? -1 : x->path_rank > y->path_rank;
}

static uint8_t fold(uint8_t c)
{
    return c >= 'a' && c <= 'z' ? (uint8_t)(c - 'a' + 'A') : c;
}

/* §3: the order of names in a table, a-z folded to upper case (as xdvdfs.md §4 compares). */
static int name_order(const node *x, const node *y)
{
    uint32_t n = x->name_len < y->name_len ? x->name_len : y->name_len;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t a = fold((uint8_t)x->name[i]);
        uint8_t b = fold((uint8_t)y->name[i]);
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return x->name_len < y->name_len ? -1 : x->name_len > y->name_len;
}

static int by_table(const void *a, const void *b)
{
    const node *x = *(const node *const *)a;
    const node *y = *(const node *const *)b;
    if (x->parent != y->parent) {
        return x->parent < y->parent ? -1 : 1;
    }
    int c = name_order(x, y);
    return c != 0 ? c : strcmp(x->name, y->name);
}

/* §2: one path's names. */
static xc_err check_path(builder *b, node *nd)
{
    const char *p = nd->path;
    if (p == NULL) {
        return refuse(b, XC_ERR_INVALID_ARG, nd->item, "no path");
    }
    size_t len = strlen(p);
    if (len == 0 || len > XC_FIMG_MAX_PATH) {
        return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "its path is empty or too long");
    }
    unsigned depth = 0;
    const char *name = p;
    for (size_t i = 0; i <= len; i++) {
        if (p[i] != '/' && p[i] != '\0') {
            if ((uint8_t)p[i] < 0x20U || p[i] == '\\') {
                return refuse(b, XC_ERR_GAME_FOLDER, nd->item,
                              "a name has a control character or a backslash");
            }
            continue;
        }
        size_t n = (size_t)(p + i - name);
        if (n == 0 || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) {
            return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "a name is empty, \".\" or \"..\"");
        }
        if (n > XC_FIMG_MAX_NAME) {
            return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "a name is longer than 255 bytes");
        }
        if (++depth > XC_FIMG_MAX_DEPTH) {
            return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "it's more than 32 folders deep");
        }
        nd->name = name;
        nd->name_len = (uint32_t)n;
        name = p + i + 1;
    }
    if (!nd->dir && nd->size > UINT32_MAX) {
        return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "a file of 4 GiB or more");
    }
    return XC_OK;
}

/* The node whose path is p[0, len), among nodes[0, hi) (sorted by path), or NONE. */
static uint32_t find(const node *nodes, size_t hi, const char *p, size_t len)
{
    size_t lo = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strncmp(nodes[mid].path, p, len);
        if (c == 0) {
            c = nodes[mid].path[len] == '\0' ? 0 : 1;
        }
        if (c == 0) {
            return (uint32_t)mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NONE;
}

static uint32_t entry_size(uint32_t name_len)
{
    return (ENTRY_HEADER + name_len + 3U) & ~3U;
}

/* §3: the binary tree of kids[lo, hi): its root in the middle, written before its subtrees,
   so the table's first entry is its root. No entry crosses a sector. */
static uint32_t place(builder *b, size_t lo, size_t hi, uint32_t *pos)
{
    if (lo >= hi) {
        return NONE;
    }
    size_t mid = lo + (hi - lo) / 2;
    node *nd = &b->nodes[b->kids[mid]];
    uint32_t size = entry_size(nd->name_len);
    if (*pos % SECTOR + size > SECTOR) {
        *pos = (uint32_t)round_up(*pos);
    }
    nd->entry_off = *pos;
    *pos += size;
    nd->left = place(b, lo, mid, pos);
    nd->right = place(b, mid + 1, hi, pos);
    return b->kids[mid];
}

static xc_err parents(builder *b)
{
    for (size_t k = 0; k < b->count; k++) {
        node *nd = &b->nodes[k];
        xc_err err = check_path(b, nd);
        if (err != XC_OK) {
            return err;
        }
        if (k > 0 && strcmp(b->nodes[k - 1].path, nd->path) == 0) {
            return refuse(b, XC_ERR_INVALID_ARG, nd->item, "listed twice");
        }
        size_t plen = (size_t)(nd->name - nd->path);
        if (plen == 0) {
            nd->parent = (uint32_t)b->count;
            continue;
        }
        uint32_t p = find(b->nodes, k, nd->path, plen - 1U);
        if (p == NONE || !b->nodes[p].dir) {
            return refuse(b, XC_ERR_INVALID_ARG, nd->item, "its folder isn't listed");
        }
        nd->parent = p;
    }
    return XC_OK;
}

static xc_err tables(builder *b, uint32_t *next_sector)
{
    size_t n = b->count;
    const node **order = malloc((n + 1U) * sizeof *order);
    if (order == NULL) {
        return XC_ERR_NOMEM;
    }
    for (size_t k = 0; k < n; k++) {
        order[k] = &b->nodes[k];
    }
    qsort(order, n, sizeof *order, by_table);
    for (size_t k = 0; k < n; k++) {
        b->kids[k] = (uint32_t)(order[k] - b->nodes);
    }
    free(order);
    for (size_t d = 0; d <= n + 1; d++) {
        b->kid_start[d] = 0;
    }
    for (size_t i = 0; i < n; i++) {
        const node *nd = &b->nodes[b->kids[i]];
        b->kid_start[nd->parent + 1U]++;
        if (i > 0) {
            const node *prev = &b->nodes[b->kids[i - 1]];
            if (prev->parent == nd->parent && name_order(prev, nd) == 0) {
                return refuse(b, XC_ERR_GAME_FOLDER, nd->item,
                              "two names in a folder differ only in upper and lower case");
            }
        }
    }
    for (size_t d = 1; d <= n + 1; d++) {
        b->kid_start[d] += b->kid_start[d - 1];
    }
    /* The root's table first, then the folders' in path order (parents before children). */
    uint64_t sector = VD_SECTOR + 1U;
    for (size_t i = 0; i <= n; i++) {
        size_t d = i == 0 ? n : i - 1;
        node *dir = &b->nodes[d];
        if (d != n && !dir->dir) {
            continue;
        }
        size_t lo = b->kid_start[d];
        size_t hi = b->kid_start[d + 1];
        dir->sector = 0;
        dir->table_size = 0;
        if (lo == hi) {
            continue; /* an empty folder: no table (xdvdfs.md §3 rule 2) */
        }
        uint32_t pos = 0;
        (void)place(b, lo, hi, &pos);
        for (size_t j = lo; j < hi; j++) {
            if (b->nodes[b->kids[j]].entry_off / 4U > MAX_CHILD) {
                return refuse(b, XC_ERR_GAME_FOLDER, d == n ? SIZE_MAX : dir->item,
                              "a folder holds too many files for one directory table");
            }
        }
        dir->table_size = (uint32_t)round_up(pos);
        dir->sector = (uint32_t)sector;
        sector += dir->table_size / SECTOR;
        if ((sector - VD_SECTOR - 1U) * SECTOR > XC_FIMG_MAX_TABLES) {
            return refuse(b, XC_ERR_GAME_FOLDER, SIZE_MAX, "too many files and folders");
        }
    }
    *next_sector = (uint32_t)sector;
    return XC_OK;
}

static void write_entry(const builder *b, uint8_t *head, const node *dir, const node *nd)
{
    uint8_t *e = head + ((uint64_t)dir->sector * SECTOR - HEAD_START) + nd->entry_off;
    put16(e, nd->left == NONE ? 0 : b->nodes[nd->left].entry_off / 4U);
    put16(e + 2, nd->right == NONE ? 0 : b->nodes[nd->right].entry_off / 4U);
    put32(e + 4, nd->sector);
    put32(e + 8, nd->dir ? nd->table_size : (uint32_t)nd->size);
    e[12] = nd->dir ? ATTR_DIRECTORY : ATTR_FILE;
    e[13] = (uint8_t)nd->name_len;
    memcpy(e + ENTRY_HEADER, nd->name, nd->name_len);
    /* the rest up to 4 bytes stays 0xFF, as the table's unused space */
}

/* §5 */
static xc_err validator(const builder *b, char out[XC_FIMG_VALIDATOR_HEX + 1])
{
    xc_image_hash *h = NULL;
    xc_err err = xc_image_hash_create(&h);
    if (err != XC_OK) {
        return err;
    }
    for (size_t k = 0; k < b->count; k++) {
        const node *nd = &b->nodes[k];
        uint8_t rec[18];
        rec[0] = 0;
        rec[1] = nd->dir ? 'd' : 'f';
        put64(rec + 2, nd->dir ? 0 : nd->size);
        put64(rec + 10, nd->mtime);
        xc_image_hash_update(h, nd->path, strlen(nd->path));
        xc_image_hash_update(h, rec, sizeof rec);
    }
    uint8_t md5[XC_MD5_BYTES];
    uint8_t sha1[XC_SHA1_BYTES];
    xc_image_hash_final(h, md5, sha1);
    xc_image_hash_free(h);
    xc_hex_lower(sha1, sizeof sha1, out);
    return XC_OK;
}

static xc_err fimg_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const xc_fimg *img = ctx;
    if (offset > img->size || len > img->size - offset) {
        return XC_ERR_RANGE;
    }
    uint8_t *p = buf;
    while (len > 0) {
        size_t n;
        if (offset < HEAD_START) {
            n = HEAD_START - offset < len ? (size_t)(HEAD_START - offset) : len;
            memset(p, 0, n);
        } else if (offset < img->head_end) {
            n = img->head_end - offset < len ? (size_t)(img->head_end - offset) : len;
            memcpy(p, img->head + (offset - HEAD_START), n);
        } else {
            /* The last file starting at or before offset. */
            size_t lo = 0;
            size_t hi = img->file_count;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (img->files[mid].start <= offset) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            const extent *f = lo > 0 ? &img->files[lo - 1] : NULL;
            if (f != NULL && offset - f->start < f->size) {
                uint64_t left = f->size - (offset - f->start);
                n = left < len ? (size_t)left : len;
                xc_err err = img->read(img->ctx, f->item, offset - f->start, p, n);
                if (err != XC_OK) {
                    return err;
                }
            } else {
                /* Padding up to the next file, or to the end. */
                uint64_t next = lo < img->file_count ? img->files[lo].start : img->size;
                n = next - offset < len ? (size_t)(next - offset) : len;
                memset(p, 0, n);
            }
        }
        p += n;
        offset += n;
        len -= n;
    }
    return XC_OK;
}

static xc_err build(builder *b, const xc_fimg_item *items, xc_fimg *img)
{
    size_t n = b->count;
    for (size_t k = 0; k < n; k++) {
        node *nd = &b->nodes[k];
        nd->path = items[k].path;
        nd->item = (uint32_t)k;
        nd->dir = items[k].dir;
        nd->size = items[k].dir ? 0 : items[k].size;
        nd->mtime = items[k].mtime;
        nd->order = items[k].dir ? 0 : items[k].order;
        if (nd->path == NULL) {
            return refuse(b, XC_ERR_INVALID_ARG, k, "no path");
        }
    }
    qsort(b->nodes, n, sizeof *b->nodes, by_path);
    node *root = &b->nodes[n];
    root->path = "";
    root->name = "";
    root->dir = true;
    root->parent = NONE;
    root->item = NONE;
    xc_err err = parents(b);
    uint32_t sector = 0;
    if (err == XC_OK) {
        err = tables(b, &sector);
    }
    if (err != XC_OK) {
        return err;
    }
    img->head_end = (uint64_t)sector * SECTOR;
    /* §4: each non-empty file at a sector, by `order`, then in path order. */
    const node **files = malloc((n + 1U) * sizeof *files);
    if (files == NULL) {
        return XC_ERR_NOMEM;
    }
    size_t nfiles = 0;
    for (size_t k = 0; k < n; k++) {
        b->nodes[k].path_rank = (uint32_t)k;
        if (!b->nodes[k].dir) {
            files[nfiles++] = &b->nodes[k];
        }
    }
    qsort(files, nfiles, sizeof *files, by_data_order);
    uint64_t next = sector;
    for (size_t f = 0; f < nfiles; f++) {
        node *nd = &b->nodes[files[f] - b->nodes];
        img->all_files++;
        img->file_bytes += nd->size;
        nd->sector = 0;
        if (nd->size == 0) {
            continue;
        }
        nd->sector = (uint32_t)next;
        img->files[img->file_count].start = next * SECTOR;
        img->files[img->file_count].size = nd->size;
        img->files[img->file_count].item = nd->item;
        img->file_count++;
        next += round_up(nd->size) / SECTOR;
        if (next > UINT32_MAX) {
            free(files);
            return refuse(b, XC_ERR_GAME_FOLDER, nd->item, "the files are too big for one image");
        }
    }
    free(files);
    img->size = next * SECTOR;
    img->head = malloc((size_t)(img->head_end - HEAD_START));
    if (img->head == NULL) {
        return XC_ERR_NOMEM;
    }
    memset(img->head, 0, SECTOR); /* the descriptor */
    memset(img->head + SECTOR, 0xFF, (size_t)(img->head_end - HEAD_START - SECTOR)); /* tables */
    memcpy(img->head, magic, sizeof magic - 1U);
    put32(img->head + 0x14, root->sector);
    put32(img->head + 0x18, root->table_size);
    put64(img->head + 0x1C, 0); /* creation time: none */
    memcpy(img->head + VD_TAIL_MAGIC, magic, sizeof magic - 1U);
    for (size_t k = 0; k < n; k++) {
        const node *nd = &b->nodes[k];
        write_entry(b, img->head, &b->nodes[nd->parent], nd);
    }
    return validator(b, img->validator);
}

xc_err xc_fimg_build(const xc_fimg_item *items, size_t count, xc_fimg_read_fn read, void *ctx,
                     xc_fimg **out, xc_source *src, xc_fimg_problem *problem)
{
    if (problem != NULL) {
        problem->item = SIZE_MAX;
        problem->why = "";
    }
    if (out == NULL || src == NULL || read == NULL || (items == NULL && count > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    memset(src, 0, sizeof *src);
    builder b = {NULL, NULL, NULL, count, problem};
    if (count > XC_FIMG_MAX_ITEMS) {
        return refuse(&b, XC_ERR_GAME_FOLDER, SIZE_MAX, "more than 65,536 files and folders");
    }
    xc_fimg *img = calloc(1, sizeof *img);
    b.nodes = calloc(count + 1U, sizeof *b.nodes);
    b.kids = malloc((count + 1U) * sizeof *b.kids);
    b.kid_start = malloc((count + 2U) * sizeof *b.kid_start);
    xc_err err = XC_ERR_NOMEM;
    if (img != NULL && b.nodes != NULL && b.kids != NULL && b.kid_start != NULL) {
        img->files = malloc((count + 1U) * sizeof *img->files);
        if (img->files != NULL) {
            err = build(&b, items, img);
        }
    }
    free(b.nodes);
    free(b.kids);
    free(b.kid_start);
    if (err != XC_OK) {
        xc_fimg_free(img);
        return err;
    }
    img->read = read;
    img->ctx = ctx;
    src->read_at = fimg_read;
    src->ctx = img;
    src->size = img->size;
    *out = img;
    return XC_OK;
}

void xc_fimg_free(xc_fimg *img)
{
    if (img == NULL) {
        return;
    }
    free(img->head);
    free(img->files);
    free(img);
}

const char *xc_fimg_validator(const xc_fimg *img)
{
    return img != NULL ? img->validator : "";
}

size_t xc_fimg_file_count(const xc_fimg *img)
{
    return img != NULL ? img->all_files : 0;
}

uint64_t xc_fimg_file_bytes(const xc_fimg *img)
{
    return img != NULL ? img->file_bytes : 0;
}
