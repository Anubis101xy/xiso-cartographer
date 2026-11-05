/* Tests for the credentials file (core/src/credentials.c, docs/formats/credentials.md). */
#include "unity.h"
#include "xcat/credentials.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR "# xcat-credentials v1\n"

static xc_credentials creds;
static xc_cred_error cerr;

void setUp(void)
{
    memset(&creds, 0, sizeof creds);
}

void tearDown(void)
{
    xc_cred_free(&creds);
}

static xc_err parse(const char *text)
{
    xc_cred_free(&creds);
    return xc_cred_parse(text, strlen(text), &creds, &cerr);
}

static void expect_ok(const char *text)
{
    xc_err err = parse(text);
    if (err != XC_OK) {
        char msg[256];
        (void)snprintf(msg, sizeof msg, "line %zu: %s", cerr.line, cerr.message);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* Rejected at `line`, with `what` in the message, and never echoing "SECRET". */
static void expect_reject(const char *text, size_t line, const char *what)
{
    TEST_ASSERT_EQUAL_INT(XC_ERR_CREDENTIALS, parse(text));
    TEST_ASSERT_EQUAL_size_t(0, creds.section_count);
    TEST_ASSERT_NULL(creds.text);
    if (strstr(cerr.message, what) == NULL || cerr.line != line) {
        char msg[300];
        (void)snprintf(msg, sizeof msg, "got line %zu: \"%s\"; wanted line %zu with \"%s\"",
                       cerr.line, cerr.message, line, what);
        TEST_FAIL_MESSAGE(msg);
    }
    TEST_ASSERT_NULL_MESSAGE(strstr(cerr.message, "SECRET"), "a value leaked into a message");
}

static xc_url url(const char *text)
{
    xc_url u;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_url_parse(text, strlen(text), &u));
    return u;
}

/* The section name chosen for `u`, or "-" for none. */
static const char *who(const char *u)
{
    xc_url x = url(u);
    const xc_cred_section *s = xc_cred_for_url(&creds, &x);
    return s != NULL ? s->name : "-";
}

/* --- syntax ------------------------------------------------------------------------------ */

static void test_spec_example(void)
{
    expect_ok("# xcat-credentials v1\n"
              "\n"
              "# Session for my own archive.org account\n"
              "[archive]\n"
              "hosts  = archive.org, *.archive.org\n"
              "cookie = logged-in-user=u; logged-in-sig=SECRET\n"
              "\n"
              "# NAS on the LAN that uses Basic auth over plain HTTP\n"
              "[nas]\n"
              "hosts      = 192.168.1.20:8080\n"
              "allow_http = yes\n"
              "header     = Authorization: Basic SECRET\n");
    TEST_ASSERT_EQUAL_size_t(2, creds.section_count);
    const xc_cred_section *a = &creds.sections[0];
    TEST_ASSERT_EQUAL_STRING("archive", a->name);
    TEST_ASSERT_EQUAL_size_t(4, a->line);
    TEST_ASSERT_EQUAL_size_t(2, a->pattern_count);
    TEST_ASSERT_EQUAL_STRING("archive.org", a->patterns[0].host);
    TEST_ASSERT_FALSE(a->patterns[0].wildcard);
    TEST_ASSERT_TRUE(a->patterns[0].any_port);
    TEST_ASSERT_EQUAL_STRING("archive.org", a->patterns[1].host);
    TEST_ASSERT_TRUE(a->patterns[1].wildcard);
    TEST_ASSERT_EQUAL_STRING("logged-in-user=u; logged-in-sig=SECRET", a->cookie);
    TEST_ASSERT_EQUAL_size_t(0, a->header_count);
    TEST_ASSERT_FALSE(a->allow_http);
    const xc_cred_section *n = &creds.sections[1];
    TEST_ASSERT_EQUAL_STRING("nas", n->name);
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", n->patterns[0].host);
    TEST_ASSERT_FALSE(n->patterns[0].any_port);
    TEST_ASSERT_EQUAL_UINT16(8080, n->patterns[0].port);
    TEST_ASSERT_TRUE(n->allow_http);
    TEST_ASSERT_NULL(n->cookie);
    TEST_ASSERT_EQUAL_size_t(1, n->header_count);
    TEST_ASSERT_EQUAL_STRING("Authorization", n->header_names[0]);
    TEST_ASSERT_EQUAL_STRING("Basic SECRET", n->header_values[0]);
}

static void test_lexical_details(void)
{
    /* BOM, CRLF, leading blank lines, tabs, a minor version, '#' inside values. */
    expect_ok("\xEF\xBB\xBF\r\n\r\n#\txcat-credentials \t v1.7\r\n"
              "  [Mixed_Case-1]  \r\n"
              "\thosts\t=\tExample.ORG:443\t\r\n"
              "cookie=a=1; b=#2\r\n");
    TEST_ASSERT_EQUAL_size_t(1, creds.section_count);
    TEST_ASSERT_EQUAL_STRING("Mixed_Case-1", creds.sections[0].name);
    TEST_ASSERT_EQUAL_STRING("example.org", creds.sections[0].patterns[0].host);
    TEST_ASSERT_EQUAL_UINT16(443, creds.sections[0].patterns[0].port);
    TEST_ASSERT_EQUAL_STRING("a=1; b=#2", creds.sections[0].cookie);
    /* No sections at all is a valid, empty file; so is a header with no final newline. */
    expect_ok("# xcat-credentials v1");
    TEST_ASSERT_EQUAL_size_t(0, creds.section_count);
    /* Several headers, an empty header value, no space after the colon. */
    expect_ok(HDR "[a]\nhosts=a.example\nheader=X-One:1\nheader = X-Two:\nheader=x-three:  v  \n");
    TEST_ASSERT_EQUAL_size_t(3, creds.sections[0].header_count);
    TEST_ASSERT_EQUAL_STRING("1", creds.sections[0].header_values[0]);
    TEST_ASSERT_EQUAL_STRING("", creds.sections[0].header_values[1]);
    TEST_ASSERT_EQUAL_STRING("v", creds.sections[0].header_values[2]);
}

static void test_header_line(void)
{
    expect_reject("", 0, "missing");
    expect_reject("\n\n  \n", 0, "missing");
    expect_reject("[a]\nhosts=x.org\ncookie=c\n", 1, "missing");
    expect_reject("# xcat-map v1\n", 1, "missing");
    expect_reject("#xcat-credentials v1\n", 1, "missing");
    expect_reject("# xcat-credentials 1\n", 1, "missing");
    expect_reject("# xcat-credentials v01\n", 1, "missing");
    expect_reject("# xcat-credentials v1.\n", 1, "missing");
    expect_reject("# xcat-credentials v1 extra\n", 1, "missing");
    expect_reject("# XCAT-CREDENTIALS v1\n", 1, "missing");
    expect_reject("\n# xcat-credentials v2\n", 2,
                  "newer xiso-cartographer (credentials format v2)");
    expect_reject("# xcat-credentials v0.9\n", 1, "unsupported");
}

static void test_rejections(void)
{
    static const struct {
        const char *text;
        size_t line;
        const char *what;
    } cases[] = {
        {HDR "hosts = a.org\n", 2, "outside a [section]"},
        {HDR "[a]\nhosts a.org\n", 3, "key = value"},
        {HDR "[a\n", 2, "malformed section"},
        {HDR "[]\n", 2, "1-32 characters"},
        {HDR "[a b]\n", 2, "invalid section name"},
        {HDR "[a.b]\n", 2, "invalid section name"},
        {HDR "[abcdefghijabcdefghijabcdefghijabc]\n", 2, "1-32 characters"},
        {HDR "[a]\nhosts=a.org\ncookie=c\n[A]\nhosts=b.org\ncookie=c\n", 5, "appears twice"},
        {HDR "[a]\ncookie=SECRET\n", 2, "[a] has no hosts"},
        {HDR "[a]\nhosts=a.org\n[b]\nhosts=b.org\ncookie=c\n", 2,
         "neither cookie, header nor smb_user"},
        {HDR "[a]\nhosts=a.org\ncookie=c\nhosts=b.org\n", 5, "hosts given twice"},
        {HDR "[a]\nhosts=a.org\ncookie=SECRET\ncookie=SECRET\n", 5, "cookie given twice"},
        {HDR "[a]\nhosts=a.org\ncookie=\n", 4, "empty cookie"},
        {HDR "[a]\nhosts=a.org\ncookie=SECRET\nCookie=SECRET\n", 5, "unknown key"},
        {HDR "[a]\nhosts=a.org\ncookie=SECRET\npassword=SECRET\n", 5, "unknown key"},
        {HDR "[a]\nhosts=a.org\ncookie=SE\x7F"
             "CRET\n",
         4, "control character"},
        {HDR "[a]\nhosts=a.org\ncookie=SE\tCRET\n", 4, "control character"},
        {HDR "[a]\nhosts=a.org\nheader=SECRET\n", 4, "Name: value"},
        {HDR "[a]\nhosts=a.org\nheader=: SECRET\n", 4, "Name: value"},
        {HDR "[a]\nhosts=a.org\nheader=Bad Name: SECRET\n", 4, "invalid header name"},
        {HDR "[a]\nhosts=a.org\nheader=Bad\"Name: SECRET\n", 4, "invalid header name"},
        {HDR "[a]\nhosts=a.org\nheader=host: SECRET\n", 4, "Host can't be set"},
        {HDR "[a]\nhosts=a.org\nheader=Content-Length: 1\n", 4, "Content-Length can't"},
        {HDR "[a]\nhosts=a.org\nheader=Transfer-Encoding: x\n", 4, "Transfer-Encoding can't"},
        {HDR "[a]\nhosts=a.org\nheader=CONNECTION: x\n", 4, "Connection can't"},
        {HDR "[a]\nhosts=a.org\nheader=Range: x\n", 4, "Range can't"},
        {HDR "[a]\nhosts=a.org\nheader=Cookie: SECRET\n", 4, "Cookie can't"},
        {HDR "[a]\nhosts=a.org\nheader=X-A: 1\nheader=x-a: 2\n", 5, "X-A given twice"},
        {HDR "[a]\nhosts=a.org\ncookie=c\nallow_http=true\n", 5, "yes or no"},
        {HDR "[a]\nhosts=a.org\ncookie=c\nallow_http=no\nallow_http=no\n", 6, "allow_http given"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        expect_reject(cases[i].text, cases[i].line, cases[i].what);
    }
}

static void test_host_patterns(void)
{
    static const char *const good[] = {
        "a.org",      "*.a.org",   "x-y.a.org:8080", "192.168.1.20",
        "10.0.0.1:1", "localhost", "*.co.uk",        "A.ORG:65535",
    };
    static const char *const bad[] = {
        "*",         "*.",      "a.*.org",    "*a.org",  "a*.org",  "**.a.org",    "*.*.a.org",
        "a..org",    ".a.org",  "a.org.",     "a.org:",  "a.org:0", "a.org:65536", ":80",
        "a.org:8:9", "[::1]",   "user@a.org", "a.org/x", "1.2.3",   "1.2.3.256",   "01.2.3.4",
        "*.1.2.3.4", "a_b.org", "a.1org",     "a org",
    };
    char text[256];
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        (void)snprintf(text, sizeof text, HDR "[s]\nhosts = %s\ncookie = c\n", good[i]);
        expect_ok(text);
    }
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        (void)snprintf(text, sizeof text, HDR "[s]\nhosts = %s\ncookie = c\n", bad[i]);
        expect_reject(text, 3, "invalid host pattern");
    }
    expect_reject(HDR "[s]\nhosts = a.org,,b.org\ncookie = c\n", 3, "invalid host pattern");
    expect_reject(HDR "[s]\nhosts = a.org,\ncookie = c\n", 3, "invalid host pattern");
    expect_reject(HDR "[s]\nhosts =\ncookie = c\n", 3, "invalid host pattern");
}

static void test_overlaps(void)
{
    static const struct {
        const char *a;
        const char *b;
        bool overlap;
    } cases[] = {
        {"a.org", "a.org", true},
        {"a.org", "A.Org", true},
        {"a.org", "b.org", false},
        {"*.a.org", "a.org", false}, /* the wildcard excludes the apex */
        {"*.a.org", "x.a.org", true},
        {"*.a.org", "x.y.a.org", true},
        {"*.a.org", "xa.org", false},
        {"*.a.org", "*.x.a.org", true},
        {"*.x.a.org", "*.a.org", true},
        {"*.a.org", "*.b.org", false},
        {"a.org:80", "a.org:81", false},
        {"a.org:80", "a.org", true},
        {"a.org", "a.org:80", true},
        {"*.a.org:443", "x.a.org:443", true},
        {"*.a.org:443", "x.a.org:80", false},
        {"192.168.1.2", "192.168.1.2:80", true},
        {"192.168.1.2", "192.168.1.3", false},
    };
    char text[256];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        (void)snprintf(text, sizeof text,
                       HDR "[one]\nhosts = %s\ncookie = c\n[two]\nhosts = z.example, %s\n"
                           "cookie = c\n",
                       cases[i].a, cases[i].b);
        if (cases[i].overlap) {
            expect_reject(text, 5, "hosts of [two] overlap with [one]");
        } else {
            expect_ok(text);
        }
    }
    /* Overlap within one section is harmless. */
    expect_ok(HDR "[a]\nhosts = a.org, a.org, *.a.org, x.a.org\ncookie = c\n");
}

static void test_limits(void)
{
    char *big = malloc(XC_CRED_MAX_FILE + 2);
    TEST_ASSERT_NOT_NULL(big);
    /* Exactly 64 KiB is fine, one byte more isn't. */
    memset(big, '#', XC_CRED_MAX_FILE + 1);
    memcpy(big, HDR, sizeof HDR - 1); /* no terminator: the buffer is passed with a length */
    for (size_t i = strlen(HDR) + 100; i < XC_CRED_MAX_FILE; i += 101) {
        big[i] = '\n';
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_cred_parse(big, XC_CRED_MAX_FILE, &creds, &cerr));
    xc_cred_free(&creds);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CREDENTIALS,
                          xc_cred_parse(big, XC_CRED_MAX_FILE + 1, &creds, &cerr));
    TEST_ASSERT_NOT_NULL(strstr(cerr.message, "64 KiB"));
    /* Line length: 4096 bytes (without CRLF) is fine, 4097 isn't. */
    size_t n = (size_t)snprintf(big, XC_CRED_MAX_FILE, HDR "#");
    memset(big + n, 'x', XC_CRED_MAX_LINE - 1);
    memcpy(big + n + XC_CRED_MAX_LINE - 1, "\r\n", 3);
    expect_ok(big);
    memcpy(big + n + XC_CRED_MAX_LINE - 1, "x\n", 3);
    expect_reject(big, 2, "longer than 4096");
    /* 32 sections, 16 headers, 16 patterns; one more of each fails. */
    for (int extra = 0; extra <= 1; extra++) {
        size_t len = (size_t)snprintf(big, XC_CRED_MAX_FILE, HDR);
        for (int s = 0; s < XC_CRED_MAX_SECTIONS + extra; s++) {
            len += (size_t)snprintf(big + len, XC_CRED_MAX_FILE - len,
                                    "[s%d]\nhosts=h%d.example\ncookie=c\n", s, s);
        }
        if (extra == 0) {
            expect_ok(big);
        } else {
            expect_reject(big, 2 + 3 * XC_CRED_MAX_SECTIONS, "more than 32 sections");
        }
        len = (size_t)snprintf(big, XC_CRED_MAX_FILE, HDR "[a]\nhosts=a.example\n");
        for (int h = 0; h < XC_CRED_MAX_HEADERS + extra; h++) {
            len += (size_t)snprintf(big + len, XC_CRED_MAX_FILE - len, "header=X-%d: v\n", h);
        }
        if (extra == 0) {
            expect_ok(big);
        } else {
            expect_reject(big, 4 + XC_CRED_MAX_HEADERS, "more than 16 headers");
        }
        len = (size_t)snprintf(big, XC_CRED_MAX_FILE, HDR "[a]\ncookie=c\nhosts=");
        for (int h = 0; h < XC_CRED_MAX_PATTERNS + extra; h++) {
            len += (size_t)snprintf(big + len, XC_CRED_MAX_FILE - len, "%sh%d.example",
                                    h > 0 ? "," : "", h);
        }
        if (extra == 0) {
            expect_ok(big);
        } else {
            expect_reject(big, 4, "more than 16 host patterns");
        }
    }
    free(big);
}

static void test_encoding(void)
{
    expect_ok(HDR
              "# caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\n[a]\nhosts=a.org\ncookie=\xC3\xA9\n");
    expect_reject(HDR "\n# \xC3\n", 3, "not UTF-8");
    expect_reject(HDR "# \xC0\xAF\n", 2, "not UTF-8");         /* overlong '/' */
    expect_reject(HDR "# \xED\xA0\x80\n", 2, "not UTF-8");     /* surrogate */
    expect_reject(HDR "# \xF4\x90\x80\x80\n", 2, "not UTF-8"); /* above U+10FFFF */
    expect_reject(HDR "# \xFF\n", 2, "not UTF-8");
    static const char with_nul[] = HDR "[a]\nhosts=a.org\ncookie=SE\0CRET\n";
    TEST_ASSERT_EQUAL_INT(XC_ERR_CREDENTIALS,
                          xc_cred_parse(with_nul, sizeof with_nul - 1, &creds, &cerr));
    TEST_ASSERT_EQUAL_size_t(4, cerr.line);
}

/* --- §6: which section goes with which hop ---------------------------------------------- */

static void test_scope(void)
{
    expect_ok(HDR "[archive]\nhosts = Archive.org, *.archive.org\ncookie = c\n"
                  "[nas]\nhosts = 192.168.1.20:8080, 10.1.2.3, 172.16.0.1, 172.31.255.255, "
                  "172.15.0.1, 172.32.0.1, 8.8.8.8, nas.lan\nallow_http = yes\nheader = A: b\n"
                  "[strict]\nhosts = 192.168.1.30\nheader = A: b\n");
    /* https: host and port decide. */
    TEST_ASSERT_EQUAL_STRING("archive", who("https://archive.org/x"));
    TEST_ASSERT_EQUAL_STRING("archive", who("https://ARCHIVE.org:8443/x"));
    TEST_ASSERT_EQUAL_STRING("archive", who("https://dn720004.ca.archive.org/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("https://evilarchive.org/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("https://archive.org.evil.com/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("https://example.com/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("https://192.168.1.20:8080/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("https://192.168.1.20/x")); /* wrong port */
    TEST_ASSERT_EQUAL_STRING("-", who("https://192.168.1.21:8080/x"));
    /* http: only with allow_http, and only to private IPv4 literals. */
    TEST_ASSERT_EQUAL_STRING("-", who("http://archive.org/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://ia800.us.archive.org/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("http://192.168.1.20:8080/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("http://10.1.2.3/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("http://172.16.0.1/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("http://172.31.255.255/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://172.15.0.1/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://172.32.0.1/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://8.8.8.8/x"));
    TEST_ASSERT_EQUAL_STRING("nas", who("https://8.8.8.8/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://nas.lan/x")); /* a name, even on the LAN */
    TEST_ASSERT_EQUAL_STRING("nas", who("https://nas.lan/x"));
    TEST_ASSERT_EQUAL_STRING("-", who("http://192.168.1.30/x")); /* no allow_http */
    TEST_ASSERT_EQUAL_STRING("strict", who("https://192.168.1.30/x"));
}

static void test_headers_output(void)
{
    expect_ok(HDR "[a]\nhosts = a.org\ncookie = k=v; s=t\nheader = Authorization: Bearer x\n"
                  "header = X-Two: 2\n");
    char out[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_cred_headers(&creds.sections[0], out, sizeof out, &len));
    const char *want = "Cookie: k=v; s=t\r\nAuthorization: Bearer x\r\nX-Two: 2\r\n";
    TEST_ASSERT_EQUAL_size_t(strlen(want), len);
    TEST_ASSERT_EQUAL_MEMORY(want, out, len);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_cred_headers(&creds.sections[0], out, strlen(want), &len));
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT,
                          xc_cred_headers(&creds.sections[0], out, strlen(want) - 1, &len));
    TEST_ASSERT_EQUAL_size_t(0, len);
}

static void test_http_auth_adapter(void)
{
    expect_ok(HDR "[archive]\nhosts = archive.org, *.archive.org\ncookie = sig=1\n");
    xc_http_auth auth;
    xc_cred_http_auth(&auth, &creds);
    char out[64];
    size_t len = 99;
    const char *section = "x";
    /* A redirect chain: in scope, out of scope, downgraded to http. Decided per hop. */
    xc_url hop = url("https://archive.org/download/item/f.iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, auth.headers(auth.ctx, &hop, out, sizeof out, &len, &section));
    TEST_ASSERT_EQUAL_STRING("archive", section);
    TEST_ASSERT_EQUAL_MEMORY("Cookie: sig=1\r\n", out, len);
    hop = url("https://cdn.example.net/f.iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, auth.headers(auth.ctx, &hop, out, sizeof out, &len, &section));
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_NULL(section);
    hop = url("http://ia800.us.archive.org/f.iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, auth.headers(auth.ctx, &hop, out, sizeof out, &len, &section));
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_NULL(section);
    hop = url("https://ia800.us.archive.org/f.iso");
    TEST_ASSERT_EQUAL_INT(XC_OK, auth.headers(auth.ctx, &hop, out, sizeof out, &len, &section));
    TEST_ASSERT_EQUAL_STRING("archive", section);
    /* Too little room is an error, never a truncated header. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, auth.headers(auth.ctx, &hop, out, 4, &len, &section));
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_NULL(section);
    /* An empty file sends nothing anywhere. */
    expect_ok(HDR);
    xc_cred_http_auth(&auth, &creds);
    TEST_ASSERT_EQUAL_INT(XC_OK, auth.headers(auth.ctx, &hop, out, sizeof out, &len, &section));
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_NULL(section);
}

/* §4.1: SMB logins, and §5's overlap rule kept per kind of server. */
static void test_smb_logins(void)
{
    expect_ok(HDR "[nas]\nhosts = 192.168.1.50\nsmb_user = shaun\n"
                  "smb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n"
                  "[web]\nhosts = 192.168.1.50\nheader = Authorization: Basic eDp5\n"
                  "allow_http = yes\n"
                  "[office]\nhosts = files.corp.lan:4450\nsmb_user = Shaun D\n"
                  "smb_domain = CORP\nsmb_nthash = 31d6cfe0d16ae931b73c59d7e0c089c0\n");
    TEST_ASSERT_EQUAL_size_t(3, creds.section_count);
    const xc_cred_section *s = xc_cred_for_smb(&creds, "192.168.1.50", 445);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("nas", s->name);
    TEST_ASSERT_EQUAL_STRING("shaun", s->smb_user);
    TEST_ASSERT_NULL(s->smb_domain);
    TEST_ASSERT_EQUAL_STRING("8846F7EAEE8FB117AD06BDD830B7586C", s->smb_nthash);
    /* HTTP to the same NAS gets the HTTP section, never the SMB one. */
    xc_url u = url("http://192.168.1.50:8080/a.iso");
    s = xc_cred_for_url(&creds, &u);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("web", s->name);
    s = xc_cred_for_smb(&creds, "files.corp.lan", 4450);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("CORP", s->smb_domain);
    TEST_ASSERT_EQUAL_STRING("Shaun D", s->smb_user);
    TEST_ASSERT_NULL(xc_cred_for_smb(&creds, "files.corp.lan", 445)); /* another port */
    TEST_ASSERT_NULL(xc_cred_for_smb(&creds, "192.168.1.51", 445));
    /* An SMB-only section is never sent over HTTP. */
    u = url("https://files.corp.lan:4450/x");
    TEST_ASSERT_NULL(xc_cred_for_url(&creds, &u));
    /* Wildcards match host names for SMB too. */
    expect_ok(HDR
              "[w]\nhosts = *.lan\nsmb_user = a\nsmb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n");
    TEST_ASSERT_NOT_NULL(xc_cred_for_smb(&creds, "tnas.lan", 445));
    TEST_ASSERT_NULL(xc_cred_for_smb(&creds, "lan", 445));

    expect_reject(HDR "[a]\nhosts = nas\nsmb_user = u\n", 2, "smb_user and smb_nthash go together");
    expect_reject(HDR "[a]\nhosts = nas\nsmb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n", 2,
                  "smb_user and smb_nthash go together");
    expect_reject(HDR "[a]\nhosts = nas\ncookie = c\nsmb_domain = D\n", 2,
                  "smb_domain without smb_user");
    expect_reject(HDR "[a]\nhosts = nas\nsmb_user = u\nsmb_nthash = SECRET\n", 5,
                  "smb_nthash must be 32 hex digits");
    expect_reject(HDR "[a]\nhosts = nas\nsmb_user = dom\\u\n"
                      "smb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n",
                  4, "invalid smb_user");
    expect_reject(HDR "[a]\nhosts = nas\nsmb_user = u\nsmb_user = v\n", 5, "smb_user given twice");
    expect_reject(HDR
                  "[a]\nhosts = nas\nsmb_user = u\nsmb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n"
                  "[b]\nhosts = NAS\nsmb_user = v\nsmb_nthash = 31d6cfe0d16ae931b73c59d7e0c089c0\n",
                  6, "overlap");
}

static void test_smb_section_writer(void)
{
    char out[512];
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_cred_smb_section("nas-192-168-1-50", "192.168.1.50", "shaun", NULL,
                                              "8846F7EAEE8FB117AD06BDD830B7586C", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("[nas-192-168-1-50]\nhosts = 192.168.1.50\nsmb_user = shaun\n"
                             "smb_nthash = 8846F7EAEE8FB117AD06BDD830B7586C\n",
                             out);
    char file[600];
    (void)snprintf(file, sizeof file, HDR "%s", out);
    expect_ok(file);
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_cred_smb_section("n", "nas:4450", "u", "WORKGROUP",
                                              "8846F7EAEE8FB117AD06BDD830B7586C", out, sizeof out));
    TEST_ASSERT_NOT_NULL(strstr(out, "smb_domain = WORKGROUP\n"));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_cred_smb_section("bad name", "nas", "u", NULL,
                                              "8846F7EAEE8FB117AD06BDD830B7586C", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_cred_smb_section("n", "nas", "u\nx", NULL,
                                              "8846F7EAEE8FB117AD06BDD830B7586C", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_cred_smb_section("n", "a,b", "u", NULL,
                                              "8846F7EAEE8FB117AD06BDD830B7586C", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_cred_smb_section("n", "nas", "u", NULL, "1234", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(
        XC_ERR_LIMIT,
        xc_cred_smb_section("n", "nas", "u", NULL, "8846F7EAEE8FB117AD06BDD830B7586C", out, 20));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spec_example);
    RUN_TEST(test_lexical_details);
    RUN_TEST(test_header_line);
    RUN_TEST(test_rejections);
    RUN_TEST(test_host_patterns);
    RUN_TEST(test_overlaps);
    RUN_TEST(test_limits);
    RUN_TEST(test_encoding);
    RUN_TEST(test_scope);
    RUN_TEST(test_headers_output);
    RUN_TEST(test_http_auth_adapter);
    RUN_TEST(test_smb_logins);
    RUN_TEST(test_smb_section_writer);
    return UNITY_END();
}
