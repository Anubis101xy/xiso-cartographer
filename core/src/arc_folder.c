/*
 * A game or app folder inside an archive (xcat/arc_folder.h). Implements
 * docs/formats/folder-image.md §7; the image itself is the folder image's (§2-§5).
 */
#include "xcat/arc_folder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xc_arc_folder {
    xc_fimg *img;
    size_t *member_of; /* image item -> archive member; SIZE_MAX for a folder made up (§7.3) */
    size_t count;      /* items */
    char *root;
    xc_arc_read_fn read;
    void *ctx;
};

/* One item while building: an owned path below the game folder. */
typedef struct pending {
    char *path;
    size_t member; /* SIZE_MAX: a folder no member names */
    uint64_t size;
    uint64_t order;
    uint64_t stamp;
    bool dir;
} pending;

typedef struct builder {
    pending *items;
    size_t count;
    size_t cap;
    char *why;
    size_t why_cap;
} builder;

static xc_err refuse(builder *b, xc_err err, const char *path, const char *what)
{
    if (b->why != NULL && b->why_cap > 0) {
        int n = snprintf(b->why, b->why_cap, "%s%s%s", path, path[0] != '\0' ? ": " : "", what);
        if (n < 0) {
            b->why[0] = '\0';
        }
    }
    return err;
}

static bool same_ci(const char *a, size_t n, const char *b)
{
    if (strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

bool xc_arc_is_default_xbe(const char *path)
{
    const char *last = strrchr(path, '/');
    last = last != NULL ? last + 1 : path;
    return same_ci(last, strlen(last), "default.xbe");
}

/* §7.1: the member's path with '\' as '/', without a leading "./" or '/', or a trailing '/'.
   NULL for nothing left; *bad for a ".." name, which no game has. Malloc'd. */
static char *normal_path(const char *in, bool *bad)
{
    *bad = false;
    size_t n = strlen(in);
    char *p = malloc(n + 1U);
    if (p == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = in[i] == '\\' ? '/' : in[i];
    }
    p[o] = '\0';
    size_t start = 0;
    while (p[start] == '/' || (p[start] == '.' && p[start + 1U] == '/')) {
        start += p[start] == '/' ? 1U : 2U;
    }
    while (o > start && p[o - 1U] == '/') {
        p[--o] = '\0';
    }
    memmove(p, p + start, o - start + 1U);
    if (p[0] == '\0') {
        free(p);
        return NULL;
    }
    for (const char *c = p; *c != '\0';) {
        size_t len = strcspn(c, "/");
        if (len == 2 && c[0] == '.' && c[1] == '.') {
            *bad = true;
        }
        c += len + (c[len] == '/' ? 1U : 0U);
    }
    return p;
}

/* §7.2: a name that's not the game's: '.' first (.DS_Store, ._x), or macOS's __MACOSX. */
static bool left_out(const char *path)
{
    for (const char *c = path; *c != '\0';) {
        size_t len = strcspn(c, "/");
        if (c[0] == '.' || same_ci(c, len, "__macosx")) {
            return true;
        }
        c += len + (c[len] == '/' ? 1U : 0U);
    }
    return false;
}

static unsigned depth_of(const char *path)
{
    unsigned d = 0;
    for (; *path != '\0'; path++) {
        d += *path == '/';
    }
    return d;
}

static xc_err add(builder *b, pending p)
{
    if (b->count == b->cap) {
        size_t cap = b->cap == 0 ? 64 : b->cap * 2U;
        pending *grown = realloc(b->items, cap * sizeof *grown);
        if (grown == NULL) {
            free(p.path);
            return XC_ERR_NOMEM;
        }
        b->items = grown;
        b->cap = cap;
    }
    b->items[b->count++] = p;
    return XC_OK;
}

static int by_path(const void *a, const void *b)
{
    int c = strcmp(((const pending *)a)->path, ((const pending *)b)->path);
    if (c != 0) {
        return c;
    }
    /* Ties: a member before a made-up folder, so the member is the one kept. */
    return (((const pending *)a)->member == SIZE_MAX) - (((const pending *)b)->member == SIZE_MAX);
}

/* §7.3: every folder a path is in is an item, even when the archive has no member for it. */
static xc_err add_parents(builder *b)
{
    size_t n = b->count;
    for (size_t i = 0; i < n; i++) {
        const char *path = b->items[i].path;
        for (const char *s = strchr(path, '/'); s != NULL; s = strchr(s + 1, '/')) {
            size_t len = (size_t)(s - path);
            char *dir = malloc(len + 1U);
            if (dir == NULL) {
                return XC_ERR_NOMEM;
            }
            memcpy(dir, path, len);
            dir[len] = '\0';
            xc_err err = add(b, (pending){dir, SIZE_MAX, 0, 0, 0, true});
            if (err != XC_OK) {
                return err;
            }
            path = b->items[i].path; /* add() may have moved the array */
            s = path + len;
        }
    }
    qsort(b->items, b->count, sizeof *b->items, by_path);
    /* One item per path: folders named twice are one; a file named twice, or a name that's
       a file and a folder, can't be installed. */
    size_t o = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (o > 0 && strcmp(b->items[o - 1U].path, b->items[i].path) == 0) {
            if (!b->items[i].dir || !b->items[o - 1U].dir) {
                return refuse(b, XC_ERR_GAME_FOLDER, b->items[i].path,
                              "the archive has it twice, or as a file and a folder");
            }
            free(b->items[i].path);
            continue;
        }
        b->items[o++] = b->items[i];
    }
    b->count = o;
    return XC_OK;
}

static xc_err folder_read(void *ctx, size_t item, uint64_t offset, void *buf, size_t len)
{
    xc_arc_folder *f = ctx;
    size_t m = f->member_of[item];
    if (m == SIZE_MAX) {
        return XC_ERR_INVALID_ARG; /* a made-up folder has no bytes */
    }
    return f->read(f->ctx, m, offset, buf, len);
}

static void free_items(builder *b)
{
    for (size_t i = 0; i < b->count; i++) {
        free(b->items[i].path);
    }
    free(b->items);
    b->items = NULL;
    b->count = 0;
}

/* §7.1: the game folder, the folder of the shallowest default.xbe. */
static xc_err find_root(builder *b, const xc_arc_member *members, size_t count, char **root)
{
    *root = NULL;
    const char *best = NULL;
    unsigned best_depth = 0;
    bool tie = false;
    for (size_t i = 0; i < count; i++) {
        if (members[i].dir || members[i].path == NULL) {
            continue;
        }
        bool bad = false;
        char *p = normal_path(members[i].path, &bad);
        if (p == NULL) {
            continue;
        }
        if (!bad && !left_out(p) && xc_arc_is_default_xbe(p)) {
            unsigned d = depth_of(p);
            if (best == NULL || d < best_depth) {
                free((char *)best);
                best = p;
                best_depth = d;
                tie = false;
                continue;
            }
            tie = tie || d == best_depth;
        }
        free(p);
    }
    if (best == NULL) {
        return XC_ERR_ZIP_MEMBER;
    }
    if (tie) {
        xc_err err = refuse(b, XC_ERR_GAME_FOLDER, "",
                            "the archive holds several games (more than one default.xbe at the "
                            "top of its folders)");
        free((char *)best);
        return err;
    }
    char *slash = strrchr(best, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        ((char *)best)[0] = '\0';
    }
    *root = (char *)best;
    return XC_OK;
}

xc_err xc_arc_folder_build(const xc_arc_member *members, size_t count, xc_arc_read_fn read,
                           void *ctx, xc_arc_folder **out, xc_source *image, char *why,
                           size_t why_cap)
{
    if (why != NULL && why_cap > 0) {
        why[0] = '\0';
    }
    if (out == NULL || image == NULL || read == NULL || (members == NULL && count > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    builder b = {NULL, 0, 0, why, why_cap};
    char *root = NULL;
    xc_err err = find_root(&b, members, count, &root);
    if (err != XC_OK) {
        return err;
    }
    size_t root_len = strlen(root);
    /* §7.2: what's in the game folder. */
    for (size_t i = 0; err == XC_OK && i < count; i++) {
        if (members[i].path == NULL) {
            continue;
        }
        bool bad = false;
        char *p = normal_path(members[i].path, &bad);
        if (p == NULL) {
            continue;
        }
        const char *rel = p;
        if (root_len > 0) {
            if (strncmp(p, root, root_len) != 0 || p[root_len] != '/') {
                free(p);
                continue; /* outside the game folder (or the folder itself) */
            }
            rel = p + root_len + 1U;
        }
        if (bad) {
            err = refuse(&b, XC_ERR_GAME_FOLDER, p, "a \"..\" in its path");
            free(p);
            break;
        }
        if (left_out(rel)) {
            free(p);
            continue;
        }
        memmove(p, rel, strlen(rel) + 1U);
        err = add(&b, (pending){p, i, members[i].dir ? 0 : members[i].size, members[i].order,
                                members[i].stamp, members[i].dir});
    }
    if (err == XC_OK) {
        err = add_parents(&b);
    }
    xc_arc_folder *f = NULL;
    xc_fimg_item *items = NULL;
    if (err == XC_OK) {
        f = calloc(1, sizeof *f);
        items = malloc((b.count + 1U) * sizeof *items);
        if (f != NULL) {
            f->member_of = malloc((b.count + 1U) * sizeof *f->member_of);
        }
        if (f == NULL || items == NULL || f->member_of == NULL) {
            err = XC_ERR_NOMEM;
        }
    }
    if (err == XC_OK) {
        for (size_t i = 0; i < b.count; i++) {
            const pending *p = &b.items[i];
            items[i] = (xc_fimg_item){p->path, p->size, p->stamp, p->dir, p->order};
            f->member_of[i] = p->member;
        }
        f->count = b.count;
        f->read = read;
        f->ctx = ctx;
        f->root = root;
        root = NULL;
        xc_fimg_problem problem;
        err = xc_fimg_build(items, b.count, folder_read, f, &f->img, image, &problem);
        if (err != XC_OK) {
            const char *path = problem.item < b.count ? b.items[problem.item].path : "";
            err = refuse(&b, err == XC_ERR_NOMEM ? err : XC_ERR_GAME_FOLDER, path, problem.why);
        }
    }
    free(items);
    free_items(&b);
    free(root);
    if (err != XC_OK) {
        xc_arc_folder_free(f);
        memset(image, 0, sizeof *image);
        return err;
    }
    *out = f;
    return XC_OK;
}

void xc_arc_folder_free(xc_arc_folder *f)
{
    if (f == NULL) {
        return;
    }
    xc_fimg_free(f->img);
    free(f->member_of);
    free(f->root);
    free(f);
}

bool xc_arc_folder_uses(const xc_arc_folder *f, size_t member)
{
    if (f == NULL) {
        return false;
    }
    for (size_t i = 0; i < f->count; i++) {
        if (f->member_of[i] == member) {
            return true;
        }
    }
    return false;
}

const char *xc_arc_folder_root(const xc_arc_folder *f)
{
    return f != NULL && f->root != NULL ? f->root : "";
}

size_t xc_arc_folder_files(const xc_arc_folder *f)
{
    return f != NULL ? xc_fimg_file_count(f->img) : 0;
}

uint64_t xc_arc_folder_bytes(const xc_arc_folder *f)
{
    return f != NULL ? xc_fimg_file_bytes(f->img) : 0;
}

const char *xc_arc_folder_validator(const xc_arc_folder *f)
{
    return f != NULL ? xc_fimg_validator(f->img) : "";
}
