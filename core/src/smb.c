/*
 * smb:// URLs (docs/formats/smb.md §1), image names (§3), NT hashes (§4) and folder
 * listings (§3) for the map's @smb directive.
 */
#include "xcat/smb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md4.h"
#include "text.h"

bool xc_smb_is_url(const char *text)
{
    static const char scheme[] = "smb://";
    for (size_t i = 0; i < sizeof scheme - 1; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != scheme[i]) {
            return false;
        }
    }
    return true;
}

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

/* §1.3: the bytes a share, folder or file name may not hold. */
static bool name_byte_ok(unsigned char c)
{
    return c >= 0x20U && c != 0x7FU && strchr("\\/:*?\"<>|", (int)c) == NULL;
}

bool xc_smb_valid_name(const char *name, size_t len)
{
    if (len == 0 || len > XC_SMB_MAX_NAME) {
        return false;
    }
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!name_byte_ok((unsigned char)name[i])) {
            return false;
        }
    }
    size_t bad = 0;
    return xc_text_utf8((const unsigned char *)name, len, &bad);
}

/* Decodes one share, folder or file name from text[0, len) into out (cap bytes with the NUL):
   %XX escapes, then §1.2-1.3's rules. */
static bool decode_name(const char *text, size_t len, char *out, size_t cap, size_t *out_len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '#' || c == '?') {
            return false; /* §1: no query or fragment */
        }
        if (c == '%') {
            if (i + 2 >= len) {
                return false;
            }
            int hi = hexval(text[i + 1]);
            int lo = hexval(text[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (char)(hi << 4 | lo);
            i += 2;
        }
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    *out_len = n;
    return xc_smb_valid_name(out, n);
}

/* §1.1: a host name of letters, digits, '-' and '_' in dot-separated labels, or an IPv4
   literal of four decimal octets without leading zeros. Lowercased into out. */
static bool parse_host(const char *text, size_t len, char *out, bool *is_ipv4)
{
    if (len == 0 || len > XC_SMB_MAX_HOST) {
        return false;
    }
    bool digits_and_dots = true;
    size_t label = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '.') {
            if (label == 0) {
                return false;
            }
            label = 0;
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   c == '-' || c == '_') {
            if (++label > 63) {
                return false;
            }
            digits_and_dots = digits_and_dots && c >= '0' && c <= '9';
        } else {
            return false;
        }
        out[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    out[len] = '\0';
    if (label == 0) {
        return false;
    }
    *is_ipv4 = false;
    if (!digits_and_dots) {
        return true;
    }
    /* All digits and dots: it must be an IPv4 literal, or it's ambiguous. */
    int octets = 0;
    const char *p = out;
    while (*p != '\0') {
        size_t n = strspn(p, "0123456789");
        if (n == 0 || n > 3 || (n > 1 && p[0] == '0')) {
            return false;
        }
        int v = atoi(p);
        if (v > 255) {
            return false;
        }
        octets++;
        p += n;
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return false;
            }
        }
    }
    if (octets != 4) {
        return false;
    }
    *is_ipv4 = true;
    return true;
}

xc_err xc_smb_url_parse(const char *text, size_t len, xc_smb_url *out)
{
    if (text == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (len > XC_SMB_MAX_URL || len < 6 || memchr(text, '\0', len) != NULL) {
        return XC_ERR_SMB_URL;
    }
    char head[7];
    memcpy(head, text, 6);
    head[6] = '\0';
    if (!xc_smb_is_url(head)) {
        return XC_ERR_SMB_URL;
    }
    const char *p = text + 6;
    const char *end = text + len;
    /* §1.1: the authority, up to the first '/'. No user info: logins never go in URLs. */
    const char *slash = memchr(p, '/', (size_t)(end - p));
    if (slash == NULL) {
        return XC_ERR_SMB_URL;
    }
    const char *colon = memchr(p, ':', (size_t)(slash - p));
    const char *host_end = colon != NULL ? colon : slash;
    if (memchr(p, '@', (size_t)(slash - p)) != NULL ||
        !parse_host(p, (size_t)(host_end - p), out->host, &out->is_ipv4)) {
        return XC_ERR_SMB_URL;
    }
    out->port = XC_SMB_PORT;
    if (colon != NULL) {
        size_t n = (size_t)(slash - colon - 1);
        if (n == 0 || n > 5 || colon[1] == '0') {
            return XC_ERR_SMB_URL;
        }
        unsigned long port = 0;
        for (size_t i = 0; i < n; i++) {
            char c = colon[1 + i];
            if (c < '0' || c > '9') {
                return XC_ERR_SMB_URL;
            }
            port = port * 10U + (unsigned long)(c - '0');
        }
        if (port == 0 || port > 65535U) {
            return XC_ERR_SMB_URL;
        }
        out->port = (uint16_t)port;
    }
    /* §1.2: the share. */
    p = slash + 1;
    const char *seg_end = memchr(p, '/', (size_t)(end - p));
    if (seg_end == NULL) {
        seg_end = end;
    }
    size_t n = 0;
    if (!decode_name(p, (size_t)(seg_end - p), out->share, sizeof out->share, &n)) {
        return XC_ERR_SMB_URL;
    }
    /* §1.3: the folders and file, each name checked; one trailing '/' is allowed. */
    size_t path_len = 0;
    p = seg_end;
    while (p < end) {
        p++; /* the '/' */
        if (p == end) {
            break; /* a trailing '/' */
        }
        seg_end = memchr(p, '/', (size_t)(end - p));
        if (seg_end == NULL) {
            seg_end = end;
        }
        char name[XC_SMB_MAX_NAME + 1];
        if (!decode_name(p, (size_t)(seg_end - p), name, sizeof name, &n)) {
            return XC_ERR_SMB_URL;
        }
        size_t need = path_len + (path_len > 0 ? 1U : 0U) + n;
        if (need > XC_SMB_MAX_PATH) {
            return XC_ERR_SMB_URL;
        }
        if (path_len > 0) {
            out->path[path_len++] = '/';
        }
        memcpy(out->path + path_len, name, n);
        path_len += n;
        out->path[path_len] = '\0';
        p = seg_end;
    }
    return XC_OK;
}

/* Appends s with '%' and '#' escaped (§1.4), the bytes a name may hold that a URL can't
   carry as themselves; false if it doesn't fit. */
static bool put_escaped(char *out, size_t cap, size_t *at, const char *s)
{
    for (; *s != '\0'; s++) {
        bool esc = *s == '%' || *s == '#';
        size_t need = esc ? 3U : 1U;
        if (*at + need >= cap) {
            return false;
        }
        if (esc) {
            memcpy(out + *at, *s == '%' ? "%25" : "%23", 3);
        } else {
            out[*at] = *s;
        }
        *at += need;
    }
    out[*at] = '\0';
    return true;
}

xc_err xc_smb_url_format(const xc_smb_url *u, char *out, size_t cap)
{
    if (u == NULL || out == NULL || cap == 0) {
        return XC_ERR_INVALID_ARG;
    }
    char head[XC_SMB_MAX_HOST + 16];
    if (u->port == XC_SMB_PORT) {
        (void)snprintf(head, sizeof head, "smb://%s/", u->host);
    } else {
        (void)snprintf(head, sizeof head, "smb://%s:%u/", u->host, (unsigned)u->port);
    }
    size_t at = strlen(head);
    if (at >= cap) {
        return XC_ERR_LIMIT;
    }
    memcpy(out, head, at + 1);
    if (!put_escaped(out, cap, &at, u->share)) {
        return XC_ERR_LIMIT;
    }
    if (u->path[0] != '\0') {
        if (at + 1 >= cap) {
            return XC_ERR_LIMIT;
        }
        out[at++] = '/';
        out[at] = '\0';
        if (!put_escaped(out, cap, &at, u->path)) {
            return XC_ERR_LIMIT;
        }
    }
    return XC_OK;
}

xc_err xc_smb_url_child(const xc_smb_url *dir, const char *rel, xc_smb_url *out)
{
    if (dir == NULL || rel == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    char path[XC_SMB_MAX_PATH + 1];
    size_t len = strlen(dir->path);
    memcpy(path, dir->path, len + 1);
    for (const char *p = rel; *p != '\0';) {
        size_t n = strcspn(p, "/");
        if (!xc_smb_valid_name(p, n)) {
            return XC_ERR_SMB_URL;
        }
        if (len + (len > 0 ? 1U : 0U) + n > XC_SMB_MAX_PATH) {
            return XC_ERR_SMB_URL;
        }
        if (len > 0) {
            path[len++] = '/';
        }
        memcpy(path + len, p, n);
        len += n;
        path[len] = '\0';
        p += n;
        if (*p == '/') {
            p++;
            if (*p == '\0') {
                return XC_ERR_SMB_URL;
            }
        }
    }
    if (out != dir) {
        *out = *dir;
    }
    memcpy(out->path, path, len + 1);
    return XC_OK;
}

static bool ends_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (n > len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[len - n + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool xc_smb_image_name(const char *name, char *out, size_t cap, bool *zip)
{
    static const char *const endings[] = {".xiso.iso", ".iso", ".xiso", ".zip", ".7z"};
    const char *last = strrchr(name, '/');
    last = last != NULL ? last + 1 : name;
    size_t len = strlen(last);
    for (size_t i = 0; i < sizeof endings / sizeof endings[0]; i++) {
        size_t n = strlen(endings[i]);
        if (!ends_ci(last, len, endings[i])) {
            continue;
        }
        if (len == n || len - n + 1 > cap) {
            return false;
        }
        memcpy(out, last, len - n);
        out[len - n] = '\0';
        if (zip != NULL) {
            *zip = i == 3;
        }
        return true;
    }
    return false;
}

/* UTF-8 to UTF-16LE (code points checked by xc_text_utf8 first). Returns bytes written. */
static size_t to_utf16le(const unsigned char *s, size_t len, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        unsigned char c = s[i];
        if (c < 0x80U) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0U) == 0xC0U) {
            cp = (uint32_t)(c & 0x1FU) << 6 | (uint32_t)(s[i + 1] & 0x3FU);
            i += 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            cp = (uint32_t)(c & 0x0FU) << 12 | (uint32_t)(s[i + 1] & 0x3FU) << 6 |
                 (uint32_t)(s[i + 2] & 0x3FU);
            i += 3;
        } else {
            cp = (uint32_t)(c & 0x07U) << 18 | (uint32_t)(s[i + 1] & 0x3FU) << 12 |
                 (uint32_t)(s[i + 2] & 0x3FU) << 6 | (uint32_t)(s[i + 3] & 0x3FU);
            i += 4;
        }
        if (cp >= 0x10000U) {
            cp -= 0x10000U;
            uint32_t hi = 0xD800U + (cp >> 10);
            uint32_t lo = 0xDC00U + (cp & 0x3FFU);
            out[o++] = (uint8_t)hi;
            out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)lo;
            out[o++] = (uint8_t)(lo >> 8);
        } else {
            out[o++] = (uint8_t)cp;
            out[o++] = (uint8_t)(cp >> 8);
        }
    }
    return o;
}

xc_err xc_smb_nthash(const char *password, char hex[XC_SMB_NTHASH_HEX + 1])
{
    if (password == NULL || hex == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t len = 0;
    while (len <= XC_SMB_MAX_PASSWORD && password[len] != '\0') {
        len++;
    }
    size_t bad = 0;
    if (len > XC_SMB_MAX_PASSWORD || !xc_text_utf8((const unsigned char *)password, len, &bad)) {
        return XC_ERR_INVALID_ARG;
    }
    /* At most two UTF-16 bytes per UTF-8 byte (four for a four-byte sequence). */
    uint8_t utf16[2 * XC_SMB_MAX_PASSWORD];
    size_t n = to_utf16le((const unsigned char *)password, len, utf16);
    uint8_t digest[XC_MD4_BYTES];
    xc_md4(utf16, n, digest);
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < XC_MD4_BYTES; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 0x0FU];
    }
    hex[XC_SMB_NTHASH_HEX] = '\0';
    /* The password's UTF-16 form is as secret as the password. */
    volatile uint8_t *wipe = utf16;
    for (size_t i = 0; i < sizeof utf16; i++) {
        wipe[i] = 0;
    }
    return XC_OK;
}

bool xc_smb_valid_nthash(const char *hex)
{
    if (hex == NULL || strlen(hex) != XC_SMB_NTHASH_HEX) {
        return false;
    }
    for (size_t i = 0; i < XC_SMB_NTHASH_HEX; i++) {
        if (hexval(hex[i]) < 0) {
            return false;
        }
    }
    return true;
}

static xc_err listing_add(xc_smb_listing *l, const char *path, uint64_t size, bool game)
{
    if (l == NULL || path == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    if (l->count == XC_SMB_MAX_LISTED) {
        l->truncated = true;
        return XC_OK;
    }
    if (l->count == l->cap) {
        size_t cap = l->cap == 0 ? 32 : l->cap * 2;
        if (cap > XC_SMB_MAX_LISTED) {
            cap = XC_SMB_MAX_LISTED;
        }
        xc_smb_listed *grown = realloc(l->files, cap * sizeof *grown);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        l->files = grown;
        l->cap = cap;
    }
    size_t n = strlen(path) + 1;
    char *copy = malloc(n);
    if (copy == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(copy, path, n);
    l->files[l->count].path = copy;
    l->files[l->count].size = size;
    l->files[l->count].game = game;
    l->count++;
    return XC_OK;
}

xc_err xc_smb_listing_add(xc_smb_listing *l, const char *path, uint64_t size)
{
    return listing_add(l, path, size, false);
}

xc_err xc_smb_listing_add_game(xc_smb_listing *l, const char *path)
{
    return listing_add(l, path, 0, true);
}

void xc_smb_listing_cut(xc_smb_listing *l, size_t count)
{
    if (l == NULL) {
        return;
    }
    while (l->count > count) {
        free(l->files[--l->count].path);
    }
    if (count < XC_SMB_MAX_LISTED) {
        l->truncated = false;
    }
}

bool xc_smb_is_default_xbe(const char *name)
{
    static const char want[] = "default.xbe";
    size_t i = 0;
    for (; want[i] != '\0'; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != want[i]) {
            return false;
        }
    }
    return name[i] == '\0';
}

void xc_smb_listing_free(xc_smb_listing *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        free(l->files[i].path);
    }
    free(l->files);
    memset(l, 0, sizeof *l);
}
