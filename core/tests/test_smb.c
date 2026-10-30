/* Tests for smb:// URLs, image names, NT hashes and listings (xcat/smb.h, docs/formats/smb.md). */
#include "unity.h"
#include "xcat/smb.h"

#include <stdio.h>
#include <string.h>

#include "md4.h"

void setUp(void) {}
void tearDown(void) {}

static xc_smb_url u;

static xc_err parse(const char *s)
{
    memset(&u, 0xAB, sizeof u);
    return xc_smb_url_parse(s, strlen(s), &u);
}

static void test_is_url(void)
{
    TEST_ASSERT_TRUE(xc_smb_is_url("smb://nas/x"));
    TEST_ASSERT_TRUE(xc_smb_is_url("SMB://nas/x"));
    TEST_ASSERT_FALSE(xc_smb_is_url("smb:/nas/x"));
    TEST_ASSERT_FALSE(xc_smb_is_url("http://nas/x"));
    TEST_ASSERT_FALSE(xc_smb_is_url("smb"));
    TEST_ASSERT_FALSE(xc_smb_is_url(""));
}

static void test_parse(void)
{
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("smb://192.168.1.50/Games/Xbox/Halo (USA).iso"));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", u.host);
    TEST_ASSERT_TRUE(u.is_ipv4);
    TEST_ASSERT_EQUAL_UINT16(445, u.port);
    TEST_ASSERT_EQUAL_STRING("Games", u.share);
    TEST_ASSERT_EQUAL_STRING("Xbox/Halo (USA).iso", u.path);

    TEST_ASSERT_EQUAL_INT(XC_OK, parse("SMB://TNAS-F4.local:4450/Public"));
    TEST_ASSERT_EQUAL_STRING("tnas-f4.local", u.host);
    TEST_ASSERT_FALSE(u.is_ipv4);
    TEST_ASSERT_EQUAL_UINT16(4450, u.port);
    TEST_ASSERT_EQUAL_STRING("Public", u.share);
    TEST_ASSERT_EQUAL_STRING("", u.path);

    /* A trailing '/' is allowed; escapes decode; UTF-8 names are kept. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("smb://nas/Games%20Share/Jeux%23/\xC3\xA9t\xC3\xA9/"));
    TEST_ASSERT_EQUAL_STRING("Games Share", u.share);
    TEST_ASSERT_EQUAL_STRING("Jeux#/\xC3\xA9t\xC3\xA9", u.path);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("smb://nas/share/"));
    TEST_ASSERT_EQUAL_STRING("", u.path);
}

static void test_rejected(void)
{
    static const char *const bad[] = {
        "smb://",
        "smb://nas",            /* no share */
        "smb://nas/",           /* empty share */
        "smb://user@nas/share", /* credentials never go in URLs (§1.1) */
        "smb://user:pw@nas/share",
        "smb://nas:0/share", /* port 0 */
        "smb://nas:65536/share",
        "smb://nas:44a/share",
        "smb://nas:/share",
        "smb://nas:0445/share",  /* leading zero */
        "smb://[::1]/share",     /* no IPv6 in v1 */
        "smb://256.1.1.1/share", /* all digits: must be IPv4 */
        "smb://1.2.3/share",
        "smb://01.2.3.4/share",
        "smb://na s/share",
        "smb://.nas/share",
        "smb://nas./share",
        "smb://nas/share//x", /* empty name */
        "smb://nas/share/./x",
        "smb://nas/share/../x",
        "smb://nas/share/a%2Fb", /* an escaped '/' */
        "smb://nas/share/a%5Cb", /* '\' */
        "smb://nas/share/a:b",
        "smb://nas/share/a*b",
        "smb://nas/share/a?b",   /* no query */
        "smb://nas/share/a#b",   /* no fragment */
        "smb://nas/share/a%0Ab", /* control character */
        "smb://nas/share/a%zz",  /* bad escape */
        "smb://nas/share/a%4",
        "smb://nas/share/\xC3", /* bad UTF-8 */
        "smb://nas/sh|are",
        "http://nas/share",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_SMB_URL, parse(bad[i]), bad[i]);
    }
    /* Limits: 255 bytes per name, 80 per share, 768 for the path. */
    char s[2048];
    char name[300];
    memset(name, 'a', 255);
    name[255] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/share/%s", name);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(s));
    name[255] = 'a';
    name[256] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/share/%s", name);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, parse(s));
    memset(name, 'b', 80);
    name[80] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/%s", name);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(s));
    name[80] = 'b';
    name[81] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/%s", name);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, parse(s));
    /* 3 names of 255 bytes and 2 slashes is 767; one more byte is 768; then too long. */
    char seg[256];
    memset(seg, 'c', 255);
    seg[255] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/share/%s/%s/%s/d", seg, seg, seg);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, parse(s)); /* 3 * 255 + 2 + 2 = 769 */
    seg[254] = '\0';
    (void)snprintf(s, sizeof s, "smb://nas/share/%s/%s/%sc/dd", seg, seg, seg);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(s)); /* 254 + 1 + 254 + 1 + 255 + 1 + 2 = 768 */
    TEST_ASSERT_EQUAL_size_t(768, strlen(u.path));
    /* A NUL inside the length is refused. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, xc_smb_url_parse("smb://nas/a\0b", 13, &u));
}

static void test_format_round_trip(void)
{
    static const char *const cases[][2] = {
        {"smb://192.168.1.50/Games/Xbox/Halo (USA).iso",
         "smb://192.168.1.50/Games/Xbox/Halo (USA).iso"},
        {"SMB://NAS:445/Public/", "smb://nas/Public"},
        {"smb://nas:4450/a%20b/100%25 done/x", "smb://nas:4450/a b/100%25 done/x"},
    };
    char out[XC_SMB_MAX_URL];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, parse(cases[i][0]), cases[i][0]);
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_format(&u, out, sizeof out));
        TEST_ASSERT_EQUAL_STRING(cases[i][1], out);
    }
}

static void test_format_escapes_what_cannot_be_written(void)
{
    char out[XC_SMB_MAX_URL];
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("smb://nas/s/a%23b%25c"));
    TEST_ASSERT_EQUAL_STRING("a#b%c", u.path);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_format(&u, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("smb://nas/s/a%23b%25c", out);
    xc_smb_url back;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_parse(out, strlen(out), &back));
    TEST_ASSERT_EQUAL_STRING(u.path, back.path);
    /* Too small a buffer. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_smb_url_format(&u, out, 12));
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_smb_url_format(&u, out, 20));
}

static void test_child(void)
{
    xc_smb_url c;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("smb://nas/Games"));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_child(&u, "Xbox/Halo.iso", &c));
    TEST_ASSERT_EQUAL_STRING("Xbox/Halo.iso", c.path);
    TEST_ASSERT_EQUAL_STRING("Games", c.share);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_url_child(&c, "more", &c));
    TEST_ASSERT_EQUAL_STRING("Xbox/Halo.iso/more", c.path);
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, xc_smb_url_child(&u, "a//b", &c));
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, xc_smb_url_child(&u, "..", &c));
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, xc_smb_url_child(&u, "a/", &c));
    TEST_ASSERT_EQUAL_INT(XC_ERR_SMB_URL, xc_smb_url_child(&u, "a:b", &c));
}

static void test_image_names(void)
{
    char n[64];
    bool zip = true;
    TEST_ASSERT_TRUE(xc_smb_image_name("Halo (USA).xiso.iso", n, sizeof n, &zip));
    TEST_ASSERT_EQUAL_STRING("Halo (USA)", n);
    TEST_ASSERT_FALSE(zip);
    TEST_ASSERT_TRUE(xc_smb_image_name("Xbox/Fable (Europe).ISO", n, sizeof n, &zip));
    TEST_ASSERT_EQUAL_STRING("Fable (Europe)", n);
    TEST_ASSERT_TRUE(xc_smb_image_name("a/b/Jet Set Radio Future.xiso", n, sizeof n, &zip));
    TEST_ASSERT_EQUAL_STRING("Jet Set Radio Future", n);
    TEST_ASSERT_TRUE(xc_smb_image_name("Outrun 2 (USA).Zip", n, sizeof n, &zip));
    TEST_ASSERT_EQUAL_STRING("Outrun 2 (USA)", n);
    TEST_ASSERT_TRUE(zip);
    TEST_ASSERT_FALSE(xc_smb_image_name("readme.txt", n, sizeof n, &zip));
    TEST_ASSERT_FALSE(xc_smb_image_name("Game.cci", n, sizeof n, &zip));
    TEST_ASSERT_FALSE(xc_smb_image_name(".iso", n, sizeof n, &zip));
    TEST_ASSERT_FALSE(xc_smb_image_name("x/.zip", n, sizeof n, &zip));
    TEST_ASSERT_FALSE(xc_smb_image_name("Long name.iso", n, 9, &zip)); /* doesn't fit */
    TEST_ASSERT_TRUE(xc_smb_image_name("Long name.iso", n, 10, &zip));
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        (void)sprintf(out + 2 * i, "%02x", d[i]);
    }
}

static void test_md4_vectors(void)
{
    /* RFC 1320 §A.5 (also checked against pycryptodome). */
    static const char *const v[][2] = {
        {"", "31d6cfe0d16ae931b73c59d7e0c089c0"},
        {"a", "bde52cb31de33e46245e05fbdbd6fb24"},
        {"abc", "a448017aaf21d8525fc10ae87aa6729d"},
        {"message digest", "d9130a8164549fe818874806e1c7014b"},
        {"abcdefghijklmnopqrstuvwxyz", "d79e1c308aa5bbcdeea8ed63df412da9"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "043f8582f241db351ce627e153e7f0e4"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "e33b4ddc9c38f2199c3e7b164fcc0536"},
    };
    uint8_t d[XC_MD4_BYTES];
    char h[33];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        xc_md4(v[i][0], strlen(v[i][0]), d);
        hex(d, sizeof d, h);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v[i][1], h, v[i][0]);
    }
}

static void test_md4_lengths_against_reference(void)
{
    /* Reference digests from Python's hashlib (OpenSSL's MD4), for n bytes of 'a'. */
    static const struct {
        size_t n;
        const char *hex;
    } v[] = {
#include "md4_vectors.inc"
    };
    char buf[300];
    memset(buf, 'a', sizeof buf);
    uint8_t d[XC_MD4_BYTES];
    char h[33];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        xc_md4(buf, v[i].n, d);
        hex(d, sizeof d, h);
        char msg[32];
        (void)snprintf(msg, sizeof msg, "n=%zu", v[i].n);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v[i].hex, h, msg);
    }
}

static void test_nthash(void)
{
    char h[XC_SMB_NTHASH_HEX + 1];
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_nthash("password", h));
    TEST_ASSERT_EQUAL_STRING("8846F7EAEE8FB117AD06BDD830B7586C", h);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_nthash("", h));
    TEST_ASSERT_EQUAL_STRING("31D6CFE0D16AE931B73C59D7E0C089C0", h);
    /* Non-ASCII: U+00E9, and U+1F600 as a surrogate pair (values from Impacket's
       compute_nthash, i.e. MD4 of the UTF-16LE form). */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_nthash("caf\xC3\xA9", h));
    TEST_ASSERT_EQUAL_STRING("B1DB12409C00D1FC586FC48ECADC36A1", h);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_nthash("\xF0\x9F\x98\x80", h));
    TEST_ASSERT_EQUAL_STRING("4B58A10CC20A4E7D808D218E1F80AABC", h);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_smb_nthash("\xC3", h));
    char longpw[XC_SMB_MAX_PASSWORD + 2];
    memset(longpw, 'x', sizeof longpw - 1);
    longpw[sizeof longpw - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_smb_nthash(longpw, h));
    longpw[XC_SMB_MAX_PASSWORD] = '\0';
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_nthash(longpw, h));

    TEST_ASSERT_TRUE(xc_smb_valid_nthash("8846F7EAEE8FB117AD06BDD830B7586C"));
    TEST_ASSERT_TRUE(xc_smb_valid_nthash("8846f7eaee8fb117ad06bdd830b7586c"));
    TEST_ASSERT_FALSE(xc_smb_valid_nthash("8846F7EAEE8FB117AD06BDD830B7586"));
    TEST_ASSERT_FALSE(xc_smb_valid_nthash("8846F7EAEE8FB117AD06BDD830B7586CC"));
    TEST_ASSERT_FALSE(xc_smb_valid_nthash("8846F7EAEE8FB117AD06BDD830B7586G"));
    TEST_ASSERT_FALSE(xc_smb_valid_nthash(NULL));
}

static void test_listing(void)
{
    xc_smb_listing l;
    memset(&l, 0, sizeof l);
    char p[16];
    for (int i = 0; i < XC_SMB_MAX_LISTED + 5; i++) {
        (void)snprintf(p, sizeof p, "f%d.iso", i);
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_smb_listing_add(&l, p, (uint64_t)i));
    }
    TEST_ASSERT_EQUAL_size_t(XC_SMB_MAX_LISTED, l.count);
    TEST_ASSERT_TRUE(l.truncated);
    TEST_ASSERT_EQUAL_STRING("f7.iso", l.files[7].path);
    TEST_ASSERT_EQUAL_UINT64(7, l.files[7].size);
    xc_smb_listing_free(&l);
    TEST_ASSERT_EQUAL_size_t(0, l.count);
    TEST_ASSERT_NULL(l.files);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_url);
    RUN_TEST(test_parse);
    RUN_TEST(test_rejected);
    RUN_TEST(test_format_round_trip);
    RUN_TEST(test_format_escapes_what_cannot_be_written);
    RUN_TEST(test_child);
    RUN_TEST(test_image_names);
    RUN_TEST(test_md4_vectors);
    RUN_TEST(test_md4_lengths_against_reference);
    RUN_TEST(test_nthash);
    RUN_TEST(test_listing);
    return UNITY_END();
}
