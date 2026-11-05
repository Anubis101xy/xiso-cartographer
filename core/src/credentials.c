/*
 * Credentials file parser and per-hop decision: docs/formats/credentials.md §3
 * (syntax), §4 (keys), §5 (host patterns), §6 (when credentials are sent) and
 * §7 (errors: any problem rejects the whole file).
 */
#include "xcat/credentials.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xcat/login.h"
#include "xcat/smb.h"

/* --- small helpers ------------------------------------------------------------------ */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool has_control(const char *s)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20U || c == 0x7FU) {
            return true;
        }
    }
    return false;
}

/* HTTP token characters (RFC 9110 §5.6.2). */
static bool is_tchar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* `h` is a subdomain of `base` at any depth ("a.b.example.org" of "example.org"). */
static bool is_subdomain(const char *h, const char *base)
{
    size_t hl = strlen(h);
    size_t bl = strlen(base);
    return hl > bl + 1U && h[hl - bl - 1U] == '.' && strcmp(h + hl - bl, base) == 0;
}

/* --- parser ------------------------------------------------------------------------------ */

typedef struct parser {
    xc_credentials *c;
    xc_cred_error *err;
    size_t line;
    xc_cred_section *cur;
    bool has_hosts;
    bool has_allow_http;
} parser;

static xc_err fail(parser *p, const char *fmt, ...)
{
    p->err->line = p->line;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(p->err->message, sizeof p->err->message, fmt, ap);
    va_end(ap);
    return XC_ERR_CREDENTIALS;
}

/* §3.3: "# xcat-credentials v<MAJOR>[.<MINOR>]". */
static xc_err check_header(parser *p, const char *line)
{
    unsigned long major = 0;
    switch (xc_text_check_header(line, "xcat-credentials", XC_CRED_VERSION_MAJOR, &major, NULL)) {
    case XC_TEXT_HEADER_OK:
        return XC_OK;
    case XC_TEXT_HEADER_NEWER:
        return fail(p, "needs a newer xiso-cartographer (credentials format v%lu)", major);
    case XC_TEXT_HEADER_OLDER:
        return fail(p, "unsupported credentials format v%lu", major);
    case XC_TEXT_HEADER_MISSING:
        break;
    }
    return fail(p, "not a credentials file (missing \"# xcat-credentials v1\")");
}

/* §4: a finished section needs hosts and something to send. */
static xc_err close_section(parser *p)
{
    xc_cred_section *s = p->cur;
    if (s == NULL) {
        return XC_OK;
    }
    if (!p->has_hosts) {
        p->line = s->line;
        return fail(p, "[%s] has no hosts", s->name);
    }
    if ((s->smb_user == NULL) != (s->smb_nthash == NULL)) {
        p->line = s->line;
        return fail(p, "[%s]: smb_user and smb_nthash go together", s->name);
    }
    if (s->smb_domain != NULL && s->smb_user == NULL) {
        p->line = s->line;
        return fail(p, "[%s]: smb_domain without smb_user", s->name);
    }
    if (s->cookie == NULL && s->header_count == 0 && s->smb_user == NULL) {
        p->line = s->line;
        return fail(p, "[%s] has neither cookie, header nor smb_user", s->name);
    }
    return XC_OK;
}

/* §4.1: an SMB user or domain name: printable, no separators a login would split at. */
static bool smb_login_name(const char *v, size_t max)
{
    size_t n = strlen(v);
    if (n == 0 || n > max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (strchr("\\/:@\"", v[i]) != NULL) {
            return false;
        }
    }
    size_t bad = 0;
    return xc_text_utf8((const unsigned char *)v, n, &bad);
}

static xc_err open_section(parser *p, char *line)
{
    xc_err err = close_section(p);
    if (err != XC_OK) {
        return err;
    }
    size_t n = strlen(line);
    if (line[n - 1] != ']') {
        return fail(p, "malformed section line");
    }
    line[n - 1] = '\0';
    char *name = line + 1;
    size_t len = n - 2;
    if (len == 0 || len > XC_CRED_MAX_NAME) {
        return fail(p, "section names are 1-%d characters", XC_CRED_MAX_NAME);
    }
    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) {
            return fail(p, "invalid section name (A-Z a-z 0-9 _ - only)");
        }
    }
    xc_credentials *c = p->c;
    for (size_t i = 0; i < c->section_count; i++) {
        if (xc_text_eq_ci(c->sections[i].name, name)) {
            return fail(p, "section [%s] appears twice", name);
        }
    }
    if (c->section_count == XC_CRED_MAX_SECTIONS) {
        return fail(p, "more than %d sections", XC_CRED_MAX_SECTIONS);
    }
    xc_cred_section *s = &c->sections[c->section_count++];
    memset(s, 0, sizeof *s);
    s->name = name;
    s->line = p->line;
    p->cur = s;
    p->has_hosts = false;
    p->has_allow_http = false;
    return XC_OK;
}

/* §5: one host pattern, in place (lowercased, cut at the port). */
static bool parse_pattern(char *text, xc_cred_pattern *out)
{
    memset(out, 0, sizeof *out);
    char *host = text;
    if (host[0] == '*' && host[1] == '.') {
        out->wildcard = true;
        host += 2;
    }
    size_t colons = 0;
    for (const char *q = host; *q != '\0'; q++) {
        char ch = *q;
        if (ch == ':') {
            colons++;
        } else if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) {
            return false; /* includes any other '*', '/', '@', '[' */
        }
    }
    if (colons > 1 || host[0] == '\0' || host[0] == ':') {
        return false;
    }
    /* The URL parser applies the host rules of http.md §2.1 (labels, IPv4, the digit rule). */
    char url_text[XC_URL_MAX_HOST + 16];
    int n = snprintf(url_text, sizeof url_text, "http://%s/", host);
    if (n < 0 || (size_t)n >= sizeof url_text) {
        return false;
    }
    xc_url u;
    if (xc_url_parse(url_text, (size_t)n, &u) != XC_OK || (out->wildcard && u.is_ipv4)) {
        return false;
    }
    char *colon = strchr(host, ':');
    out->any_port = colon == NULL;
    out->port = colon != NULL ? u.port : 0;
    if (colon != NULL) {
        *colon = '\0';
    }
    for (char *q = host; *q != '\0'; q++) {
        *q = lower(*q);
    }
    out->host = host;
    return true;
}

static xc_err key_hosts(parser *p, char *value)
{
    xc_cred_section *s = p->cur;
    if (p->has_hosts) {
        return fail(p, "[%s]: hosts given twice", s->name);
    }
    p->has_hosts = true;
    char *item = value;
    for (;;) {
        char *comma = strchr(item, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *pat = xc_text_trim(item);
        if (s->pattern_count == XC_CRED_MAX_PATTERNS) {
            return fail(p, "[%s]: more than %d host patterns", s->name, XC_CRED_MAX_PATTERNS);
        }
        if (pat[0] == '\0' || !parse_pattern(pat, &s->patterns[s->pattern_count])) {
            return fail(p, "[%s]: invalid host pattern", s->name);
        }
        s->pattern_count++;
        if (comma == NULL) {
            return XC_OK;
        }
        item = comma + 1;
    }
}

static xc_err key_header(parser *p, char *value)
{
    static const char *const forbidden[] = {
        "Host", "Content-Length", "Transfer-Encoding", "Connection", "Range", "Cookie",
    };
    xc_cred_section *s = p->cur;
    if (s->header_count == XC_CRED_MAX_HEADERS) {
        return fail(p, "[%s]: more than %d headers", s->name, XC_CRED_MAX_HEADERS);
    }
    char *colon = strchr(value, ':');
    if (colon == NULL || colon == value) {
        return fail(p, "[%s]: header must be \"Name: value\"", s->name);
    }
    *colon = '\0';
    for (const char *q = value; *q != '\0'; q++) {
        if (!is_tchar(*q)) {
            return fail(p, "[%s]: invalid header name", s->name);
        }
    }
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
        if (xc_text_eq_ci(value, forbidden[i])) {
            return fail(p, "[%s]: header %s can't be set here", s->name, forbidden[i]);
        }
    }
    for (size_t i = 0; i < s->header_count; i++) {
        if (xc_text_eq_ci(s->header_names[i], value)) {
            return fail(p, "[%s]: header %s given twice", s->name, s->header_names[i]);
        }
    }
    s->header_names[s->header_count] = value;
    s->header_values[s->header_count] = xc_text_trim(colon + 1);
    s->header_count++;
    return XC_OK;
}

static xc_err key_value(parser *p, char *line)
{
    if (p->cur == NULL) {
        return fail(p, "key outside a [section]");
    }
    xc_cred_section *s = p->cur;
    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return fail(p, "expected \"key = value\"");
    }
    *eq = '\0';
    char *key = xc_text_trim(line);
    char *value = xc_text_trim(eq + 1);
    if (has_control(value)) {
        return fail(p, "[%s]: control character in a value", s->name);
    }
    if (strcmp(key, "hosts") == 0) {
        return key_hosts(p, value);
    }
    if (strcmp(key, "cookie") == 0) {
        if (s->cookie != NULL) {
            return fail(p, "[%s]: cookie given twice", s->name);
        }
        if (value[0] == '\0') {
            return fail(p, "[%s]: empty cookie", s->name);
        }
        s->cookie = value;
        return XC_OK;
    }
    if (strcmp(key, "header") == 0) {
        return key_header(p, value);
    }
    if (strcmp(key, "smb_user") == 0 || strcmp(key, "smb_domain") == 0) {
        bool user = key[4] == 'u';
        const char **slot = user ? &s->smb_user : &s->smb_domain;
        if (*slot != NULL) {
            return fail(p, "[%s]: %s given twice", s->name, key);
        }
        if (!smb_login_name(value, user ? XC_SMB_MAX_USER : XC_SMB_MAX_DOMAIN)) {
            return fail(p, "[%s]: invalid %s", s->name, key);
        }
        *slot = value;
        return XC_OK;
    }
    if (strcmp(key, "smb_nthash") == 0) {
        if (s->smb_nthash != NULL) {
            return fail(p, "[%s]: smb_nthash given twice", s->name);
        }
        if (!xc_smb_valid_nthash(value)) {
            return fail(p, "[%s]: smb_nthash must be 32 hex digits", s->name);
        }
        s->smb_nthash = value;
        return XC_OK;
    }
    if (strcmp(key, "allow_http") == 0) {
        if (p->has_allow_http) {
            return fail(p, "[%s]: allow_http given twice", s->name);
        }
        p->has_allow_http = true;
        if (strcmp(value, "yes") != 0 && strcmp(value, "no") != 0) {
            return fail(p, "[%s]: allow_http must be yes or no", s->name);
        }
        s->allow_http = value[0] == 'y';
        return XC_OK;
    }
    return fail(p, "[%s]: unknown key", s->name);
}

static bool patterns_overlap(const xc_cred_pattern *a, const xc_cred_pattern *b)
{
    if (!a->any_port && !b->any_port && a->port != b->port) {
        return false;
    }
    if (!a->wildcard && !b->wildcard) {
        return strcmp(a->host, b->host) == 0;
    }
    if (a->wildcard && b->wildcard) {
        return strcmp(a->host, b->host) == 0 || is_subdomain(a->host, b->host) ||
               is_subdomain(b->host, a->host);
    }
    const xc_cred_pattern *w = a->wildcard ? a : b;
    const xc_cred_pattern *e = a->wildcard ? b : a;
    return is_subdomain(e->host, w->host);
}

static bool is_http_section(const xc_cred_section *s)
{
    return s->cookie != NULL || s->header_count > 0;
}

/* §5: no host may be matched by two sections that log in to the same kind of server: two with
   HTTP credentials, or two with an SMB login. */
static xc_err check_overlaps(parser *p)
{
    xc_credentials *c = p->c;
    for (size_t j = 1; j < c->section_count; j++) {
        const xc_cred_section *sj = &c->sections[j];
        for (size_t i = 0; i < j; i++) {
            const xc_cred_section *si = &c->sections[i];
            bool both_http = is_http_section(si) && is_http_section(sj);
            bool both_smb = si->smb_user != NULL && sj->smb_user != NULL;
            if (!both_http && !both_smb) {
                continue;
            }
            for (size_t a = 0; a < si->pattern_count; a++) {
                for (size_t b = 0; b < sj->pattern_count; b++) {
                    if (patterns_overlap(&si->patterns[a], &sj->patterns[b])) {
                        p->line = sj->line;
                        return fail(p, "hosts of [%s] overlap with [%s]", sj->name, si->name);
                    }
                }
            }
        }
    }
    return XC_OK;
}

static xc_err parse_lines(parser *p, char *text)
{
    bool header_seen = false;
    char *next = text;
    p->line = 0;
    while (next != NULL) {
        char *line = next;
        p->line++;
        char *nl = strchr(line, '\n');
        next = nl != NULL ? nl + 1 : NULL;
        if (nl != NULL) {
            *nl = '\0';
            if (nl > line && nl[-1] == '\r') {
                nl[-1] = '\0';
            }
        }
        if (strlen(line) > XC_CRED_MAX_LINE) {
            return fail(p, "line longer than %d bytes", XC_CRED_MAX_LINE);
        }
        line = xc_text_trim(line);
        if (line[0] == '\0') {
            continue;
        }
        if (!header_seen) {
            xc_err err = check_header(p, line);
            if (err != XC_OK) {
                return err;
            }
            header_seen = true;
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        xc_err err = line[0] == '[' ? open_section(p, line) : key_value(p, line);
        if (err != XC_OK) {
            return err;
        }
    }
    if (!header_seen) {
        p->line = 0;
        return fail(p, "empty file (missing \"# xcat-credentials v1\")");
    }
    xc_err err = close_section(p);
    return err != XC_OK ? err : check_overlaps(p);
}

xc_err xc_cred_parse(const void *data, size_t len, xc_credentials *out, xc_cred_error *err)
{
    if (out == NULL || err == NULL || (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    memset(err, 0, sizeof *err);
    parser p = {out, err, 0, NULL, false, false};
    if (len > XC_CRED_MAX_FILE) {
        return fail(&p, "file larger than 64 KiB");
    }
    const unsigned char *bytes = data;
    if (len >= 3 && bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU) {
        bytes += 3;
        len -= 3;
    }
    /* The first byte that isn't UTF-8 text, or a NUL, whichever comes first. */
    size_t at = len;
    (void)xc_text_utf8(bytes, len, &at);
    const unsigned char *nul = len > 0 ? memchr(bytes, '\0', len) : NULL;
    if (nul != NULL && (size_t)(nul - bytes) < at) {
        at = (size_t)(nul - bytes);
    }
    if (at < len) {
        p.line = 1;
        for (size_t i = 0; i < at; i++) {
            p.line += bytes[i] == '\n' ? 1U : 0U;
        }
        return fail(&p, "not UTF-8 text");
    }
    out->text = malloc(len + 1);
    out->sections = calloc(XC_CRED_MAX_SECTIONS, sizeof *out->sections);
    if (out->text == NULL || out->sections == NULL) {
        xc_cred_free(out);
        return XC_ERR_NOMEM;
    }
    if (len > 0) {
        memcpy(out->text, bytes, len);
    }
    out->text[len] = '\0';
    xc_err e = parse_lines(&p, out->text);
    if (e != XC_OK) {
        xc_cred_free(out);
    }
    return e;
}

void xc_cred_free(xc_credentials *c)
{
    if (c != NULL) {
        free(c->text);
        free(c->sections);
        memset(c, 0, sizeof *c);
    }
}

/* --- §6: per-hop decision --------------------------------------------------------------- */

static bool pattern_matches(const xc_cred_pattern *pat, const xc_url *url)
{
    if (!pat->any_port && pat->port != url->port) {
        return false;
    }
    if (pat->wildcard) {
        return !url->is_ipv4 && is_subdomain(url->host, pat->host);
    }
    return strcmp(url->host, pat->host) == 0;
}

static bool private_ipv4(const xc_url *url)
{
    uint8_t ip[4];
    if (!xc_url_ipv4(url, ip)) {
        return false;
    }
    return ip[0] == 10U || (ip[0] == 172U && ip[1] >= 16U && ip[1] <= 31U) ||
           (ip[0] == 192U && ip[1] == 168U);
}

const xc_cred_section *xc_cred_for_url(const xc_credentials *c, const xc_url *url)
{
    if (c == NULL || url == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < c->section_count; i++) {
        const xc_cred_section *s = &c->sections[i];
        if (!is_http_section(s)) {
            continue; /* an SMB login only (§6.1) */
        }
        for (size_t k = 0; k < s->pattern_count; k++) {
            if (pattern_matches(&s->patterns[k], url)) {
                /* HTTP sections don't overlap, so no other one can match this host. */
                if (url->scheme == XC_URL_HTTPS) {
                    return s;
                }
                return s->allow_http && private_ipv4(url) ? s : NULL;
            }
        }
    }
    return NULL;
}

const xc_cred_section *xc_cred_for_smb(const xc_credentials *c, const char *host, uint16_t port)
{
    if (c == NULL || host == NULL) {
        return NULL;
    }
    xc_url url;
    memset(&url, 0, sizeof url);
    size_t n = strlen(host);
    if (n == 0 || n > XC_URL_MAX_HOST) {
        return NULL;
    }
    memcpy(url.host, host, n + 1);
    url.port = port;
    uint8_t ip[4];
    url.is_ipv4 = xc_url_ipv4(&url, ip);
    for (size_t i = 0; i < c->section_count; i++) {
        const xc_cred_section *s = &c->sections[i];
        if (s->smb_user == NULL) {
            continue;
        }
        for (size_t k = 0; k < s->pattern_count; k++) {
            if (pattern_matches(&s->patterns[k], &url)) {
                return s; /* SMB sections don't overlap (§5) */
            }
        }
    }
    return NULL;
}

xc_err xc_cred_smb_section(const char *name, const char *host, const char *user, const char *domain,
                           const char *nthash, char *out, size_t cap)
{
    if (name == NULL || host == NULL || user == NULL || nthash == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t nl = strlen(name);
    if (nl == 0 || nl > XC_CRED_MAX_NAME) {
        return XC_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < nl; i++) {
        char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) {
            return XC_ERR_INVALID_ARG;
        }
    }
    bool with_domain = domain != NULL && domain[0] != '\0';
    if (!smb_login_name(user, XC_SMB_MAX_USER) || has_control(user) ||
        (with_domain && (!smb_login_name(domain, XC_SMB_MAX_DOMAIN) || has_control(domain))) ||
        !xc_smb_valid_nthash(nthash) || host[0] == '\0' || has_control(host) ||
        strpbrk(host, " ,[]=") != NULL) {
        return XC_ERR_INVALID_ARG;
    }
    int n = with_domain ? snprintf(out, cap,
                                   "[%s]\nhosts = %s\nsmb_user = %s\nsmb_domain = %s\n"
                                   "smb_nthash = %s\n",
                                   name, host, user, domain, nthash)
                        : snprintf(out, cap, "[%s]\nhosts = %s\nsmb_user = %s\nsmb_nthash = %s\n",
                                   name, host, user, nthash);
    if (n < 0 || (size_t)n >= cap) {
        return XC_ERR_LIMIT;
    }
    return XC_OK;
}

/* Appends bytes (header lines are not NUL-terminated: the client sends exactly *n bytes). */
static bool put_mem(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    if (len > cap - *n) {
        return false;
    }
    memcpy(out + *n, s, len);
    *n += len;
    return true;
}

static bool put(char *out, size_t cap, size_t *n, const char *s)
{
    return put_mem(out, cap, n, s, strlen(s));
}

xc_err xc_cred_headers(const xc_cred_section *s, char *out, size_t cap, size_t *len)
{
    if (s == NULL || out == NULL || len == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t n = 0;
    bool ok = true;
    if (s->cookie != NULL) {
        ok = put(out, cap, &n, "Cookie: ") && put(out, cap, &n, s->cookie) &&
             put(out, cap, &n, "\r\n");
    }
    for (size_t i = 0; ok && i < s->header_count; i++) {
        ok = put(out, cap, &n, s->header_names[i]) && put(out, cap, &n, ": ") &&
             put(out, cap, &n, s->header_values[i]) && put(out, cap, &n, "\r\n");
    }
    *len = ok ? n : 0;
    return ok ? XC_OK : XC_ERR_LIMIT;
}

static xc_err auth_headers(void *ctx, const xc_url *url, char *out, size_t cap, size_t *len,
                           const char **section)
{
    const xc_cred_section *s = xc_cred_for_url(ctx, url);
    *len = 0;
    *section = NULL;
    if (s == NULL) {
        return XC_OK;
    }
    xc_err err = xc_cred_headers(s, out, cap, len);
    if (err == XC_OK) {
        *section = s->name;
    }
    return err;
}

void xc_cred_http_auth(xc_http_auth *auth, const xc_credentials *c)
{
    auth->headers = auth_headers;
    auth->ctx = (void *)(uintptr_t)c; /* only ever read */
}

const xc_cred_section *xc_cred_section_named(const xc_credentials *c, const char *name)
{
    for (size_t i = 0; c != NULL && name != NULL && i < c->section_count; i++) {
        if (xc_text_eq_ci(c->sections[i].name, name)) {
            return &c->sections[i];
        }
    }
    return NULL;
}

/* --- login.md §5: writing one section --------------------------------------------------- */

/* One line of the file: [start, end) with its line ending, and its text trimmed. */
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

static bool is_section_line(const line_at *l)
{
    return l->text_len >= 2 && l->text[0] == '[' && l->text[l->text_len - 1] == ']';
}

static bool names_section(const line_at *l, const char *name)
{
    size_t n = strlen(name);
    if (!is_section_line(l) || l->text_len != n + 2U) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(l->text[i + 1]) != lower(name[i])) {
            return false;
        }
    }
    return true;
}

/* §5: where [name] is: from its line to the end of its last key line. False if it isn't. */
static bool find_section(const char *data, size_t len, const char *name, size_t *from, size_t *to)
{
    line_at l;
    bool in = false;
    for (size_t pos = 0; next_line(data, len, pos, &l); pos = l.end) {
        if (in && is_section_line(&l)) {
            break;
        }
        if (!in && names_section(&l, name)) {
            in = true;
            *from = l.start;
            *to = l.end;
        } else if (in && l.text_len > 0 && l.text[0] != '#') {
            *to = l.end;
        }
    }
    return in;
}

static bool append(char **buf, size_t *n, size_t *cap, const char *s, size_t len)
{
    if (len > *cap - *n) {
        return false;
    }
    if (len == 0) {
        return true; /* s may be NULL (no section, when logging out): memcpy must not see it */
    }
    memcpy(*buf + *n, s, len);
    *n += len;
    return true;
}

xc_err xc_cred_set_section(const char *data, size_t len, const char *name, const char *section,
                           char **out, size_t *out_len, xc_cred_error *err)
{
    if (name == NULL || out == NULL || out_len == NULL || err == NULL ||
        (data == NULL && len > 0)) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    memset(err, 0, sizeof *err);
    xc_credentials c;
    static const char header[] = "# xcat-credentials v1\n\n";
    if (len > 0) {
        xc_err e = xc_cred_parse(data, len, &c, err);
        if (e != XC_OK) {
            return e;
        }
        xc_cred_free(&c);
    }
    size_t sec_len = section != NULL ? strlen(section) : 0;
    size_t cap = len + sec_len + sizeof header + 2U;
    char *buf = malloc(cap + 1U);
    if (buf == NULL) {
        return XC_ERR_NOMEM;
    }
    size_t n = 0;
    size_t from = 0;
    size_t to = 0;
    bool ok = true;
    if (len == 0) {
        ok = section == NULL || (append(&buf, &n, &cap, header, sizeof header - 1U) &&
                                 append(&buf, &n, &cap, section, sec_len));
    } else if (find_section(data, len, name, &from, &to)) {
        ok = append(&buf, &n, &cap, data, from) && append(&buf, &n, &cap, section, sec_len) &&
             append(&buf, &n, &cap, data + to, len - to);
    } else {
        ok = append(&buf, &n, &cap, data, len);
        if (ok && section != NULL) {
            ok = (data[len - 1] == '\n' || append(&buf, &n, &cap, "\n", 1)) &&
                 append(&buf, &n, &cap, "\n", 1) && append(&buf, &n, &cap, section, sec_len);
        }
    }
    buf[n] = '\0';
    xc_err e = ok ? XC_OK : XC_ERR_LIMIT;
    if (e == XC_OK && n > 0) {
        e = xc_cred_parse(buf, n, &c, err); /* the result must be a valid file too */
        if (e == XC_OK) {
            xc_cred_free(&c);
        }
    }
    if (e != XC_OK) {
        xc_login_wipe(buf, cap + 1U);
        free(buf);
        return e;
    }
    *out = buf;
    *out_len = n;
    return XC_OK;
}
