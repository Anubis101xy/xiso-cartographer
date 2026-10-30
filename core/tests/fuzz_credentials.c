/*
 * libFuzzer target for the credentials file parser (docs/formats/credentials.md).
 *
 * Every input is parsed as a credentials file. When it is accepted, the result is
 * checked against the guarantees the HTTP client relies on; a violation aborts:
 *
 *   - limits hold (sections, patterns, headers, name lengths);
 *   - every section has hosts and something to send: HTTP credentials, or an SMB login
 *     (user and 32 hex digits of NT hash, §4.1);
 *   - the emitted header lines are well formed: "Name: value\r\n" with no CR, LF
 *     or other control character anywhere else, so a value can't inject a header;
 *   - every pattern of an HTTP section selects its own section over https (no overlap,
 *     §5), and a plain-http request gets credentials only from a section with allow_http;
 *   - every exact pattern of an SMB section selects it for SMB (no overlap among them), and
 *     an SMB-only section is never sent over HTTP.
 *
 * Seeds are in core/tests/fuzz-seeds/credentials/. Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/credentials.h"
#include "xcat/smb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

/* Header lines: CRLF only as terminators, a ": " in each, no other control bytes. */
static void check_headers(const xc_cred_section *s)
{
    static char out[XC_CRED_MAX_FILE + 1024];
    size_t len = 0;
    check(xc_cred_headers(s, out, sizeof out, &len) == XC_OK);
    size_t lines = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c == '\r') {
            check(i + 1 < len && out[i + 1] == '\n');
            check(memchr(out + start, ':', i - start) != NULL);
            lines++;
            i++;
            start = i + 1;
            continue;
        }
        check(c >= 0x20U && c != 0x7FU);
    }
    check(start == len);
    check(lines == s->header_count + (s->cookie != NULL ? 1U : 0U));
}

/* The URL a pattern stands for: "https://[x.]host[:port]/". */
static void check_selects(const xc_credentials *c, const xc_cred_section *s,
                          const xc_cred_pattern *p)
{
    char text[XC_URL_MAX_HOST + 32];
    int n = p->any_port
                ? snprintf(text, sizeof text, "https://%s%s/", p->wildcard ? "x." : "", p->host)
                : snprintf(text, sizeof text, "https://%s%s:%u/", p->wildcard ? "x." : "", p->host,
                           (unsigned)p->port);
    check(n > 0 && (size_t)n < sizeof text);
    static xc_url u;
    if (xc_url_parse(text, (size_t)n, &u) != XC_OK) {
        check(p->wildcard); /* only "x." + a 252-253 byte host can be too long */
        return;
    }
    bool http = s->cookie != NULL || s->header_count > 0;
    const xc_cred_section *got = xc_cred_for_url(c, &u);
    check(http ? got == s : got != s);
    if (s->smb_user != NULL && !p->wildcard) {
        check(xc_cred_for_smb(c, p->host, p->any_port ? 445 : p->port) == s);
    }
    if (!http) {
        return;
    }
    /* The same host over plain http: only allow_http sections, and only for IPv4. */
    memmove(text + 4, text + 5, strlen(text + 5) + 1);
    check(xc_url_parse(text, strlen(text), &u) == XC_OK);
    const xc_cred_section *h = xc_cred_for_url(c, &u);
    check(h == NULL || (h == s && s->allow_http && u.is_ipv4));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_credentials c;
    xc_cred_error err;
    xc_err e = xc_cred_parse(data, size, &c, &err);
    if (e != XC_OK) {
        check(e == XC_ERR_CREDENTIALS && c.section_count == 0 && c.text == NULL);
        check(memchr(err.message, '\0', sizeof err.message) != NULL);
        return 0;
    }
    check(c.section_count <= XC_CRED_MAX_SECTIONS);
    for (size_t i = 0; i < c.section_count; i++) {
        const xc_cred_section *s = &c.sections[i];
        size_t name_len = strlen(s->name);
        check(name_len >= 1 && name_len <= XC_CRED_MAX_NAME);
        check(s->pattern_count >= 1 && s->pattern_count <= XC_CRED_MAX_PATTERNS);
        check(s->header_count <= XC_CRED_MAX_HEADERS);
        check(s->cookie != NULL || s->header_count > 0 || s->smb_user != NULL);
        check((s->smb_user == NULL) == (s->smb_nthash == NULL));
        check(s->smb_nthash == NULL || xc_smb_valid_nthash(s->smb_nthash));
        check(s->smb_domain == NULL || s->smb_user != NULL);
        check_headers(s);
        for (size_t k = 0; k < s->pattern_count; k++) {
            check_selects(&c, s, &s->patterns[k]);
        }
    }
    xc_cred_free(&c);
    return 0;
}
