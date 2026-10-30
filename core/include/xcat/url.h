#ifndef XCAT_URL_H
#define XCAT_URL_H

/*
 * http:// and https:// URLs as the HTTP client accepts them
 * (docs/formats/http.md §2): parsing, request targets and redirect resolution.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_URL_MAX 2048        /* bytes in a URL, before percent-encoding (§1) */
#define XC_URL_MAX_HOST 253    /* bytes in a host name (§2.1) */
#define XC_URL_MAX_TARGET 4096 /* bytes in a request target, after percent-encoding (§1) */

typedef enum xc_url_scheme {
    XC_URL_HTTP,
    XC_URL_HTTPS,
    XC_URL_SMB /* only in xc_map_host: an smb:// source (docs/formats/smb.md); never parsed here */
} xc_url_scheme;

typedef struct xc_url {
    xc_url_scheme scheme;
    uint16_t port;                      /* explicit, or the scheme's default */
    bool is_ipv4;                       /* host is an IPv4 literal */
    char host[XC_URL_MAX_HOST + 1];     /* lowercased */
    char target[XC_URL_MAX_TARGET + 1]; /* path and query, percent-encoded; starts with '/' */
} xc_url;

/* Parses `len` bytes of `text` (§2.1, §2.2). Returns XC_ERR_URL if it isn't an accepted URL. */
xc_err xc_url_parse(const char *text, size_t len, xc_url *out);

/*
 * Resolves a Location header value against `base` (§2.3). `out` may not alias
 * `base`. Returns XC_ERR_URL if the result isn't an accepted URL.
 */
xc_err xc_url_resolve(const xc_url *base, const char *ref, size_t ref_len, xc_url *out);

/* True if `url` uses its scheme's default port. */
bool xc_url_default_port(const xc_url *url);

/* The four bytes of an IPv4 host (most significant first). False if the host isn't one. */
bool xc_url_ipv4(const xc_url *url, uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_URL_H */
