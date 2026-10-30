/* Editing a map's sources (xcat/map_edit.h, docs/formats/map.md §9). */
#include "xcat/map_edit.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/archiveorg.h"
#include "xcat/map.h"

/* One line of data: [start, end) with its line ending, and its text, trimmed. */
typedef struct line_at {
    size_t start, end;
    const char *text;
    size_t text_len;
} line_at;

static bool next_line(const char *data, size_t len, size_t pos, line_at *l)
{
    if (pos >= len) {
        return false;
    }
    const char *nl = memchr(data + pos, '\n', len - pos);
    l->start = pos;
    l->end = nl != NULL ? (size_t)(nl - data) + 1U : len;
    size_t a = pos;
    size_t b = nl != NULL ? (size_t)(nl - data) : len;
    while (a < b && (data[a] == ' ' || data[a] == '\t')) {
        a++;
    }
    while (b > a && (data[b - 1] == ' ' || data[b - 1] == '\t' || data[b - 1] == '\r')) {
        b--;
    }
    l->text = data + a;
    l->text_len = b - a;
    return true;
}

/* Where the lines start: after a UTF-8 byte order mark (§2.1). */
static size_t first_pos(const char *data, size_t len)
{
    return len >= 3 && (unsigned char)data[0] == 0xEFU && (unsigned char)data[1] == 0xBBU &&
                   (unsigned char)data[2] == 0xBFU
               ? 3U
               : 0U;
}

/* §3: the first line that isn't blank is "# xcat-map v1[.n]". Sets *after to the position
   after it. */
static bool has_header(const char *data, size_t len, size_t *after, size_t *line_no)
{
    line_at l;
    size_t n = 0;
    for (size_t pos = first_pos(data, len); next_line(data, len, pos, &l); pos = l.end) {
        n++;
        if (l.text_len == 0) {
            continue;
        }
        char buf[64];
        if (l.text_len >= sizeof buf) {
            return false;
        }
        memcpy(buf, l.text, l.text_len);
        buf[l.text_len] = '\0';
        unsigned long major = 0;
        if (xc_text_check_header(buf, "xcat-map", XC_MAP_VERSION_MAJOR, &major, NULL) !=
            XC_TEXT_HEADER_OK) {
            return false;
        }
        *after = l.end;
        *line_no = n;
        return true;
    }
    return false;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* What a source line is, and its parts (§4, §5). */
static void classify(const line_at *l, size_t line_no, xc_map_line *m)
{
    memset(m, 0, sizeof *m);
    m->line = line_no;
    m->text = l->text;
    m->text_len = l->text_len;
    const char *t = l->text;
    size_t n = l->text_len;
    if (t[0] == '@') {
        size_t name = 1;
        while (name < n && t[name] >= 'a' && t[name] <= 'z') {
            name++;
        }
        size_t a = name;
        while (a < n && is_space(t[a])) {
            a++;
        }
        bool parted = a > name || a == n;
        m->arg = t + a;
        m->arg_len = n - a;
        if (parted && name - 1 == 7 && memcmp(t + 1, "include", 7) == 0) {
            m->kind = XC_MAP_LINE_INCLUDE;
        } else if (parted && name - 1 == 10 && memcmp(t + 1, "archiveorg", 10) == 0) {
            m->kind = XC_MAP_LINE_ARCHIVEORG;
        } else if (parted && name - 1 == 3 && memcmp(t + 1, "smb", 3) == 0) {
            m->kind = XC_MAP_LINE_SMB;
        } else {
            m->kind = XC_MAP_LINE_DIRECTIVE;
        }
        return;
    }
    m->kind = XC_MAP_LINE_ENTRY;
    const char *eq = memchr(t, '=', n);
    size_t k = eq != NULL ? (size_t)(eq - t) : n;
    m->key_len = k;
    while (m->key_len > 0 && is_space(t[m->key_len - 1])) {
        m->key_len--;
    }
    if (eq != NULL) {
        size_t a = k + 1;
        while (a < n && is_space(t[a])) {
            a++;
        }
        m->arg = t + a;
        m->arg_len = n - a;
    } else {
        m->arg = t + n;
    }
}

xc_err xc_map_lines(const char *data, size_t len, xc_map_line *out, size_t max, size_t *count)
{
    if (count == NULL || (data == NULL && len > 0) || (out == NULL && max > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *count = 0;
    if (len > XC_MAP_MAX_FILE) {
        return XC_ERR_LIMIT;
    }
    size_t pos = 0;
    size_t line_no = 0;
    if (!has_header(data, len, &pos, &line_no)) {
        return XC_ERR_MAP;
    }
    line_at l;
    for (; next_line(data, len, pos, &l); pos = l.end) {
        line_no++;
        if (l.text_len == 0 || l.text[0] == '#') {
            continue;
        }
        if (*count < max) {
            classify(&l, line_no, &out[*count]);
        }
        (*count)++;
    }
    return XC_OK;
}

static void say(char *why, size_t cap, const char *text)
{
    if (why != NULL && cap > 0) {
        (void)snprintf(why, cap, "%s", text);
    }
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix != '\0'; s++, prefix++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s;
        if (c != *prefix) {
            return false;
        }
    }
    return true;
}

/* An included map, served while a new @include line is checked: just a header. */
static xc_err empty_map(void *ctx, const char *location, bool is_url, size_t max_len, char **data,
                        size_t *len)
{
    (void)ctx;
    (void)location;
    (void)is_url;
    (void)max_len;
    static const char header[] = "# xcat-map v1\n";
    *data = malloc(sizeof header);
    if (*data == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(*data, header, sizeof header);
    *len = sizeof header - 1U;
    return XC_OK;
}

/* An @smb folder seen as empty: checking a line never lists the NAS. */
static xc_err empty_listing(void *ctx, const xc_smb_url *folder, xc_smb_listing *out)
{
    (void)ctx;
    (void)folder;
    (void)out;
    return XC_OK;
}

/* Loads `line` alone in a map, as the reader would: XC_OK if it's a valid source, else
   XC_ERR_MAP and why. */
static xc_err check_alone(const char *line, char *why, size_t why_cap)
{
    static const char header[] = "# xcat-map v1\n";
    size_t n = strlen(line);
    char *text = malloc(sizeof header + n + 1U);
    if (text == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(text, header, sizeof header); /* NULs included: each is replaced */
    memcpy(text + sizeof header - 1U, line, n + 1U);
    text[sizeof header - 1U + n] = '\n';
    xc_map map;
    xc_map_loader loader = {empty_map, NULL, empty_listing};
    xc_err err = xc_map_load(text, sizeof header + n, "map.txt", &loader, &map);
    free(text);
    if (err == XC_ERR_NOMEM) {
        xc_map_free(&map);
        return err;
    }
    for (size_t i = 0; i < map.problem_count; i++) {
        if (map.problems[i].severity == XC_MAP_ERROR) {
            say(why, why_cap, map.problems[i].message);
            xc_map_free(&map);
            return XC_ERR_MAP;
        }
    }
    xc_map_free(&map);
    return err == XC_OK ? XC_OK : XC_ERR_MAP;
}

xc_err xc_map_new_line(xc_map_line_kind kind, const char *key, const char *url, char *out,
                       size_t cap, char *why, size_t why_cap)
{
    if (url == NULL || out == NULL || cap == 0 || (kind == XC_MAP_LINE_ENTRY && key == NULL)) {
        return XC_ERR_INVALID_ARG;
    }
    say(why, why_cap, "");
    out[0] = '\0';
    int n = 0;
    if (kind == XC_MAP_LINE_ARCHIVEORG) {
        if (!xc_archiveorg_valid_id(url)) {
            say(why, why_cap,
                "an archive.org identifier is 1 to 100 letters, digits, dots, dashes or _");
            return XC_ERR_MAP;
        }
        n = snprintf(out, cap, "@archiveorg %s", url);
    } else if (kind == XC_MAP_LINE_SMB) {
        xc_smb_url u;
        if (!xc_smb_is_url(url) || xc_smb_url_parse(url, strlen(url), &u) != XC_OK) {
            say(why, why_cap, "a NAS folder's address is smb://<NAS>/<share>[/<folder>]");
            return XC_ERR_MAP;
        }
        char canon[XC_SMB_MAX_URL + 64];
        if (xc_smb_url_format(&u, canon, sizeof canon) != XC_OK) {
            say(why, why_cap, "the line is too long");
            return XC_ERR_LIMIT;
        }
        n = snprintf(out, cap, "@smb %s", canon);
    } else if (kind == XC_MAP_LINE_INCLUDE) {
        if (!starts_with_ci(url, "http://") && !starts_with_ci(url, "https://")) {
            say(why, why_cap, "a map's address starts with http:// or https://");
            return XC_ERR_MAP;
        }
        n = snprintf(out, cap, "@include %s", url);
    } else if (kind == XC_MAP_LINE_ENTRY) {
        char k[32];
        size_t kl = strlen(key);
        if (kl == 0 || kl >= sizeof k) {
            say(why, why_cap,
                "a title ID is 8 hex digits, like 4D530004, then @ and a variant "
                "if there is one");
            return XC_ERR_MAP;
        }
        for (size_t i = 0; i <= kl; i++) {
            char c = key[i];
            k[i] = i < 8 && c >= 'a' && c <= 'f' ? (char)(c - 'a' + 'A') : c;
        }
        n = snprintf(out, cap, "%s = %s", k, url);
    } else {
        return XC_ERR_INVALID_ARG;
    }
    if (n < 0 || (size_t)n >= cap || (size_t)n > XC_MAP_MAX_LINE) {
        out[0] = '\0';
        say(why, why_cap, "the line is too long");
        return XC_ERR_LIMIT;
    }
    for (const char *c = out; *c != '\0'; c++) {
        if ((unsigned char)*c < 0x20U || *c == 0x7F) {
            out[0] = '\0';
            say(why, why_cap, "it has a control character");
            return XC_ERR_MAP; /* a line break would make two lines */
        }
    }
    if (kind == XC_MAP_LINE_ARCHIVEORG) {
        return XC_OK; /* checking it would fetch the item; the identifier is all there is */
    }
    xc_err err = check_alone(out, why, why_cap);
    if (err != XC_OK) {
        out[0] = '\0';
    }
    return err;
}

static xc_err finish(char *buf, size_t n, char **out, size_t *out_len)
{
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return XC_OK;
}

xc_err xc_map_add_line(const char *data, size_t len, const char *line, char **out, size_t *out_len)
{
    if (line == NULL || out == NULL || out_len == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    size_t after = 0;
    size_t header_line = 0;
    if (len > 0 && !has_header(data, len, &after, &header_line)) {
        return XC_ERR_MAP;
    }
    static const char header[] = "# xcat-map v1\n\n";
    size_t n = strlen(line);
    size_t total = (len > 0 ? len + 1U : sizeof header - 1U) + n + 1U;
    if (total > XC_MAP_MAX_FILE) {
        return XC_ERR_LIMIT;
    }
    char *buf = malloc(total + 1U);
    if (buf == NULL) {
        return XC_ERR_NOMEM;
    }
    size_t k = 0;
    if (len == 0) {
        memcpy(buf, header, sizeof header - 1U);
        k = sizeof header - 1U;
    } else {
        memcpy(buf, data, len);
        k = len;
        if (data[len - 1] != '\n') {
            buf[k++] = '\n';
        }
    }
    memcpy(buf + k, line, n + 1U); /* its NUL too, which the line ending replaces */
    k += n;
    buf[k++] = '\n';
    return finish(buf, k, out, out_len);
}

xc_err xc_map_remove_line(const char *data, size_t len, size_t line, char **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    size_t pos = 0;
    size_t line_no = 0;
    if (!has_header(data, len, &pos, &line_no)) {
        return XC_ERR_MAP;
    }
    line_at l;
    for (; next_line(data, len, pos, &l); pos = l.end) {
        line_no++;
        if (line_no != line) {
            continue;
        }
        if (l.text_len == 0 || l.text[0] == '#') {
            return XC_ERR_INVALID_ARG;
        }
        char *buf = malloc(len - (l.end - l.start) + 1U);
        if (buf == NULL) {
            return XC_ERR_NOMEM;
        }
        memcpy(buf, data, l.start);
        memcpy(buf + l.start, data + l.end, len - l.end);
        return finish(buf, len - (l.end - l.start), out, out_len);
    }
    return XC_ERR_INVALID_ARG;
}
