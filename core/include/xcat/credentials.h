#ifndef XCAT_CREDENTIALS_H
#define XCAT_CREDENTIALS_H

/*
 * Credentials file (docs/formats/credentials.md, ADR 0007): login tokens, each
 * scoped to the hosts that may receive it, decided again for every request hop.
 *
 * The file is all-or-nothing (§7): xc_cred_parse() either accepts all of it or
 * rejects it with a line number and a reason. Messages name sections, never
 * values (§8).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/http_client.h"
#include "xcat/url.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_CRED_MAX_FILE ((size_t)64 * 1024) /* §3.1 */
#define XC_CRED_MAX_LINE 4096                /* §3.1, bytes without the line ending */
#define XC_CRED_MAX_SECTIONS 32              /* §3.5 */
#define XC_CRED_MAX_NAME 32                  /* §3.4 */
#define XC_CRED_MAX_HEADERS 16               /* §4 */
#define XC_CRED_MAX_PATTERNS 16              /* §5 */
#define XC_CRED_VERSION_MAJOR 1

/* One `hosts` pattern (§5). Strings point into xc_credentials.text. */
typedef struct xc_cred_pattern {
    const char *host; /* lowercased; for "*.example.org" this is "example.org" */
    bool wildcard;    /* "*." prefix: any subdomain, not the host itself */
    bool any_port;
    uint16_t port; /* when !any_port */
} xc_cred_pattern;

typedef struct xc_cred_section {
    const char *name; /* without the brackets */
    size_t line;      /* where "[name]" is, for messages */
    xc_cred_pattern patterns[XC_CRED_MAX_PATTERNS];
    size_t pattern_count;
    const char *cookie; /* NULL if none */
    const char *header_names[XC_CRED_MAX_HEADERS];
    const char *header_values[XC_CRED_MAX_HEADERS];
    size_t header_count;
    bool allow_http;
    /* SMB login (§4.1, docs/formats/smb.md §4): NULL if the section has none. smb_nthash is
       32 hex digits, never the password. */
    const char *smb_user;
    const char *smb_domain; /* NULL: none given */
    const char *smb_nthash;
} xc_cred_section;

typedef struct xc_credentials {
    char *text; /* owned copy of the file, split into strings in place */
    xc_cred_section *sections;
    size_t section_count;
} xc_credentials;

/* Why a file was rejected. `message` names at most a section, never a value. */
typedef struct xc_cred_error {
    size_t line; /* 1-based; 0 for the file as a whole */
    char message[128];
} xc_cred_error;

/*
 * Parses a credentials file held in data[0, len). On success `out` owns its
 * memory (free with xc_cred_free). On failure (XC_ERR_CREDENTIALS, or
 * XC_ERR_NOMEM) `out` is empty and `err` says where and why; per §7 the caller
 * then goes on without credentials.
 */
xc_err xc_cred_parse(const void *data, size_t len, xc_credentials *out, xc_cred_error *err);
void xc_cred_free(xc_credentials *c);

/*
 * The section whose credentials may go with a request to `url` (§6): the host
 * and port match one of its patterns, and the scheme is https, or http to a
 * private IPv4 address with allow_http. NULL means: send none. Patterns never
 * overlap (§5), so at most one section matches.
 */
const xc_cred_section *xc_cred_for_url(const xc_credentials *c, const xc_url *url);

/*
 * Writes a section's header lines ("Cookie: ...\r\n", then each header) into
 * out[0, cap) and sets *len. XC_ERR_LIMIT if they don't fit.
 */
xc_err xc_cred_headers(const xc_cred_section *s, char *out, size_t cap, size_t *len);

/*
 * The section whose SMB login goes to the NAS `host` (lowercased) on `port` (§6.1): one with
 * smb_user whose hosts match. NULL: none, so the NAS is asked as a guest. SMB sections never
 * overlap one another (§5), so at most one matches.
 */
const xc_cred_section *xc_cred_for_smb(const xc_credentials *c, const char *host, uint16_t port);

/*
 * Writes a section for an SMB login (§4.1) into out[0, cap): "[name]", then hosts, smb_user,
 * smb_domain (if `domain` isn't NULL or "") and smb_nthash lines, each ending in "\n", as
 * xc_cred_set_section takes it. XC_ERR_INVALID_ARG if a value breaks §4.1, XC_ERR_LIMIT if it
 * doesn't fit.
 */
xc_err xc_cred_smb_section(const char *name, const char *host, const char *user, const char *domain,
                           const char *nthash, char *out, size_t cap);

/* Fills in an xc_http_auth that attaches `c`'s credentials hop by hop. `c` must outlive it. */
void xc_cred_http_auth(xc_http_auth *auth, const xc_credentials *c);

/* The section called `name` (case-insensitively), or NULL. */
const xc_cred_section *xc_cred_section_named(const xc_credentials *c, const char *name);

/*
 * The file with one section written or removed (docs/formats/login.md §5): data[0, len) with
 * section [name] replaced by `section` (whole lines, each ending in "\n"), or removed if
 * `section` is NULL; added at the end if the file has none. With no file (data NULL, len 0),
 * a new one. Every other byte stays as it was. The file must parse before, and the result
 * after, or it's XC_ERR_CREDENTIALS and `err` says why. On success *out (malloc'd, NUL-
 * terminated, *out_len bytes) holds tokens: wipe it before freeing it.
 */
xc_err xc_cred_set_section(const char *data, size_t len, const char *name, const char *section,
                           char **out, size_t *out_len, xc_cred_error *err);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_CREDENTIALS_H */
