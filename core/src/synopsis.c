/* XBMC4Gamers synopses (xcat/synopsis.h, docs/formats/synopsis.md). */
#include "xcat/synopsis.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* §3: the elements read, and where their text goes. */
static const struct {
    const char *tag;
    size_t offset;
    size_t size;
} fields[] = {
#define FIELD(tag, member)                                                                         \
    {                                                                                              \
        tag, offsetof(xc_synopsis, member), sizeof((xc_synopsis *)0)->member                       \
    }
    FIELD("title", title),
    FIELD("developer", developer),
    FIELD("publisher", publisher),
    FIELD("genre", genre),
    FIELD("release_date", release_date),
    FIELD("year", year),
    FIELD("esrb", esrb),
    FIELD("rating", rating),
    FIELD("features_general", features),
    FIELD("features_online", online),
    FIELD("titleid", title_id),
    FIELD("overview", overview),
#undef FIELD
};

typedef struct text {
    char *out;
    size_t cap; /* with the NUL */
    size_t n;
    bool space; /* a space is owed before the next character */
} text;

static bool starts(const char *p, const char *end, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
}

/* Where s starts in [p, end), or NULL. */
static const char *find(const char *p, const char *end, const char *s)
{
    size_t n = strlen(s);
    for (; (size_t)(end - p) >= n; p++) {
        if (memcmp(p, s, n) == 0) {
            return p;
        }
    }
    return NULL;
}

/* §3: appends code point u (whitespace and control characters as one space). */
static void put(text *t, uint32_t u)
{
    if (u <= 0x20U || u == 0x7FU || (u >= 0x80U && u <= 0x9FU)) {
        t->space = t->n > 0;
        return;
    }
    uint8_t b[5];
    size_t len = 0;
    if (u < 0x80U) {
        b[len++] = (uint8_t)u;
    } else if (u < 0x800U) {
        b[len++] = (uint8_t)(0xC0U | (u >> 6U));
        b[len++] = (uint8_t)(0x80U | (u & 0x3FU));
    } else if (u < 0x10000U) {
        b[len++] = (uint8_t)(0xE0U | (u >> 12U));
        b[len++] = (uint8_t)(0x80U | ((u >> 6U) & 0x3FU));
        b[len++] = (uint8_t)(0x80U | (u & 0x3FU));
    } else {
        b[len++] = (uint8_t)(0xF0U | (u >> 18U));
        b[len++] = (uint8_t)(0x80U | ((u >> 12U) & 0x3FU));
        b[len++] = (uint8_t)(0x80U | ((u >> 6U) & 0x3FU));
        b[len++] = (uint8_t)(0x80U | (u & 0x3FU));
    }
    size_t want = len + (t->space ? 1U : 0U);
    if (t->n + want >= t->cap) {
        t->cap = t->n + 1U; /* full: nothing more, so it's cut at a code point */
        return;
    }
    if (t->space) {
        t->out[t->n++] = ' ';
        t->space = false;
    }
    memcpy(t->out + t->n, b, len);
    t->n += len;
}

/* The UTF-8 sequence at p (p < end), as a code point; `?` for a byte that doesn't start a
   valid one. Moves p past it. */
static uint32_t utf8(const char **pp, const char *end)
{
    const uint8_t *p = (const uint8_t *)*pp;
    const uint8_t *e = (const uint8_t *)end;
    uint32_t c = p[0];
    size_t len = c < 0x80U              ? 1U
                 : (c & 0xE0U) == 0xC0U ? 2U
                 : (c & 0xF0U) == 0xE0U ? 3U
                 : (c & 0xF8U) == 0xF0U ? 4U
                                        : 0U;
    uint32_t u = len == 1U ? c : len == 2U ? c & 0x1FU : len == 3U ? c & 0x0FU : c & 0x07U;
    bool ok = len > 0U && (size_t)(e - p) >= len;
    for (size_t i = 1; ok && i < len; i++) {
        ok = (p[i] & 0xC0U) == 0x80U;
        u = (u << 6U) | (p[i] & 0x3FU);
    }
    static const uint32_t least[5] = {0, 0, 0x80U, 0x800U, 0x10000U};
    if (!ok || u < least[len] || u > 0x10FFFFU || (u >= 0xD800U && u <= 0xDFFFU)) {
        *pp += 1;
        return '?';
    }
    *pp += len;
    return u;
}

/* §3: an entity at p ('&'), as a code point, moving p past it; '&' itself if it isn't one. */
static uint32_t entity(const char **pp, const char *end)
{
    static const struct {
        const char *name;
        uint32_t u;
    } named[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    const char *p = *pp;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (starts(p, end, named[i].name)) {
            *pp = p + strlen(named[i].name);
            return named[i].u;
        }
    }
    if (starts(p, end, "&#")) {
        const char *q = p + 2;
        bool hex = q < end && (*q == 'x' || *q == 'X');
        q += hex ? 1 : 0;
        uint32_t u = 0;
        size_t digits = 0;
        for (; q < end && digits < 8; q++, digits++) {
            int v = *q >= '0' && *q <= '9'          ? *q - '0'
                    : hex && *q >= 'a' && *q <= 'f' ? *q - 'a' + 10
                    : hex && *q >= 'A' && *q <= 'F' ? *q - 'A' + 10
                                                    : -1;
            if (v < 0) {
                break;
            }
            u = u * (hex ? 16U : 10U) + (uint32_t)v;
        }
        if (digits > 0 && q < end && *q == ';' && u > 0 && u <= 0x10FFFFU &&
            !(u >= 0xD800U && u <= 0xDFFFU)) {
            *pp = q + 1;
            return u;
        }
    }
    *pp = p + 1;
    return '&';
}

/* §3: an element's content [p, end) into t: text and CDATA, other markup left out. */
static void content(text *t, const char *p, const char *end)
{
    while (p < end) {
        if (starts(p, end, "<![CDATA[")) {
            const char *q = p + 9;
            const char *e = find(q, end, "]]>");
            const char *stop = e != NULL ? e : end;
            while (q < stop) {
                put(t, utf8(&q, stop));
            }
            p = e != NULL ? e + 3 : end;
        } else if (*p == '<') {
            const char *e = memchr(p, '>', (size_t)(end - p));
            p = e != NULL ? e + 1 : end; /* a tag inside: left out */
        } else if (*p == '&') {
            put(t, entity(&p, end));
        } else {
            put(t, utf8(&p, end));
        }
    }
}

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == ':' || c == '.';
}

/* Past whitespace, the XML declaration, processing instructions, comments and a DOCTYPE. */
static const char *prolog(const char *p, const char *end)
{
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }
        const char *e = NULL;
        if (starts(p, end, "<?")) {
            e = find(p, end, "?>");
            p = e != NULL ? e + 2 : end;
        } else if (starts(p, end, "<!--")) {
            e = find(p + 4, end, "-->");
            p = e != NULL ? e + 3 : end;
        } else if (starts(p, end, "<!")) {
            e = memchr(p, '>', (size_t)(end - p));
            p = e != NULL ? e + 1 : end;
        } else {
            return p;
        }
    }
}

xc_err xc_synopsis_parse(const char *data, size_t len, xc_synopsis *out)
{
    memset(out, 0, sizeof *out);
    if (data == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    if (len > XC_SYNOPSIS_MAX_FILE) {
        return XC_ERR_LIMIT;
    }
    const char *p = data;
    const char *end = data + len;
    if (starts(p, end, "\xEF\xBB\xBF")) {
        p += 3; /* a UTF-8 byte order mark */
    }
    p = prolog(p, end);
    /* §2: the root, <synopsis> (any attributes). */
    if (!starts(p, end, "<synopsis") || p + 9 >= end || name_char(p[9])) {
        return XC_ERR_CORRUPT;
    }
    const char *gt = memchr(p, '>', (size_t)(end - p));
    if (gt == NULL) {
        return XC_ERR_CORRUPT;
    }
    if (gt[-1] == '/') {
        return XC_OK; /* <synopsis/>: nothing in it */
    }
    p = gt + 1;
    bool seen[sizeof fields / sizeof fields[0]] = {false};
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (lt == NULL || starts(lt, end, "</synopsis")) {
            break;
        }
        if (starts(lt, end, "<!--")) {
            const char *e = find(lt + 4, end, "-->");
            p = e != NULL ? e + 3 : end;
            continue;
        }
        if (lt + 1 >= end || !name_char(lt[1])) {
            const char *e = memchr(lt, '>', (size_t)(end - lt)); /* a stray end tag, or "<" */
            p = e != NULL ? e + 1 : end;
            continue;
        }
        const char *name = lt + 1;
        const char *ne = name;
        while (ne < end && name_char(*ne)) {
            ne++;
        }
        size_t nlen = (size_t)(ne - name);
        gt = memchr(ne, '>', (size_t)(end - ne));
        if (gt == NULL) {
            break;
        }
        /* Its content: to its end tag, "</name>", or to the file's end. */
        bool empty = gt[-1] == '/';
        const char *body = gt + 1;
        const char *close = NULL;
        if (!empty) {
            for (const char *q = body; (q = find(q, end, "</")) != NULL; q += 2) {
                if ((size_t)(end - q - 2) >= nlen && memcmp(q + 2, name, nlen) == 0 &&
                    (q + 2 + nlen == end || !name_char(q[2 + nlen]))) {
                    close = q;
                    break;
                }
            }
        }
        const char *body_end = empty ? body : close != NULL ? close : end;
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            if (!seen[i] && strlen(fields[i].tag) == nlen &&
                memcmp(fields[i].tag, name, nlen) == 0) {
                seen[i] = true; /* §3: the first of each counts */
                text t = {(char *)out + fields[i].offset, fields[i].size, 0, false};
                content(&t, body, body_end);
                t.out[t.n] = '\0';
            }
        }
        if (empty) {
            p = body;
        } else if (close != NULL) {
            const char *e = memchr(close, '>', (size_t)(end - close));
            p = e != NULL ? e + 1 : end;
        } else {
            p = end;
        }
    }
    return XC_OK;
}
