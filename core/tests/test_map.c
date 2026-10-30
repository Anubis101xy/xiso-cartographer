/* Tests for the map loader (core/src/map.c, docs/formats/map.md). */
#include "unity.h"
#include "xcat/archiveorg.h"
#include "xcat/map.h"
#include "xcat/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR "# xcat-map v1\n"
#define U1 "http://192.168.1.20:8080/a.iso"

static xc_map map;

/* --- a fake file system for @include ------------------------------------------------------ */

typedef struct ffile {
    const char *location;
    const char *text;
} ffile;

static const ffile *files;
static size_t nfiles;
static int fetches;
static char fetched[64][256];

static xc_err fake_fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **data,
                         size_t *len)
{
    (void)ctx;
    bool listing = strncmp(location, "https://archive.org/metadata/", 29) == 0;
    TEST_ASSERT_TRUE(!listing || is_url);
    TEST_ASSERT_EQUAL_size_t(listing ? XC_ARCHIVEORG_MAX_METADATA : XC_MAP_MAX_FILE, max_len);
    if (fetches < 64) {
        (void)snprintf(fetched[fetches], sizeof fetched[0], "%s", location);
    }
    fetches++;
    for (size_t i = 0; i < nfiles; i++) {
        if (strcmp(files[i].location, location) == 0) {
            size_t n = strlen(files[i].text);
            if (n > max_len) {
                return XC_ERR_LIMIT;
            }
            *data = malloc(n == 0 ? 1 : n); /* exactly the size: the loader must not assume more */
            TEST_ASSERT_NOT_NULL(*data);
            memcpy(*data, files[i].text, n);
            *len = n;
            return XC_OK;
        }
    }
    return XC_ERR_IO;
}

/* --- a fake NAS for @smb ----------------------------------------------------------------- */

typedef struct fnas {
    const char *folder; /* canonical smb:// URL */
    const char *const *paths;
    xc_err err;
} fnas;

static const fnas *nases;
static size_t nnases;
static int listings;

static xc_err fake_list(void *ctx, const xc_smb_url *folder, xc_smb_listing *out)
{
    (void)ctx;
    char where[XC_SMB_MAX_URL];
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_format(folder, where, sizeof where));
    listings++;
    for (size_t i = 0; i < nnases; i++) {
        if (strcmp(nases[i].folder, where) == 0) {
            if (nases[i].err != XC_OK) {
                (void)xc_smb_listing_add(out, "half.iso", 1); /* the map must free it */
                return nases[i].err;
            }
            for (size_t k = 0; nases[i].paths[k] != NULL; k++) {
                const char *p = nases[i].paths[k];
                size_t n = strlen(p);
                if (n > 0 && p[n - 1] == '/') { /* a game folder (smb.md §3.1) */
                    char game[256];
                    (void)snprintf(game, sizeof game, "%.*s", (int)(n - 1), p);
                    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_listing_add_game(out, game));
                } else {
                    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_listing_add(out, p, 1000 + k));
                }
            }
            return XC_OK;
        }
    }
    return XC_ERR_NET_CONNECT;
}

static const xc_map_loader loader = {fake_fetch, NULL, fake_list};

static void set_files(const ffile *f, size_t n)
{
    files = f;
    nfiles = n;
    fetches = 0;
}

static void set_nases(const fnas *f, size_t n)
{
    nases = f;
    nnases = n;
    listings = 0;
}

void setUp(void)
{
    memset(&map, 0, sizeof map);
    set_files(NULL, 0);
    set_nases(NULL, 0);
}

void tearDown(void)
{
    xc_map_free(&map);
}

static xc_err load_at(const char *text, const char *location)
{
    xc_map_free(&map);
    return xc_map_load(text, strlen(text), location, &loader, &map);
}

static xc_err load(const char *text)
{
    return load_at(text, "E:\\xcat\\map.txt");
}

/* Some problem has this severity, line and message fragment. */
static void expect_problem(xc_map_severity sev, size_t line, const char *what)
{
    for (size_t i = 0; i < map.problem_count; i++) {
        const xc_map_problem *p = &map.problems[i];
        if (p->severity == sev && p->line == line && strstr(p->message, what) != NULL) {
            return;
        }
    }
    char msg[512];
    int n = snprintf(msg, sizeof msg, "no %s on line %zu with \"%s\"; have:",
                     sev == XC_MAP_ERROR ? "error" : "warning", line, what);
    for (size_t i = 0; i < map.problem_count && n > 0 && (size_t)n < sizeof msg; i++) {
        n += snprintf(msg + n, sizeof msg - (size_t)n, " [%zu: %s]", map.problems[i].line,
                      map.problems[i].message);
    }
    TEST_FAIL_MESSAGE(msg);
}

static const char *url_of(uint32_t id, const char *variant)
{
    const xc_map_entry *e = xc_map_find(&map, id, variant);
    return e != NULL ? e->url : "-";
}

/* --- §1-§4 ---------------------------------------------------------------------------------- */

static void test_spec_example(void)
{
    static const ffile inc[] = {
        {"E:\\xcat\\more.txt", HDR "4D530064 = http://192.168.1.20:8080/xbox/Halo 2.iso\n"},
        {"https://archive.org/metadata/some-item_1.0",
         "{\"files\":[{\"name\":\"Game (USA).zip\",\"source\":\"original\",\"size\":\"5\"}]}"},
    };
    set_files(inc, 2);
    TEST_ASSERT_EQUAL_INT(
        XC_OK, load("# xcat-map v1\n# Lines starting with # are comments.\n\n"
                    "4D530004       = https://archive.org/download/item/file.xiso.iso\n"
                    "4d530004@pal   = http://192.168.1.20:8080/xbox/Halo (Europe).xiso.iso\n"
                    "@include more.txt\n"
                    "@archiveorg some-item_1.0\n"));
    TEST_ASSERT_EQUAL_size_t(4, map.entry_count);
    TEST_ASSERT_EQUAL_size_t(0, map.error_count);
    TEST_ASSERT_EQUAL_size_t(0, map.problem_count);
    const xc_map_entry *g = xc_map_find_name(&map, "game (usa)");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/some-item_1.0/Game%20%28USA%29.zip",
                             g->url);
    TEST_ASSERT_EQUAL_size_t(7, g->line);
    TEST_ASSERT_EQUAL_size_t(0, g->source);
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/item/file.xiso.iso",
                             url_of(0x4D530004U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8080/xbox/Halo (Europe).xiso.iso",
                             url_of(0x4D530004U, "pal"));
    TEST_ASSERT_EQUAL_STRING("PAL", map.entries[1].variant);
    TEST_ASSERT_EQUAL_size_t(5, map.entries[1].line);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8080/xbox/Halo 2.iso", url_of(0x4D530064U, ""));
    TEST_ASSERT_EQUAL_size_t(2, map.source_count);
    TEST_ASSERT_EQUAL_STRING("E:\\xcat\\more.txt", map.sources[1]);
    TEST_ASSERT_EQUAL_size_t(1, map.entries[2].source);
    /* A variant without its own entry falls back to the default; another title has none. */
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/item/file.xiso.iso",
                             url_of(0x4D530004U, "NTSC-J"));
    TEST_ASSERT_EQUAL_STRING("-", url_of(0x12345678U, NULL));
    TEST_ASSERT_EQUAL_STRING("-", url_of(0x4D530064U - 1U, "PAL"));
}

static void test_lines_and_header(void)
{
    /* BOM, CRLF, leading blank lines, tabs, a minor version, '#' and '=' later in a URL. */
    TEST_ASSERT_EQUAL_INT(XC_OK, load("\xEF\xBB\xBF\r\n \t\r\n\t#\txcat-map  v1.4 \r\n"
                                      "\t0000ABCD=http://h.lan/x%23y.iso?a=b \r\n"
                                      "00000001 = http://h.lan/last-line-no-newline.iso"));
    TEST_ASSERT_EQUAL_size_t(2, map.entry_count);
    TEST_ASSERT_EQUAL_STRING("http://h.lan/x%23y.iso?a=b", url_of(0xABCDU, NULL));
    TEST_ASSERT_EQUAL_size_t(4, map.entries[0].line);
    TEST_ASSERT_EQUAL_STRING("http://h.lan/last-line-no-newline.iso", url_of(1U, NULL));
    /* File errors reject everything (§7). */
    static const char *const bad_headers[] = {
        "",
        "\n\n",
        "0000ABCD = http://192.168.1.20:8080/a.iso\n",
        "# xcat-credentials v1\n",
        "#xcat-map v1\n",
        "# xcat-map 1\n",
        "# xcat-map v01\n",
        "# xcat-map v1.\n",
        "# xcat-map v1 x\n",
        "# XCAT-MAP v1\n",
    };
    for (size_t i = 0; i < sizeof bad_headers / sizeof bad_headers[0]; i++) {
        TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, load(bad_headers[i]));
        TEST_ASSERT_EQUAL_size_t(0, map.entry_count);
        TEST_ASSERT_EQUAL_size_t(1, map.error_count);
        expect_problem(XC_MAP_ERROR, 0, "missing \"# xcat-map v1\" header");
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, load("# xcat-map v2\n0000ABCD = " U1 "\n"));
    expect_problem(XC_MAP_ERROR, 0, "needs a newer xiso-cartographer (map format v2)");
    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, load("# xcat-map v0.3\n"));
    expect_problem(XC_MAP_ERROR, 0, "unsupported map format v0");
}

static void test_line_errors_keep_loading(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "0000000A = " U1 "\n"                        /* 2 */
                                          "0000000B " U1 "\n"                          /* 3 */
                                          "0000000 = " U1 "\n"                         /* 4 */
                                          "0000000G = " U1 "\n"                        /* 5 */
                                          "0000000B@ = " U1 "\n"                       /* 6 */
                                          "0000000B@ABCDEFGHIJKLMNOPQ = " U1 "\n"      /* 7 */
                                          "0000000B@P.L = " U1 "\n"                    /* 8 */
                                          "0000000B = ftp://h.lan/a.iso\n"             /* 9 */
                                          "0000000B = http://user:pass@h.lan/a.iso\n"  /* 10 */
                                          "0000000B = http://h.lan/a.iso#frag\n"       /* 11 */
                                          "0000000B = http://h.lan/a\x01.iso\n"        /* 12 */
                                          "0000000B = http://h..lan/a.iso\n"           /* 13 */
                                          "0000000B = \n"                              /* 14 */
                                          "0000000B = http://h.lan/caf\xC3.iso\n"      /* 15 */
                                          "@\n"                                        /* 16 */
                                          "@Include x\n"                               /* 17 */
                                          "@include\n"                                 /* 18 */
                                          "@archiveorg bad/id\n"                       /* 19 */
                                          "@frobnicate 1\n"                            /* 20 */
                                          "0000000C@ab_-9 = HTTPS://h.lan/ok.iso\n")); /* 21 */
    TEST_ASSERT_EQUAL_size_t(2, map.entry_count);
    TEST_ASSERT_EQUAL_STRING(U1, url_of(0xAU, NULL));
    TEST_ASSERT_EQUAL_STRING("HTTPS://h.lan/ok.iso", url_of(0xCU, "AB_-9"));
    expect_problem(XC_MAP_ERROR, 3, "expected \"<title-id> = <url>\"");
    for (size_t line = 4; line <= 8; line++) {
        expect_problem(XC_MAP_ERROR, line, "malformed key");
    }
    expect_problem(XC_MAP_ERROR, 9, "must start with http://, https:// or smb://");
    expect_problem(XC_MAP_ERROR, 10, "user credentials");
    expect_problem(XC_MAP_ERROR, 11, "'#'");
    expect_problem(XC_MAP_ERROR, 12, "invalid URL");
    expect_problem(XC_MAP_ERROR, 13, "invalid URL");
    expect_problem(XC_MAP_ERROR, 14, "must start with http");
    expect_problem(XC_MAP_ERROR, 15, "not valid UTF-8");
    expect_problem(XC_MAP_ERROR, 16, "malformed directive");
    expect_problem(XC_MAP_ERROR, 17, "malformed directive");
    expect_problem(XC_MAP_ERROR, 18, "@include needs an argument");
    expect_problem(XC_MAP_ERROR, 19, "invalid archive.org identifier");
    expect_problem(XC_MAP_WARNING, 20, "unknown directive @frobnicate");
    TEST_ASSERT_EQUAL_size_t(17, map.error_count);
    /* Credentials in a URL never reach a message. */
    for (size_t i = 0; i < map.problem_count; i++) {
        TEST_ASSERT_NULL(strstr(map.problems[i].message, "pass"));
    }
}

static void test_zip_urls(void)
{
    /* §4.2: a zip is an entry like any other; a file inside one isn't (ADR 0009). */
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          load(HDR "0000000A = https://h.lan/d/x/Game (USA).zip\n"        /* 2 */
                                   "0000000B = https://h.lan/d/x/Game.ZIP?a=b.zip/c\n"    /* 3 */
                                   "0000000C = https://h.lan/d/x/Game.zip/Game.iso\n"     /* 4 */
                                   "0000000D = https://h.lan/d/x/Game.Zip/sub/Game.iso\n" /* 5 */
                                   "0000000E = https://h.lan/d/x.zip.d/Game.iso\n"        /* 6 */
                                   "0000000F = https://h.lan/d/x/Game.zip/\n"));          /* 7 */
    TEST_ASSERT_EQUAL_size_t(4, map.entry_count);
    TEST_ASSERT_NOT_NULL(url_of(0xAU, NULL));
    TEST_ASSERT_NOT_NULL(url_of(0xBU, NULL));
    TEST_ASSERT_NOT_NULL(url_of(0xEU, NULL)); /* ".zip.d" is a directory name */
    TEST_ASSERT_NOT_NULL(url_of(0xFU, NULL)); /* nothing after the slash */
    expect_problem(XC_MAP_ERROR, 4, "inside a zip");
    expect_problem(XC_MAP_ERROR, 5, "use the URL of the .zip itself");
    TEST_ASSERT_TRUE(xc_zip_is_zip_name("https://h.lan/d/x/Game (USA).ZIP?download=1#x"));
    TEST_ASSERT_TRUE(xc_zip_is_zip_name("E:\\xcat\\isos\\game.zip"));
    TEST_ASSERT_FALSE(xc_zip_is_zip_name("https://h.lan/d/x/Game.zip/Game.iso"));
    TEST_ASSERT_FALSE(xc_zip_is_zip_name("https://h.lan/d/x/Game.iso?f=a.zip"));
    TEST_ASSERT_FALSE(xc_zip_is_zip_name("zip"));
    TEST_ASSERT_FALSE(xc_zip_is_zip_name(NULL));
}

static void test_duplicates(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "0000000A = http://h.lan/1.iso\n"
                                          "0000000a@x = http://h.lan/x.iso\n"
                                          "0000000A = http://h.lan/2.iso\n"
                                          "0000000A@X = http://h.lan/x2.iso\n"));
    TEST_ASSERT_EQUAL_size_t(2, map.entry_count);
    TEST_ASSERT_EQUAL_STRING("http://h.lan/2.iso", url_of(0xAU, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/x2.iso", url_of(0xAU, "x"));
    TEST_ASSERT_EQUAL_size_t(4, map.entries[0].line);
    expect_problem(XC_MAP_WARNING, 4, "0000000A also defined at E:\\xcat\\map.txt:2");
    expect_problem(XC_MAP_WARNING, 5, "0000000A@X also defined at E:\\xcat\\map.txt:3");
    TEST_ASSERT_EQUAL_size_t(0, map.error_count);
}

static void test_lines_too_long_and_files_too_big(void)
{
    char *big = malloc(XC_MAP_MAX_FILE + 2);
    TEST_ASSERT_NOT_NULL(big);
    /* A 2048-byte entry line is fine; 2049 isn't, and only that line is lost. */
    size_t n = (size_t)snprintf(big, XC_MAP_MAX_FILE, HDR "0000000A = http://h.lan/");
    size_t pad = XC_MAP_MAX_LINE - (n - strlen(HDR));
    memset(big + n, 'a', pad);
    memcpy(big + n + pad, "\r\n0000000B = http://h.lan/", 27); /* the NUL is overwritten next */
    size_t m = n + pad + 26;
    memset(big + m, 'b', pad + 1);
    memcpy(big + m + pad + 1, "\n", 2);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(big));
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count);
    TEST_ASSERT_EQUAL_size_t(XC_MAP_MAX_LINE - 11, strlen(map.entries[0].url));
    expect_problem(XC_MAP_ERROR, 3, "longer than 2048");
    /* 1 MiB of comments is fine; one byte more rejects the file. */
    memset(big, '#', XC_MAP_MAX_FILE + 1);
    memcpy(big, HDR, sizeof HDR - 1);
    for (size_t i = sizeof HDR + 99; i < XC_MAP_MAX_FILE; i += 100) {
        big[i] = '\n';
    }
    xc_map_free(&map);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_load(big, XC_MAP_MAX_FILE, "m", &loader, &map));
    xc_map_free(&map);
    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, xc_map_load(big, XC_MAP_MAX_FILE + 1, "m", &loader, &map));
    expect_problem(XC_MAP_ERROR, 0, "larger than 1 MiB");
    free(big);
}

static void test_entry_limit(void)
{
    size_t cap = (size_t)(XC_MAP_MAX_ENTRIES + 10) * 40U + 64U;
    char *text = malloc(cap);
    TEST_ASSERT_NOT_NULL(text);
    size_t n = (size_t)snprintf(text, cap, HDR);
    for (unsigned i = 0; i < XC_MAP_MAX_ENTRIES + 5U; i++) {
        n += (size_t)snprintf(text + n, cap - n, "%08X = http://h.lan/%u.iso\n", i, i);
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, load(text));
    TEST_ASSERT_EQUAL_size_t(XC_MAP_MAX_ENTRIES, map.entry_count);
    expect_problem(XC_MAP_ERROR, XC_MAP_MAX_ENTRIES + 2U, "more than 10000 entries");
    TEST_ASSERT_EQUAL_size_t(1, map.error_count); /* loading stopped: no further problems */
    free(text);
}

static void test_problem_cap(void)
{
    size_t cap = (size_t)(XC_MAP_MAX_PROBLEMS + 50) * 8U + 64U;
    char *text = malloc(cap);
    TEST_ASSERT_NOT_NULL(text);
    size_t n = (size_t)snprintf(text, cap, HDR);
    for (int i = 0; i < XC_MAP_MAX_PROBLEMS + 50; i++) {
        n += (size_t)snprintf(text + n, cap - n, "bad\n");
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, load(text));
    TEST_ASSERT_EQUAL_size_t(XC_MAP_MAX_PROBLEMS, map.problem_count);
    TEST_ASSERT_EQUAL_size_t(50, map.problems_dropped);
    TEST_ASSERT_EQUAL_size_t(XC_MAP_MAX_PROBLEMS + 50U, map.error_count);
    free(text);
}

/* --- §5.1: includes --------------------------------------------------------------------- */

static void test_include_paths(void)
{
    static const ffile fs[] = {
        {"E:\\xcat\\sub\\a.txt", HDR "00000001 = http://h.lan/1.iso\n@include b.txt\n"},
        {"E:\\xcat\\sub\\b.txt", HDR "00000002 = http://h.lan/2.iso\n"},
        {"E:\\xcat\\c.txt", HDR "00000003 = http://h.lan/3.iso\n"},
        {"http://nas.lan:8080/maps/remote.txt", HDR "00000004 = http://h.lan/4.iso\n"
                                                    "@include ../up.txt\n"
                                                    "@include rel/r.txt\n"
                                                    "@include /abs.txt\n"},
        {"http://nas.lan:8080/maps/rel/r.txt", HDR "00000005 = http://h.lan/5.iso\n"},
        {"http://nas.lan:8080/abs.txt", HDR "00000006 = http://h.lan/6.iso\n"},
    };
    set_files(fs, sizeof fs / sizeof fs[0]);
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          load(HDR "@include sub/a.txt\n"                                /* 2 */
                                   "@include c.txt\n"                                    /* 3 */
                                   "@include HTTP://NAS.lan:8080/maps/x/../remote.txt\n" /* 4 */
                                   "@include ..\\up.txt\n"                               /* 5 */
                                   "@include sub/../c.txt\n"                             /* 6 */
                                   "@include \\abs.txt\n"                                /* 7 */
                                   "@include /abs.txt\n"                                 /* 8 */
                                   "@include E:\\abs.txt\n"                              /* 9 */
                                   "@include ftp://nas.lan/m.txt\n"                      /* 10 */
                                   "@include missing.txt\n"));                           /* 11 */
    TEST_ASSERT_EQUAL_STRING("http://h.lan/1.iso", url_of(1U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/2.iso", url_of(2U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/3.iso", url_of(3U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/4.iso", url_of(4U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/5.iso", url_of(5U, NULL));
    TEST_ASSERT_EQUAL_STRING("-", url_of(6U, NULL)); /* "/abs.txt" is absolute, even remotely */
    expect_problem(XC_MAP_ERROR, 5, "must not contain \"..\"");
    expect_problem(XC_MAP_ERROR, 6, "must not contain \"..\"");
    for (size_t line = 7; line <= 10; line++) {
        expect_problem(XC_MAP_ERROR, line, "relative");
    }
    expect_problem(XC_MAP_ERROR, 11, "@include E:\\xcat\\missing.txt: Read or write failed");
    /* Errors inside an include are reported against that file. */
    size_t remote = 0;
    for (size_t i = 0; i < map.source_count; i++) {
        if (strcmp(map.sources[i], "http://nas.lan:8080/maps/remote.txt") == 0) {
            remote = i;
        }
    }
    TEST_ASSERT_TRUE(remote > 0);
    bool found = false;
    for (size_t i = 0; i < map.problem_count; i++) {
        if (map.problems[i].source == remote && map.problems[i].line == 3) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "../up.txt in the remote map is reported there");
}

/* The servers a map uses, each once, entries first: what the Authentication page lists. */
static void test_hosts(void)
{
    static const ffile fs[] = {
        {"http://nas.lan:8080/maps/remote.txt", HDR "00000004 = http://h.lan/4.iso\n"},
    };
    set_files(fs, sizeof fs / sizeof fs[0]);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "00000001 = https://Archive.org/download/x/a b.iso\n"
                                          "00000002 = http://192.168.1.20:8080/b.iso\n"
                                          "00000003 = https://archive.org:443/download/y/c.iso\n"
                                          "00000005 = http://archive.org/download/z/d.iso\n"
                                          "@include http://nas.lan:8080/maps/remote.txt\n"
                                          "@include missing.txt\n"));
    xc_map_host h[8];
    TEST_ASSERT_EQUAL_size_t(5, xc_map_hosts(&map, h, 8));
    TEST_ASSERT_EQUAL_STRING("archive.org", h[0].host);
    TEST_ASSERT_EQUAL_INT(XC_URL_HTTPS, h[0].scheme);
    TEST_ASSERT_EQUAL_UINT16(443, h[0].port);
    TEST_ASSERT_EQUAL_size_t(2, h[0].entries); /* the port written or not */
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", h[1].host);
    TEST_ASSERT_TRUE(h[1].is_ipv4);
    TEST_ASSERT_EQUAL_UINT16(8080, h[1].port);
    TEST_ASSERT_EQUAL_STRING("archive.org", h[2].host); /* plain http is another server */
    TEST_ASSERT_EQUAL_INT(XC_URL_HTTP, h[2].scheme);
    TEST_ASSERT_EQUAL_STRING("h.lan", h[3].host); /* the included map's entry */
    TEST_ASSERT_EQUAL_size_t(1, h[3].entries);
    TEST_ASSERT_EQUAL_STRING("nas.lan", h[4].host); /* where the map came from */
    TEST_ASSERT_EQUAL_size_t(0, h[4].entries);
    TEST_ASSERT_EQUAL_size_t(1, h[4].maps);
    /* No room for more: the first ones, still counted right. */
    TEST_ASSERT_EQUAL_size_t(2, xc_map_hosts(&map, h, 2));
    TEST_ASSERT_EQUAL_size_t(2, h[0].entries);
    TEST_ASSERT_EQUAL_size_t(0, xc_map_hosts(&map, NULL, 0));
    TEST_ASSERT_EQUAL_size_t(0, xc_map_hosts(NULL, h, 8));
}

static void test_include_failures(void)
{
    static const ffile fs[] = {
        {"m/self.txt", HDR "@include self.txt\n"},
        {"m/a.txt", HDR "@include b.txt\n"},
        {"m/b.txt", HDR "@include a.txt\n00000001 = http://h.lan/1.iso\n"},
        {"m/noheader.txt", "00000002 = http://h.lan/2.iso\n"},
        {"m/newer.txt", "# xcat-map v9\n00000002 = http://h.lan/2.iso\n"},
        {"m/d1.txt", HDR "@include d2.txt\n"},
        {"m/d2.txt", HDR "@include d3.txt\n"},
        {"m/d3.txt", HDR "@include d4.txt\n"},
        {"m/d4.txt", HDR "@include d5.txt\n00000004 = http://h.lan/4.iso\n"},
        {"m/d5.txt", HDR "00000005 = http://h.lan/5.iso\n"},
    };
    set_files(fs, sizeof fs / sizeof fs[0]);
    TEST_ASSERT_EQUAL_INT(XC_OK, load_at(HDR "@include self.txt\n"     /* 2 */
                                             "@include a.txt\n"        /* 3 */
                                             "@include noheader.txt\n" /* 4 */
                                             "@include newer.txt\n"    /* 5 */
                                             "@include d1.txt\n",      /* 6 */
                                         "m/top.txt"));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/1.iso", url_of(1U, NULL)); /* b.txt's own line */
    TEST_ASSERT_EQUAL_STRING("-", url_of(2U, NULL));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/4.iso", url_of(4U, NULL)); /* depth 4 is allowed */
    TEST_ASSERT_EQUAL_STRING("-", url_of(5U, NULL));                  /* depth 5 isn't */
    expect_problem(XC_MAP_ERROR, 2, "a cycle"); /* reported in self.txt, line 2 */
    expect_problem(XC_MAP_ERROR, 4, "@include m/noheader.txt: not an xcat-map file");
    expect_problem(XC_MAP_ERROR, 5, "@include m/newer.txt: this map needs a newer");
    expect_problem(XC_MAP_ERROR, 2, "nests deeper than 4 levels");
    /* Without a fetch callback, includes fail but everything else loads. */
    xc_map_free(&map);
    const char *t = HDR "@include a.txt\n00000001 = http://h.lan/1.iso\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_load(t, strlen(t), "m/top.txt", NULL, &map));
    expect_problem(XC_MAP_ERROR, 2, "isn't available here");
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count);
}

static void test_file_limit(void)
{
    static ffile fs[40];
    static char names[40][16];
    for (int i = 0; i < 40; i++) {
        (void)snprintf(names[i], sizeof names[i], "f%d.txt", i);
        fs[i].location = names[i];
        fs[i].text = HDR;
    }
    set_files(fs, 40);
    char text[1024];
    size_t n = (size_t)snprintf(text, sizeof text, HDR);
    for (int i = 0; i < 40; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n, "@include f%d.txt\n", i);
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, load_at(text, "top.txt"));
    TEST_ASSERT_EQUAL_size_t(XC_MAP_MAX_FILES, map.source_count);
    TEST_ASSERT_EQUAL_INT(XC_MAP_MAX_FILES - 1, fetches);
    expect_problem(XC_MAP_ERROR, 33, "more than 32 map files");
    TEST_ASSERT_EQUAL_size_t(40 - (XC_MAP_MAX_FILES - 1), map.error_count);
}

static void test_find(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "0000000A@PAL = http://h.lan/pal.iso\n"));
    TEST_ASSERT_EQUAL_STRING("http://h.lan/pal.iso", url_of(0xAU, "pal"));
    TEST_ASSERT_EQUAL_STRING("-", url_of(0xAU, NULL)); /* no default entry to fall back to */
    TEST_ASSERT_EQUAL_STRING("-", url_of(0xAU, "ABCDEFGHIJKLMNOPQ"));
    TEST_ASSERT_NULL(xc_map_find(NULL, 0xAU, NULL));
}

/* --- §5.2: @archiveorg ---------------------------------------------------------------------- */

#define MD "https://archive.org/metadata/"

static void test_archiveorg(void)
{
    static const ffile fs[] = {
        {MD "games-a",
         "{\"files\":[{\"name\":\"Game A "
         "(USA).zip\",\"source\":\"original\",\"size\":\"6266337551\","
         "\"md5\":\"38b167e84cd7f5342419b83e7896c992\",\"crc32\":\"57654766\","
         "\"sha1\":\"0bf206772bb82ead1eb27c24531c2042b719011b\",\"private\":\"true\"},"
         "{\"name\":\"Game B (Europe).xiso.iso\",\"size\":\"3728867328\",\"md5\":\"x\"},"
         "{\"name\":\"bad.iso\",\"size\":\"0\"},"
         "{\"name\":\"in.zip/Game C.iso\",\"size\":\"9\"},"
         "{\"name\":\"cover.jpg\",\"size\":\"9\"}]}"},
        {MD "games-b", "{\"files\":[{\"name\":\"GAME A (USA).iso\",\"size\":\"7\"}]}"},
    };
    set_files(fs, 2);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "@archiveorg games-a\n"           /* 2 */
                                          "0000000A = http://h.lan/a.iso\n" /* 3 */
                                          "@archiveorg games-b\n"));        /* 4 */
    TEST_ASSERT_EQUAL_size_t(0, map.error_count);
    TEST_ASSERT_EQUAL_size_t(3, map.entry_count);
    TEST_ASSERT_EQUAL_INT(2, fetches);
    TEST_ASSERT_EQUAL_STRING(MD "games-a", fetched[0]);

    /* games-b's "GAME A (USA)" replaces games-a's "Game A (USA)" (§4.3, case-insensitive). */
    const xc_map_entry *a = xc_map_find_name(&map, "Game A (USA)");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("GAME A (USA)", a->name);
    TEST_ASSERT_EQUAL_STRING("https://archive.org/download/games-b/GAME%20A%20%28USA%29.iso",
                             a->url);
    TEST_ASSERT_EQUAL_size_t(4, a->line);
    TEST_ASSERT_TRUE(a->size == 7);
    TEST_ASSERT_FALSE(a->has_md5 || a->has_sha1 || a->has_crc32);
    expect_problem(XC_MAP_WARNING, 4, "\"GAME A (USA)\" also defined at E:\\xcat\\map.txt:2");

    const xc_map_entry *b = xc_map_find_name(&map, "game b (europe)");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING(
        "https://archive.org/download/games-a/Game%20B%20%28Europe%29.xiso.iso", b->url);
    TEST_ASSERT_TRUE(b->size == UINT64_C(3728867328));
    TEST_ASSERT_EQUAL_UINT32(0, b->title_id);
    TEST_ASSERT_EQUAL_STRING("", b->variant);
    TEST_ASSERT_EQUAL_size_t(2, b->line);
    expect_problem(XC_MAP_WARNING, 2,
                   "skipped 1 file(s) with a bad name or size, first \"bad.iso\"");
    expect_problem(XC_MAP_WARNING, 2, "skipped 1 file(s) whose URL isn't accepted");
    expect_problem(XC_MAP_WARNING, 2, "ignored 1 malformed hash(es)");

    /* Title-ID lookups never return a generated entry, and the reverse. */
    TEST_ASSERT_EQUAL_STRING("http://h.lan/a.iso", url_of(0xAU, NULL));
    TEST_ASSERT_EQUAL_STRING("-", url_of(0, NULL));
    TEST_ASSERT_NULL(xc_map_find_name(&map, "Game C"));
    TEST_ASSERT_NULL(xc_map_find_name(&map, "cover"));
    TEST_ASSERT_NULL(xc_map_find_name(NULL, "x"));
}

static void test_archiveorg_listing_details(void)
{
    static const ffile fs[] = {
        {MD "full", "{\"files\":[{\"name\":\"Game (USA).zip\",\"size\":\"6266337551\","
                    "\"md5\":\"38B167E84CD7F5342419B83E7896C992\",\"crc32\":\"57654766\","
                    "\"sha1\":\"0bf206772bb82ead1eb27c24531c2042b719011b\"}]}"},
    };
    set_files(fs, 1);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "@archiveorg full\n"));
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count);
    const xc_map_entry *e = &map.entries[0];
    TEST_ASSERT_TRUE(e->has_md5 && e->has_sha1 && e->has_crc32);
    TEST_ASSERT_EQUAL_HEX8(0x38, e->md5[0]);
    TEST_ASSERT_EQUAL_HEX8(0x92, e->md5[15]);
    TEST_ASSERT_EQUAL_HEX8(0x0B, e->sha1[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1B, e->sha1[19]);
    TEST_ASSERT_EQUAL_HEX32(0x57654766U, e->crc32);
    TEST_ASSERT_TRUE(e->size == UINT64_C(6266337551));

    /* A hand-written entry keeps no listing fields. */
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "0000000A = http://h.lan/a.iso\n"));
    TEST_ASSERT_NULL(map.entries[0].name);
    TEST_ASSERT_TRUE(map.entries[0].size == 0);
    TEST_ASSERT_FALSE(map.entries[0].has_md5);
}

static void test_archiveorg_failures(void)
{
    static const ffile fs[] = {
        {MD "gone", "{}"},
        {MD "dark", "{\"is_dark\":true,\"files\":[{\"name\":\"a.iso\",\"size\":\"1\"}]}"},
        {MD "html", "<html>busy</html>"},
        {MD "empty", "{\"files\":[{\"name\":\"cover.jpg\",\"size\":\"1\"}]}"},
        {MD "ok", "{\"files\":[{\"name\":\"a.iso\",\"size\":\"1\"}]}"},
    };
    set_files(fs, 5);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "@archiveorg gone\n"    /* 2 */
                                          "@archiveorg dark\n"    /* 3 */
                                          "@archiveorg html\n"    /* 4 */
                                          "@archiveorg empty\n"   /* 5 */
                                          "@archiveorg unknown\n" /* 6 */
                                          "@archiveorg\n"         /* 7 */
                                          "@archiveorg a b\n"     /* 8 */
                                          "@archiveorg ok\n"));   /* 9 */
    expect_problem(XC_MAP_ERROR, 2, "archive.org item gone doesn't exist or was removed");
    expect_problem(XC_MAP_ERROR, 3, "archive.org item dark was taken down");
    expect_problem(XC_MAP_ERROR, 4, "archive.org didn't send the metadata of item html");
    expect_problem(XC_MAP_WARNING, 5, "archive.org item empty has no .iso, .xiso or .zip files");
    expect_problem(XC_MAP_ERROR, 6, "@archiveorg unknown: couldn't get the item's listing");
    expect_problem(XC_MAP_ERROR, 7, "@archiveorg needs an argument");
    expect_problem(XC_MAP_ERROR, 8, "invalid archive.org identifier");
    TEST_ASSERT_EQUAL_size_t(6, map.error_count);
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count); /* every other line still loads */
    TEST_ASSERT_NOT_NULL(xc_map_find_name(&map, "a"));
    TEST_ASSERT_EQUAL_size_t(1, map.source_count); /* listings aren't map files */

    /* Without a fetch callback the directive fails, and nothing else does. */
    xc_map_free(&map);
    const char *text = HDR "@archiveorg ok\n0000000A = http://h.lan/a.iso\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_load(text, strlen(text), "m.txt", NULL, &map));
    expect_problem(XC_MAP_ERROR, 2, "@archiveorg isn't available here");
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count);
}

static void test_archiveorg_limit(void)
{
    static const ffile fs[] = {{MD "ok", "{\"files\":[{\"name\":\"a.iso\",\"size\":\"1\"}]}"}};
    set_files(fs, 1);
    char text[2048];
    size_t n = (size_t)snprintf(text, sizeof text, HDR);
    for (int i = 0; i < XC_MAP_MAX_LISTINGS + 2; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n, "@archiveorg ok\n");
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, load(text));
    TEST_ASSERT_EQUAL_INT(XC_MAP_MAX_LISTINGS, fetches);
    expect_problem(XC_MAP_ERROR, XC_MAP_MAX_LISTINGS + 2, "more than 32 items");
    TEST_ASSERT_EQUAL_size_t(2, map.error_count);
    TEST_ASSERT_EQUAL_size_t(1, map.entry_count);
}

/* §5.4: @smb lists a NAS folder's images (smb.md §3). */
static void test_smb_folders(void)
{
    static const char *const games[] = {"Halo (USA).xiso.iso",   "readme.txt",     "Xbox/Fable.zip",
                                        "Xbox/100% Done #1.iso", "Xbox/cover.jpg", NULL};
    static const char *const empty[] = {"notes.txt", NULL};
    static const fnas fs[] = {
        {"smb://192.168.1.50/Games", games, XC_OK},
        {"smb://192.168.1.50/Empty", empty, XC_OK},
        {"smb://192.168.1.50/Locked", NULL, XC_ERR_SMB_LOGIN},
    };
    set_nases(fs, sizeof fs / sizeof fs[0]);
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          load(HDR "@smb SMB://192.168.1.50:445/Games/\n"
                                   "@smb smb://192.168.1.50/Empty\n"
                                   "@smb smb://192.168.1.50/Locked\n"
                                   "@smb smb://shaun@192.168.1.50/Games\n"
                                   "@smb http://192.168.1.50/Games\n"
                                   "4D530004 = smb://192.168.1.50/Games/Halo (USA).xiso.iso\n"
                                   "4D530005 = smb://192.168.1.50/Games\n"
                                   "4D530006 = smb://u@nas/Games/a.iso\n"
                                   "4D530007 = smb://nas/Games/a.zip/b.iso\n"));
    TEST_ASSERT_EQUAL_INT(3, listings);
    TEST_ASSERT_EQUAL_size_t(4, map.entry_count);
    const xc_map_entry *e = xc_map_find_name(&map, "halo (usa)");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://192.168.1.50/Games/Halo (USA).xiso.iso", e->url);
    TEST_ASSERT_EQUAL_UINT64(1000, e->size);
    TEST_ASSERT_EQUAL_size_t(2, e->line); /* line 1 is the header */
    e = xc_map_find_name(&map, "Fable");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://192.168.1.50/Games/Xbox/Fable.zip", e->url);
    e = xc_map_find_name(&map, "100% Done #1");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://192.168.1.50/Games/Xbox/100%25 Done %231.iso", e->url);
    e = xc_map_find(&map, 0x4D530004, NULL);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://192.168.1.50/Games/Halo (USA).xiso.iso", e->url);
    expect_problem(XC_MAP_WARNING, 3, "has no .iso, .xiso or .zip files");
    expect_problem(XC_MAP_ERROR, 4, "The NAS refused the user name or password");
    expect_problem(XC_MAP_ERROR, 5, "invalid @smb folder");
    expect_problem(XC_MAP_ERROR, 6, "invalid @smb folder");
    expect_problem(XC_MAP_ERROR, 8, "names a share, not an image");
    expect_problem(XC_MAP_ERROR, 9, "must not contain user credentials");
    expect_problem(XC_MAP_ERROR, 10, "inside a zip");
    /* The NAS is one server: its folders and entries. */
    xc_map_host h[4];
    TEST_ASSERT_EQUAL_size_t(1, xc_map_hosts(&map, h, 4));
    TEST_ASSERT_EQUAL_INT(XC_URL_SMB, h[0].scheme);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", h[0].host);
    TEST_ASSERT_EQUAL_UINT16(445, h[0].port);
    TEST_ASSERT_TRUE(h[0].is_ipv4);
    TEST_ASSERT_EQUAL_size_t(4, h[0].entries);
    TEST_ASSERT_EQUAL_size_t(3, h[0].folders);
    TEST_ASSERT_EQUAL_size_t(3, map.smb_folder_count);
    TEST_ASSERT_EQUAL_STRING("smb://192.168.1.50/Games", map.smb_folders[0]);
}

/* smb.md §3.1: game folders are entries named after the folder, whose address ends in '/'. */
static void test_smb_game_folders(void)
{
    static const char *const mixed[] = {"Halo (USA)/", "Xbox/Fable #2/", "Xbox/Jet Set.iso",
                                        "Bad:Name/", NULL};
    static const char *const one[] = {"/", NULL};
    static const char *const top[] = {"/", NULL};
    static const fnas fs[] = {
        {"smb://nas/Games", mixed, XC_OK},
        {"smb://nas/Games/Xbox/Outrun 2006", one, XC_OK},
        {"smb://nas/Halo2", top, XC_OK},
    };
    set_nases(fs, sizeof fs / sizeof fs[0]);
    TEST_ASSERT_EQUAL_INT(XC_OK, load(HDR "@smb smb://nas/Games\n"
                                          "@smb smb://nas/Games/Xbox/Outrun 2006\n"
                                          "@smb smb://nas/Halo2\n"));
    TEST_ASSERT_EQUAL_size_t(5, map.entry_count);
    const xc_map_entry *e = xc_map_find_name(&map, "Halo (USA)");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://nas/Games/Halo (USA)/", e->url);
    TEST_ASSERT_EQUAL_UINT64(0, e->size);
    e = xc_map_find_name(&map, "Fable #2");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://nas/Games/Xbox/Fable %232/", e->url);
    e = xc_map_find_name(&map, "Jet Set");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://nas/Games/Xbox/Jet Set.iso", e->url);
    /* The @smb folder is itself a game: named after its last folder, or its share. */
    e = xc_map_find_name(&map, "Outrun 2006");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://nas/Games/Xbox/Outrun 2006/", e->url);
    e = xc_map_find_name(&map, "Halo2");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("smb://nas/Halo2/", e->url);
    expect_problem(XC_MAP_WARNING, 2, "skipped 1 game(s)"); /* Bad:Name */
}

static void test_smb_unavailable(void)
{
    static const xc_map_loader no_smb = {fake_fetch, NULL, NULL};
    xc_map_free(&map);
    static const char text[] = HDR "@smb smb://nas/Games\n";
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_load(text, strlen(text), "map.txt", &no_smb, &map));
    expect_problem(XC_MAP_ERROR, 2, "@smb isn't available here");
    /* The folder still counts as a server the map uses (for SETTINGS' logins). */
    xc_map_host h[2];
    TEST_ASSERT_EQUAL_size_t(1, xc_map_hosts(&map, h, 2));
    TEST_ASSERT_EQUAL_STRING("nas", h[0].host);
    TEST_ASSERT_EQUAL_size_t(1, h[0].folders);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_archiveorg);
    RUN_TEST(test_archiveorg_listing_details);
    RUN_TEST(test_archiveorg_failures);
    RUN_TEST(test_archiveorg_limit);
    RUN_TEST(test_spec_example);
    RUN_TEST(test_lines_and_header);
    RUN_TEST(test_line_errors_keep_loading);
    RUN_TEST(test_zip_urls);
    RUN_TEST(test_duplicates);
    RUN_TEST(test_lines_too_long_and_files_too_big);
    RUN_TEST(test_entry_limit);
    RUN_TEST(test_problem_cap);
    RUN_TEST(test_include_paths);
    RUN_TEST(test_hosts);
    RUN_TEST(test_include_failures);
    RUN_TEST(test_file_limit);
    RUN_TEST(test_find);
    RUN_TEST(test_smb_folders);
    RUN_TEST(test_smb_game_folders);
    RUN_TEST(test_smb_unavailable);
    return UNITY_END();
}
