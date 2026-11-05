/*
 * Map file loader: docs/formats/map.md §2 (lines), §3 (header), §4 (entries),
 * §5 (directives), §6 (limits) and §7 (errors and warnings: bad lines are
 * skipped, never the whole map).
 */
#include "xcat/map.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/archiveorg.h"
#include "xcat/url.h"

typedef struct loader {
    xc_map *m;
    const xc_map_loader *ld;
    bool is_url[XC_MAP_MAX_FILES];      /* per source */
    size_t stack[XC_MAP_MAX_DEPTH + 1]; /* sources being processed, outermost first */
    size_t depth;
    size_t listings; /* @archiveorg directives fetched so far (§6) */
    bool stopped;    /* the entry limit was reached */
    xc_url scratch;
    xc_url base;
    xc_smb_url smb_scratch; /* url_problem's */
    xc_smb_url smb_folder;  /* smb_line's */
    xc_smb_url smb_file;
} loader;

/* --- bookkeeping ------------------------------------------------------------------------ */

static void problem(loader *L, xc_map_severity sev, size_t source, size_t line, const char *fmt,
                    ...)
{
    xc_map *m = L->m;
    if (sev == XC_MAP_ERROR) {
        m->error_count++;
    }
    if (m->problem_count == XC_MAP_MAX_PROBLEMS) {
        m->problems_dropped++;
        return;
    }
    xc_map_problem *p = &m->problems[m->problem_count++];
    p->severity = sev;
    p->source = source;
    p->line = line;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(p->message, sizeof p->message, fmt, ap);
    va_end(ap);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

static xc_err add_source(loader *L, const char *location, bool is_url, size_t *index)
{
    xc_map *m = L->m;
    char *copy = dup_str(location);
    if (copy == NULL) {
        return XC_ERR_NOMEM;
    }
    m->sources[m->source_count] = copy;
    L->is_url[m->source_count] = is_url;
    *index = m->source_count++;
    return XC_OK;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != prefix[i]) {
            return false;
        }
    }
    return true;
}

static bool is_http_url(const char *s)
{
    return starts_with_ci(s, "http://") || starts_with_ci(s, "https://");
}

/* "scheme://host[:port]/target", the form used to compare and fetch URLs. */
static void canonical_url(const xc_url *u, char *out, size_t cap)
{
    if (xc_url_default_port(u)) {
        (void)snprintf(out, cap, "%s://%s%s", u->scheme == XC_URL_HTTPS ? "https" : "http", u->host,
                       u->target);
    } else {
        (void)snprintf(out, cap, "%s://%s:%u%s", u->scheme == XC_URL_HTTPS ? "https" : "http",
                       u->host, (unsigned)u->port, u->target);
    }
}

/* --- §4: entries ------------------------------------------------------------------------ */

static int hex_value(char c)
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

/* §4.1: 8HEXDIG ["@" 1*16(ALPHA / DIGIT / "-" / "_")], normalised to upper case. */
static bool parse_key(const char *key, uint32_t *title_id, char *variant)
{
    uint32_t id = 0;
    for (int i = 0; i < 8; i++) {
        int v = hex_value(key[i]);
        if (v < 0) {
            return false;
        }
        id = (id << 4U) | (uint32_t)v;
    }
    variant[0] = '\0';
    if (key[8] == '\0') {
        *title_id = id;
        return true;
    }
    if (key[8] != '@') {
        return false;
    }
    const char *v = key + 9;
    size_t n = strlen(v);
    if (n == 0 || n > XC_MAP_MAX_VARIANT) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) {
            return false;
        }
        variant[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    variant[n] = '\0';
    *title_id = id;
    return true;
}

/* §4.2: a path segment ending in .zip with more path after it ("…/game.zip/game.iso"). */
static bool inside_zip(const char *path)
{
    size_t len = strcspn(path, "?");
    for (size_t i = 0; i + 5 <= len; i++) {
        if (path[i] == '.' && (path[i + 1] | 0x20) == 'z' && (path[i + 2] | 0x20) == 'i' &&
            (path[i + 3] | 0x20) == 'p' && path[i + 4] == '/' && i + 5 < len) {
            return true;
        }
    }
    return false;
}

/* §4.2: why a URL is invalid, without echoing it (it may hold user:pass, §4.2). */
static const char *url_problem(loader *L, const char *url)
{
    if (xc_smb_is_url(url)) {
        /* §4.2, smb.md §1: a file on a NAS's shared folder. */
        const char *auth = url + 6;
        if (memchr(auth, '@', strcspn(auth, "/")) != NULL) {
            return "URL must not contain user credentials (log in in SETTINGS > NAS)";
        }
        xc_smb_url *su = &L->smb_scratch;
        if (xc_smb_url_parse(url, strlen(url), su) != XC_OK) {
            return "invalid smb:// URL (smb://<NAS>/<share>/<path to the image>)";
        }
        /* A '/' at the end names a game folder (smb.md §3.1), which may be a whole share. */
        if (su->path[0] == '\0' && url[strlen(url) - 1U] != '/') {
            return "smb:// URL names a share, not an image (use @smb for a folder)";
        }
        if (inside_zip(su->path)) {
            return "URL points to a file inside a zip, which can't be resumed; use the URL of "
                   "the .zip itself (ADR 0009)";
        }
        return NULL;
    }
    if (!is_http_url(url)) {
        return "URL must start with http://, https:// or smb://";
    }
    const char *auth = strstr(url, "://") + 3;
    size_t auth_len = strcspn(auth, "/?");
    if (memchr(auth, '@', auth_len) != NULL) {
        return "URL must not contain user credentials (use the credentials file)";
    }
    if (strchr(url, '#') != NULL) {
        return "URL must not contain '#' (write %23)";
    }
    if (inside_zip(auth + auth_len)) {
        return "URL points to a file inside a zip, which can't be resumed; use the URL of the "
               ".zip itself (ADR 0009)";
    }
    if (xc_url_parse(url, strlen(url), &L->scratch) != XC_OK) {
        return "invalid URL";
    }
    return NULL;
}

/* Fills in an entry's URL and, for a generated one, its name and listing (§5.2). */
static xc_err set_entry(xc_map_entry *e, const char *url, const xc_archiveorg_file *f)
{
    char *copy = dup_str(url);
    char *name = f != NULL ? dup_str(f->name) : NULL;
    if (copy == NULL || (f != NULL && name == NULL)) {
        free(copy);
        free(name);
        return XC_ERR_NOMEM;
    }
    free(e->url);
    free(e->name);
    e->url = copy;
    e->name = name;
    e->size = 0;
    e->has_md5 = false;
    e->has_sha1 = false;
    e->has_crc32 = false;
    memset(e->md5, 0, sizeof e->md5);
    memset(e->sha1, 0, sizeof e->sha1);
    e->crc32 = 0;
    if (f != NULL) {
        e->size = f->size;
        e->has_md5 = f->has_md5;
        e->has_sha1 = f->has_sha1;
        e->has_crc32 = f->has_crc32;
        memcpy(e->md5, f->md5, sizeof e->md5);
        memcpy(e->sha1, f->sha1, sizeof e->sha1);
        e->crc32 = f->crc32;
    }
    return XC_OK;
}

/* Adds an entry keyed by title ID and variant, or, for a file `f` of an archive.org listing,
 * by f->name (§5.2), with `url`. */
static xc_err add_entry(loader *L, size_t source, size_t line, uint32_t title_id,
                        const char *variant, const char *url, const xc_archiveorg_file *f)
{
    xc_map *m = L->m;
    for (size_t i = 0; i < m->entry_count; i++) {
        xc_map_entry *e = &m->entries[i];
        bool same = f != NULL ? e->name != NULL && xc_text_eq_ci(e->name, f->name)
                              : e->name == NULL && e->title_id == title_id &&
                                    strcmp(e->variant, variant) == 0;
        if (same) {
            /* §4.3: the last entry wins. */
            if (f != NULL) {
                problem(L, XC_MAP_WARNING, source, line,
                        "\"%s\" also defined at %s:%zu; this line wins", f->name,
                        m->sources[e->source], e->line);
            } else {
                problem(L, XC_MAP_WARNING, source, line,
                        "%08X%s%s also defined at %s:%zu; this line wins", (unsigned)title_id,
                        variant[0] != '\0' ? "@" : "", variant, m->sources[e->source], e->line);
            }
            e->source = source;
            e->line = line;
            return set_entry(e, url, f);
        }
    }
    if (m->entry_count == XC_MAP_MAX_ENTRIES) {
        problem(L, XC_MAP_ERROR, source, line, "more than %d entries; loading stopped",
                XC_MAP_MAX_ENTRIES);
        L->stopped = true;
        return XC_OK;
    }
    if (m->entry_count == m->entry_cap) {
        size_t cap = m->entry_cap == 0 ? 64 : m->entry_cap * 2;
        if (cap > XC_MAP_MAX_ENTRIES) {
            cap = XC_MAP_MAX_ENTRIES;
        }
        xc_map_entry *grown = realloc(m->entries, cap * sizeof *grown);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        m->entries = grown;
        m->entry_cap = cap;
    }
    xc_map_entry *e = &m->entries[m->entry_count];
    memset(e, 0, sizeof *e);
    xc_err err = set_entry(e, url, f);
    if (err != XC_OK) {
        return err;
    }
    m->entry_count++;
    e->title_id = f != NULL ? 0 : title_id;
    memcpy(e->variant, f != NULL ? "" : variant, f != NULL ? 1 : strlen(variant) + 1);
    e->source = source;
    e->line = line;
    return XC_OK;
}

static xc_err entry_line(loader *L, size_t source, size_t line, char *text)
{
    char *eq = strchr(text, '=');
    if (eq == NULL) {
        problem(L, XC_MAP_ERROR, source, line, "expected \"<title-id> = <url>\"");
        return XC_OK;
    }
    *eq = '\0';
    char *key = xc_text_trim(text);
    char *url = xc_text_trim(eq + 1);
    uint32_t title_id = 0;
    char variant[XC_MAP_MAX_VARIANT + 1];
    if (!parse_key(key, &title_id, variant)) {
        problem(L, XC_MAP_ERROR, source, line,
                "malformed key (8 hex digits, optionally @variant of up to 16 characters)");
        return XC_OK;
    }
    const char *why = url_problem(L, url);
    if (why != NULL) {
        problem(L, XC_MAP_ERROR, source, line, "%s", why);
        return XC_OK;
    }
    return add_entry(L, source, line, title_id, variant, url, NULL);
}

/* --- §5: directives --------------------------------------------------------------------- */

static xc_err load_file(loader *L, char *text, size_t len, size_t source, char *why,
                        size_t why_cap);

/* §5.1: where a relative include points, given the including map. */
static bool resolve_include(loader *L, size_t source, char *arg, char *out, size_t cap,
                            bool *is_url, const char **why)
{
    *why = "invalid @include location";
    if (is_http_url(arg)) {
        if (url_problem(L, arg) != NULL) {
            return false;
        }
        canonical_url(&L->scratch, out, cap);
        *is_url = true;
        return true;
    }
    /* A relative path: no absolute paths, drives, schemes or ".." (§5.1). */
    if (arg[0] == '/' || arg[0] == '\\' || strchr(arg, ':') != NULL) {
        *why = "@include paths must be relative to the map's folder";
        return false;
    }
    for (const char *seg = arg; *seg != '\0';) {
        size_t n = strcspn(seg, "/\\");
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            *why = "@include paths must not contain \"..\"";
            return false;
        }
        seg += n;
        if (*seg != '\0') {
            seg++;
        }
    }
    const char *base = L->m->sources[source];
    if (L->is_url[source]) {
        for (char *c = arg; *c != '\0'; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }
        if (xc_url_parse(base, strlen(base), &L->base) != XC_OK ||
            xc_url_resolve(&L->base, arg, strlen(arg), &L->scratch) != XC_OK) {
            return false;
        }
        canonical_url(&L->scratch, out, cap);
        *is_url = true;
        return true;
    }
    /* Local: the including map's folder, with its own separator style. */
    const char *slash = strrchr(base, '/');
    const char *back = strrchr(base, '\\');
    const char *cut = slash > back ? slash : back;
    char sep = cut != NULL ? *cut : '/';
    size_t dir_len = cut != NULL ? (size_t)(cut - base) + 1 : 0;
    size_t arg_len = strlen(arg);
    if (dir_len + arg_len + 1 > cap) {
        *why = "@include path too long";
        return false;
    }
    memcpy(out, base, dir_len);
    for (size_t i = 0; i < arg_len; i++) {
        out[dir_len + i] = arg[i] == '/' || arg[i] == '\\' ? sep : arg[i];
    }
    out[dir_len + arg_len] = '\0';
    *is_url = false;
    return true;
}

static xc_err include_line(loader *L, size_t source, size_t line, char *arg)
{
    xc_map *m = L->m;
    if (L->ld == NULL || L->ld->fetch == NULL) {
        problem(L, XC_MAP_ERROR, source, line, "@include isn't available here");
        return XC_OK;
    }
    if (L->depth + 1 > XC_MAP_MAX_DEPTH) {
        problem(L, XC_MAP_ERROR, source, line, "@include nests deeper than %d levels",
                XC_MAP_MAX_DEPTH);
        return XC_OK;
    }
    char location[XC_URL_MAX_TARGET + XC_URL_MAX_HOST + 32];
    bool is_url = false;
    const char *why = NULL;
    if (!resolve_include(L, source, arg, location, sizeof location, &is_url, &why)) {
        problem(L, XC_MAP_ERROR, source, line, "%s", why);
        return XC_OK;
    }
    for (size_t i = 0; i <= L->depth; i++) {
        const char *active = m->sources[L->stack[i]];
        bool same = is_url ? strcmp(active, location) == 0 : xc_text_eq_ci(active, location);
        if (L->is_url[L->stack[i]] == is_url && same) {
            problem(L, XC_MAP_ERROR, source, line, "@include %s: includes itself (a cycle)",
                    location);
            return XC_OK;
        }
    }
    if (m->source_count == XC_MAP_MAX_FILES) {
        problem(L, XC_MAP_ERROR, source, line, "@include %s: more than %d map files", location,
                XC_MAP_MAX_FILES);
        return XC_OK;
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = L->ld->fetch(L->ld->ctx, location, is_url, XC_MAP_MAX_FILE, &data, &len);
    if (err != XC_OK) {
        free(data);
        if (err == XC_ERR_NOMEM) {
            return err;
        }
        problem(L, XC_MAP_ERROR, source, line, "@include %s: %s", location,
                err == XC_ERR_LIMIT ? "file larger than 1 MiB" : xc_err_str(err));
        return XC_OK;
    }
    /* One more byte, so the last line can be terminated in place like the top-level map's. */
    char *grown = len <= XC_MAP_MAX_FILE ? realloc(data, len + 1) : data;
    if (grown == NULL) {
        free(data);
        return XC_ERR_NOMEM;
    }
    data = grown;
    size_t index = 0;
    err = add_source(L, location, is_url, &index);
    if (err == XC_OK) {
        L->stack[++L->depth] = index;
        char reason[120];
        err = load_file(L, data, len, index, reason, sizeof reason);
        L->depth--;
        if (err == XC_ERR_MAP) {
            problem(L, XC_MAP_ERROR, source, line, "@include %s: %s", location, reason);
            err = XC_OK;
        }
    }
    free(data);
    return err;
}

/* Turns a listing's files into entries (archiveorg.md §4, §7). */
static xc_err add_listing(loader *L, size_t source, size_t line, const char *id,
                          const xc_archiveorg_item *item)
{
    size_t bad_urls = 0;
    for (size_t i = 0; i < item->file_count && !L->stopped; i++) {
        const xc_archiveorg_file *f = &item->files[i];
        /* The URL is built from checked parts, but goes through the same rules as any
         * other (§4.2): a file in a folder named *.zip would be read as a file in a zip. */
        if (url_problem(L, f->url) != NULL) {
            bad_urls++;
            continue;
        }
        xc_err err = add_entry(L, source, line, 0, "", f->url, f);
        if (err != XC_OK) {
            return err;
        }
    }
    if (item->file_count == 0 && item->invalid == 0) {
        problem(L, XC_MAP_WARNING, source, line,
                "archive.org item %s has no .iso, .xiso or .zip files", id);
    }
    if (item->invalid > 0) {
        problem(L, XC_MAP_WARNING, source, line,
                "@archiveorg %s: skipped %zu file(s) with a bad name or size, first \"%s\"", id,
                item->invalid, item->first_invalid);
    }
    if (bad_urls > 0) {
        problem(L, XC_MAP_WARNING, source, line,
                "@archiveorg %s: skipped %zu file(s) whose URL isn't accepted", id, bad_urls);
    }
    if (item->hashes_dropped > 0) {
        problem(L, XC_MAP_WARNING, source, line, "@archiveorg %s: ignored %zu malformed hash(es)",
                id, item->hashes_dropped);
    }
    return XC_OK;
}

/* §5.2: generates entries from an archive.org item's listing (archiveorg.md). */
static xc_err archiveorg_line(loader *L, size_t source, size_t line, const char *id)
{
    char url[XC_ARCHIVEORG_MAX_ID + 40];
    if (xc_archiveorg_metadata_url(id, url, sizeof url) != XC_OK) {
        problem(L, XC_MAP_ERROR, source, line, "invalid archive.org identifier");
        return XC_OK;
    }
    if (L->ld == NULL || L->ld->fetch == NULL) {
        problem(L, XC_MAP_ERROR, source, line, "@archiveorg isn't available here");
        return XC_OK;
    }
    if (L->listings == XC_MAP_MAX_LISTINGS) {
        problem(L, XC_MAP_ERROR, source, line, "@archiveorg %s: more than %d items", id,
                XC_MAP_MAX_LISTINGS);
        return XC_OK;
    }
    L->listings++;
    char *data = NULL;
    size_t len = 0;
    xc_err err = L->ld->fetch(L->ld->ctx, url, true, XC_ARCHIVEORG_MAX_METADATA, &data, &len);
    if (err != XC_OK) {
        free(data);
        if (err == XC_ERR_NOMEM) {
            return err;
        }
        problem(L, XC_MAP_ERROR, source, line,
                "@archiveorg %s: couldn't get the item's listing: %s", id,
                err == XC_ERR_LIMIT ? "larger than 4 MiB" : xc_err_str(err));
        return XC_OK;
    }
    xc_archiveorg_item item;
    err = xc_archiveorg_parse(id, data, len, &item);
    free(data);
    if (err == XC_OK) {
        switch (item.status) {
        case XC_ARCHIVEORG_OK:
            err = add_listing(L, source, line, id, &item);
            break;
        case XC_ARCHIVEORG_NOT_METADATA:
            problem(L, XC_MAP_ERROR, source, line,
                    "archive.org didn't send the metadata of item %s", id);
            break;
        case XC_ARCHIVEORG_DARK:
            problem(L, XC_MAP_ERROR, source, line, "archive.org item %s was taken down", id);
            break;
        case XC_ARCHIVEORG_MISSING:
            problem(L, XC_MAP_ERROR, source, line,
                    "archive.org item %s doesn't exist or was removed", id);
            break;
        }
    } else if (err == XC_ERR_LIMIT) {
        problem(L, XC_MAP_ERROR, source, line,
                "@archiveorg %s: couldn't get the item's listing: larger than 4 MiB", id);
        err = XC_OK;
    }
    xc_archiveorg_free(&item);
    return err;
}

/* §5.4: generates entries from the images in a NAS folder (smb.md §3). */
static xc_err smb_line(loader *L, size_t source, size_t line, const char *arg)
{
    xc_map *m = L->m;
    xc_smb_url *folder = &L->smb_folder;
    if (!xc_smb_is_url(arg) || xc_smb_url_parse(arg, strlen(arg), folder) != XC_OK) {
        problem(L, XC_MAP_ERROR, source, line,
                "invalid @smb folder (smb://<NAS>/<share>[/<folder>], no user name)");
        return XC_OK;
    }
    char where[XC_SMB_MAX_URL + 64];
    if (xc_smb_url_format(folder, where, sizeof where) != XC_OK) {
        problem(L, XC_MAP_ERROR, source, line, "@smb folder address too long");
        return XC_OK;
    }
    if (m->smb_folder_count < XC_MAP_MAX_LISTINGS) {
        char *copy = dup_str(where);
        if (copy == NULL) {
            return XC_ERR_NOMEM;
        }
        m->smb_folders[m->smb_folder_count++] = copy;
    }
    if (L->ld == NULL || L->ld->list_smb == NULL) {
        problem(L, XC_MAP_ERROR, source, line, "@smb isn't available here");
        return XC_OK;
    }
    if (L->listings == XC_MAP_MAX_LISTINGS) {
        problem(L, XC_MAP_ERROR, source, line, "@smb %s: more than %d listings", where,
                XC_MAP_MAX_LISTINGS);
        return XC_OK;
    }
    L->listings++;
    xc_smb_listing listing;
    memset(&listing, 0, sizeof listing);
    xc_err err = L->ld->list_smb(L->ld->ctx, folder, &listing);
    if (err != XC_OK) {
        xc_smb_listing_free(&listing);
        if (err == XC_ERR_NOMEM) {
            return err;
        }
        problem(L, XC_MAP_ERROR, source, line, "@smb %s: %s", where, xc_err_str(err));
        return XC_OK;
    }
    size_t images = 0;
    size_t bad = 0;
    xc_smb_url *file = &L->smb_file;
    char url[XC_SMB_MAX_URL + 64];
    for (size_t i = 0; i < listing.count && !L->stopped; i++) {
        const xc_smb_listed *f = &listing.files[i];
        xc_archiveorg_file gen;
        memset(&gen, 0, sizeof gen);
        char name[XC_SMB_MAX_NAME + 1];
        bool zip = false;
        if (f->game) {
            /* A game folder (smb.md §3.1): named after itself, its address ends in '/'. */
            const char *in = f->path[0] != '\0'        ? f->path
                             : folder->path[0] != '\0' ? folder->path
                                                       : folder->share;
            const char *last = strrchr(in, '/');
            last = last != NULL ? last + 1 : in;
            size_t n = strlen(last);
            if (n == 0 || n >= sizeof name) {
                bad++;
                continue;
            }
            memcpy(name, last, n + 1U);
        } else if (!xc_smb_image_name(f->path, name, sizeof name, &zip)) {
            continue; /* not an image: skipped without a word (smb.md §3) */
        }
        if (f->path[0] == '\0') {
            *file = *folder;
        } else if (xc_smb_url_child(folder, f->path, file) != XC_OK) {
            bad++;
            continue;
        }
        if (xc_smb_url_format(file, url, sizeof url - 1U) != XC_OK) {
            bad++;
            continue;
        }
        size_t used = strlen(url);
        if (f->game && url[used - 1U] != '/') {
            url[used++] = '/';
            url[used] = '\0';
        }
        if (url_problem(L, url) != NULL) {
            bad++;
            continue;
        }
        gen.name = name;
        gen.size = f->size;
        gen.zip = zip;
        err = add_entry(L, source, line, 0, "", url, &gen);
        if (err != XC_OK) {
            xc_smb_listing_free(&listing);
            return err;
        }
        images++;
    }
    if (images == 0 && bad == 0) {
        problem(L, XC_MAP_WARNING, source, line,
                "@smb %s has no .iso, .xiso or .zip files and no game folders", where);
    }
    if (bad > 0) {
        problem(L, XC_MAP_WARNING, source, line,
                "@smb %s: skipped %zu game(s) whose name or path isn't accepted", where, bad);
    }
    if (listing.truncated) {
        problem(L, XC_MAP_WARNING, source, line, "@smb %s: more than %d files; the rest skipped",
                where, XC_SMB_MAX_LISTED);
    }
    xc_smb_listing_free(&listing);
    return XC_OK;
}

static xc_err directive_line(loader *L, size_t source, size_t line, char *text)
{
    char *name = text + 1;
    size_t n = 0;
    while (name[n] >= 'a' && name[n] <= 'z') {
        n++;
    }
    if (n == 0 || (name[n] != '\0' && !xc_text_space(name[n]))) {
        problem(L, XC_MAP_ERROR, source, line, "malformed directive");
        return XC_OK;
    }
    char *arg = xc_text_trim(name + n);
    name[n] = '\0';
    if (strcmp(name, "include") == 0 || strcmp(name, "archiveorg") == 0 ||
        strcmp(name, "smb") == 0) {
        if (arg[0] == '\0') {
            problem(L, XC_MAP_ERROR, source, line, "@%s needs an argument", name);
            return XC_OK;
        }
        return name[0] == 'i'   ? include_line(L, source, line, arg)
               : name[0] == 's' ? smb_line(L, source, line, arg)
                                : archiveorg_line(L, source, line, arg);
    }
    problem(L, XC_MAP_WARNING, source, line, "unknown directive @%s skipped", name);
    return XC_OK;
}

/* --- one file --------------------------------------------------------------------------- */

/*
 * Processes one map. XC_ERR_MAP if the file itself is rejected (why[] says why);
 * line problems are recorded and the file still counts as loaded.
 */
static xc_err load_file(loader *L, char *text, size_t len, size_t source, char *why, size_t why_cap)
{
    if (len > XC_MAP_MAX_FILE) {
        (void)snprintf(why, why_cap, "file larger than 1 MiB");
        return XC_ERR_MAP;
    }
    if (len >= 3 && (unsigned char)text[0] == 0xEFU && (unsigned char)text[1] == 0xBBU &&
        (unsigned char)text[2] == 0xBFU) {
        text += 3;
        len -= 3;
    }
    bool header_seen = false;
    size_t line = 0;
    size_t pos = 0;
    while (pos < len && !L->stopped) {
        line++;
        char *start = text + pos;
        char *nl = memchr(start, '\n', len - pos);
        size_t raw = nl != NULL ? (size_t)(nl - start) : len - pos;
        pos += raw + (nl != NULL ? 1U : 0U);
        if (raw > 0 && start[raw - 1] == '\r') {
            raw--;
        }
        size_t bad = 0;
        const char *line_problem = NULL;
        if (raw > XC_MAP_MAX_LINE) {
            line_problem = "line longer than 2048 bytes";
        } else if (!xc_text_utf8((const unsigned char *)start, raw, &bad) ||
                   memchr(start, '\0', raw) != NULL) {
            line_problem = "not valid UTF-8 text";
        }
        if (line_problem != NULL) {
            if (!header_seen) {
                (void)snprintf(why, why_cap, "line %zu: %s", line, line_problem);
                return XC_ERR_MAP;
            }
            problem(L, XC_MAP_ERROR, source, line, "%s", line_problem);
            continue;
        }
        start[raw] = '\0'; /* the line ending, or the byte after the last line */
        char *t = xc_text_trim(start);
        if (t[0] == '\0') {
            continue;
        }
        if (!header_seen) {
            unsigned long major = 0;
            switch (xc_text_check_header(t, "xcat-map", XC_MAP_VERSION_MAJOR, &major, NULL)) {
            case XC_TEXT_HEADER_OK:
                header_seen = true;
                continue;
            case XC_TEXT_HEADER_NEWER:
                (void)snprintf(why, why_cap,
                               "this map needs a newer xiso-cartographer (map format v%lu)", major);
                return XC_ERR_MAP;
            case XC_TEXT_HEADER_OLDER:
                (void)snprintf(why, why_cap, "unsupported map format v%lu", major);
                return XC_ERR_MAP;
            case XC_TEXT_HEADER_MISSING:
                break;
            }
            (void)snprintf(why, why_cap, "not an xcat-map file (missing \"# xcat-map v1\" header)");
            return XC_ERR_MAP;
        }
        xc_err err = XC_OK;
        if (t[0] == '#') {
            continue;
        }
        err = t[0] == '@' ? directive_line(L, source, line, t) : entry_line(L, source, line, t);
        if (err != XC_OK) {
            return err;
        }
    }
    if (!header_seen) {
        (void)snprintf(why, why_cap, "empty file (missing \"# xcat-map v1\" header)");
        return XC_ERR_MAP;
    }
    return XC_OK;
}

/* --- API -------------------------------------------------------------------------------- */

xc_err xc_map_load(const char *data, size_t len, const char *location, const xc_map_loader *ld,
                   xc_map *map)
{
    if (map == NULL || location == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    memset(map, 0, sizeof *map);
    map->problems = malloc(sizeof *map->problems * XC_MAP_MAX_PROBLEMS);
    map->sources = (char **)calloc(XC_MAP_MAX_FILES, sizeof *map->sources);
    map->smb_folders = (char **)calloc(XC_MAP_MAX_LISTINGS, sizeof *map->smb_folders);
    loader *L = calloc(1, sizeof *L);
    /* One extra byte so the last line can be terminated in place. */
    char *text = malloc((len > XC_MAP_MAX_FILE ? 0 : len) + 1);
    xc_err err = XC_OK;
    if (map->problems == NULL || map->sources == NULL || map->smb_folders == NULL || L == NULL ||
        text == NULL) {
        err = XC_ERR_NOMEM;
    }
    size_t root = 0;
    if (err == XC_OK) {
        L->m = map;
        L->ld = ld;
        err = add_source(L, location, is_http_url(location), &root);
    }
    if (err == XC_OK) {
        char why[160];
        if (len <= XC_MAP_MAX_FILE && len > 0) {
            memcpy(text, data, len);
        }
        L->stack[0] = root;
        err = load_file(L, text, len, root, why, sizeof why);
        if (err == XC_ERR_MAP) {
            /* §7: a rejected top-level file has no entries at all. */
            for (size_t i = 0; i < map->entry_count; i++) {
                free(map->entries[i].url);
                free(map->entries[i].name);
            }
            map->entry_count = 0;
            problem(L, XC_MAP_ERROR, root, 0, "%s", why);
        }
    }
    free(text);
    free(L);
    return err;
}

void xc_map_free(xc_map *map)
{
    if (map == NULL) {
        return;
    }
    for (size_t i = 0; i < map->entry_count; i++) {
        free(map->entries[i].url);
        free(map->entries[i].name);
    }
    free(map->entries);
    free(map->problems);
    if (map->sources != NULL) {
        for (size_t i = 0; i < map->source_count; i++) {
            free(map->sources[i]);
        }
    }
    free((void *)map->sources);
    if (map->smb_folders != NULL) {
        for (size_t i = 0; i < map->smb_folder_count; i++) {
            free(map->smb_folders[i]);
        }
    }
    free((void *)map->smb_folders);
    memset(map, 0, sizeof *map);
}

const xc_map_entry *xc_map_find(const xc_map *map, uint32_t title_id, const char *variant)
{
    if (map == NULL) {
        return NULL;
    }
    char want[XC_MAP_MAX_VARIANT + 1] = "";
    if (variant != NULL) {
        size_t n = strlen(variant);
        if (n > XC_MAP_MAX_VARIANT) {
            return NULL;
        }
        for (size_t i = 0; i <= n; i++) {
            char c = variant[i];
            want[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        }
    }
    const xc_map_entry *fallback = NULL;
    for (size_t i = 0; i < map->entry_count; i++) {
        const xc_map_entry *e = &map->entries[i];
        if (e->name != NULL || e->title_id != title_id) {
            continue;
        }
        if (strcmp(e->variant, want) == 0) {
            return e;
        }
        if (e->variant[0] == '\0') {
            fallback = e;
        }
    }
    return fallback;
}

const xc_map_entry *xc_map_find_name(const xc_map *map, const char *name)
{
    if (map == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < map->entry_count; i++) {
        const xc_map_entry *e = &map->entries[i];
        if (e->name != NULL && xc_text_eq_ci(e->name, name)) {
            return e;
        }
    }
    return NULL;
}

typedef enum host_use { HOST_ENTRY, HOST_MAP, HOST_FOLDER } host_use;

/* Counts u's server in out[0, *n), adding it if it's new and there's room. */
static void count_host(const xc_url *u, host_use use, xc_map_host *out, size_t max, size_t *n)
{
    for (size_t i = 0; i < *n && i < max; i++) {
        xc_map_host *h = &out[i];
        if (h->scheme == u->scheme && h->port == u->port && strcmp(h->host, u->host) == 0) {
            h->entries += use == HOST_ENTRY ? 1U : 0U;
            h->maps += use == HOST_MAP ? 1U : 0U;
            h->folders += use == HOST_FOLDER ? 1U : 0U;
            return;
        }
    }
    if (*n < max) {
        xc_map_host *h = &out[*n];
        memset(h, 0, sizeof *h);
        h->scheme = u->scheme;
        h->port = u->port;
        h->is_ipv4 = u->is_ipv4;
        memcpy(h->host, u->host, sizeof h->host);
        h->entries = use == HOST_ENTRY ? 1U : 0U;
        h->maps = use == HOST_MAP ? 1U : 0U;
        h->folders = use == HOST_FOLDER ? 1U : 0U;
        (*n)++;
    }
}

/* An smb:// URL's server as an xc_url of scheme XC_URL_SMB, for count_host. */
static bool smb_host(const char *text, xc_url *u)
{
    xc_smb_url su;
    if (!xc_smb_is_url(text) || xc_smb_url_parse(text, strlen(text), &su) != XC_OK) {
        return false;
    }
    memset(u, 0, sizeof *u);
    u->scheme = XC_URL_SMB;
    u->port = su.port;
    u->is_ipv4 = su.is_ipv4;
    memcpy(u->host, su.host, sizeof su.host);
    return true;
}

size_t xc_map_hosts(const xc_map *map, xc_map_host *out, size_t max)
{
    if (map == NULL || (out == NULL && max > 0)) {
        return 0;
    }
    xc_url u;
    size_t n = 0;
    for (size_t i = 0; i < map->entry_count; i++) {
        const char *url = map->entries[i].url;
        if (url != NULL && (xc_url_parse(url, strlen(url), &u) == XC_OK || smb_host(url, &u))) {
            count_host(&u, HOST_ENTRY, out, max, &n);
        }
    }
    for (size_t i = 1; i < map->source_count; i++) {
        const char *loc = map->sources[i];
        if (loc != NULL && is_http_url(loc) && xc_url_parse(loc, strlen(loc), &u) == XC_OK) {
            count_host(&u, HOST_MAP, out, max, &n);
        }
    }
    for (size_t i = 0; i < map->smb_folder_count; i++) {
        if (smb_host(map->smb_folders[i], &u)) {
            count_host(&u, HOST_FOLDER, out, max, &n);
        }
    }
    return n;
}
