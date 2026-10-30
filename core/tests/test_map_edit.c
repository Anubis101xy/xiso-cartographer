/* Tests for editing a map's sources (xcat/map_edit.h, docs/formats/map.md §9). */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "xcat/map.h"
#include "xcat/map_edit.h"

void setUp(void) {}
void tearDown(void) {}

static const char sample[] = "\xEF\xBB\xBF\r\n"
                             "# xcat-map v1\r\n"
                             "# my games\r\n"
                             "4D530004 = https://archive.org/download/x/a.iso\r\n"
                             "\r\n"
                             "  @include   http://192.168.1.20:8080/map.txt  \r\n"
                             "@archiveorg my-item\r\n"
                             "@newthing whatever\r\n"
                             "@includex nope\r\n"
                             "no equals here";

static void expect_text(const char *want, const char *p, size_t n)
{
    TEST_ASSERT_EQUAL_size_t(strlen(want), n);
    TEST_ASSERT_EQUAL_MEMORY(want, p, n);
}

static void test_lines(void)
{
    xc_map_line l[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_lines(sample, sizeof sample - 1, l, 8, &count));
    TEST_ASSERT_EQUAL_size_t(6, count);
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_ENTRY, l[0].kind);
    TEST_ASSERT_EQUAL_size_t(4, l[0].line);
    expect_text("4D530004", l[0].text, l[0].key_len);
    expect_text("https://archive.org/download/x/a.iso", l[0].arg, l[0].arg_len);
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_INCLUDE, l[1].kind);
    TEST_ASSERT_EQUAL_size_t(6, l[1].line);
    expect_text("@include   http://192.168.1.20:8080/map.txt", l[1].text, l[1].text_len);
    expect_text("http://192.168.1.20:8080/map.txt", l[1].arg, l[1].arg_len);
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_ARCHIVEORG, l[2].kind);
    expect_text("my-item", l[2].arg, l[2].arg_len);
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_DIRECTIVE, l[3].kind);
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_DIRECTIVE, l[4].kind); /* not @include */
    TEST_ASSERT_EQUAL_INT(XC_MAP_LINE_ENTRY, l[5].kind);     /* invalid, but listed */
    TEST_ASSERT_EQUAL_size_t(10, l[5].line);
    TEST_ASSERT_EQUAL_size_t(0, l[5].arg_len);

    /* More than there's room for: all counted. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_lines(sample, sizeof sample - 1, l, 2, &count));
    TEST_ASSERT_EQUAL_size_t(6, count);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_lines("# xcat-map v1", 13, NULL, 0, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);

    /* No header, or a newer one. */
    static const char *const bad[] = {"", "\n\n", "00000001 = http://a/b.iso\n",
                                      "# xcat-map v2\n00000001 = http://a/b.iso\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, xc_map_lines(bad[i], strlen(bad[i]), l, 8, &count));
    }
}

static void new_ok(xc_map_line_kind kind, const char *key, const char *url, const char *want)
{
    char line[XC_MAP_MAX_LINE + 1];
    char why[200];
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        XC_OK, xc_map_new_line(kind, key, url, line, sizeof line, why, sizeof why), why);
    TEST_ASSERT_EQUAL_STRING(want, line);
}

static void new_bad(xc_map_line_kind kind, const char *key, const char *url, const char *why_has)
{
    char line[XC_MAP_MAX_LINE + 1];
    char why[200];
    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP,
                          xc_map_new_line(kind, key, url, line, sizeof line, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, why_has), why);
}

static void test_new_line(void)
{
    new_ok(XC_MAP_LINE_ARCHIVEORG, NULL, "my_item-2.0", "@archiveorg my_item-2.0");
    new_ok(XC_MAP_LINE_SMB, NULL, "SMB://192.168.1.50:445/Games/Xbox%20OG/",
           "@smb smb://192.168.1.50/Games/Xbox OG");
    new_bad(XC_MAP_LINE_SMB, NULL, "smb://shaun@nas/Games", "smb://<NAS>/<share>");
    new_bad(XC_MAP_LINE_SMB, NULL, "http://nas/Games", "smb://<NAS>/<share>");
    new_ok(XC_MAP_LINE_INCLUDE, NULL, "HTTP://192.168.1.20:8080/map.txt",
           "@include HTTP://192.168.1.20:8080/map.txt");
    new_ok(XC_MAP_LINE_ENTRY, "4d530004", "http://192.168.1.20/Halo (Europe).xiso.iso",
           "4D530004 = http://192.168.1.20/Halo (Europe).xiso.iso");
    new_ok(XC_MAP_LINE_ENTRY, "4d530004@pal", "https://h.lan/a.zip",
           "4D530004@pal = https://h.lan/a.zip");

    new_bad(XC_MAP_LINE_ARCHIVEORG, NULL, "", "identifier");
    new_bad(XC_MAP_LINE_ARCHIVEORG, NULL, "has space", "identifier");
    new_bad(XC_MAP_LINE_INCLUDE, NULL, "map.txt", "http:// or https://");
    new_bad(XC_MAP_LINE_INCLUDE, NULL, "ftp://h/map.txt", "http:// or https://");
    new_bad(XC_MAP_LINE_ENTRY, "4D53", "http://h.lan/a.iso", "");
    new_bad(XC_MAP_LINE_ENTRY, "", "http://h.lan/a.iso", "8 hex digits");
    new_bad(XC_MAP_LINE_ENTRY, "4D530004", "h.lan/a.iso", "");
    new_bad(XC_MAP_LINE_ENTRY, "4D530004", "http://user:pw@h.lan/a.iso", "");
    new_bad(XC_MAP_LINE_ENTRY, "4D530004", "http://h.lan/a.zip/a.iso", "zip");
    new_bad(XC_MAP_LINE_ENTRY, "4D530004", "http://h.lan/a.iso\n00000001 = http://x/y.iso",
            "control character");

    char line[16];
    char why[64];
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_map_new_line(XC_MAP_LINE_ARCHIVEORG, NULL, "abcdefgh",
                                                        line, sizeof line, why, sizeof why));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                          xc_map_new_line(XC_MAP_LINE_ENTRY, NULL, "http://h/a.iso", line,
                                          sizeof line, why, sizeof why));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_map_new_line(XC_MAP_LINE_DIRECTIVE, NULL, "x",
                                                              line, sizeof line, why, sizeof why));
}

static void test_add(void)
{
    char *out = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_add_line(NULL, 0, "@archiveorg a", &out, &len));
    TEST_ASSERT_EQUAL_STRING("# xcat-map v1\n\n@archiveorg a\n", out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), len);
    free(out);

    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_map_add_line(sample, sizeof sample - 1, "@archiveorg b", &out, &len));
    TEST_ASSERT_EQUAL_MEMORY(sample, out, sizeof sample - 1); /* every byte kept */
    TEST_ASSERT_EQUAL_STRING("\n@archiveorg b\n", out + sizeof sample - 1);
    xc_map_line l[8];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_lines(out, len, l, 8, &count));
    TEST_ASSERT_EQUAL_size_t(7, count);
    free(out);

    static const char ends[] = "# xcat-map v1\n";
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_map_add_line(ends, sizeof ends - 1, "@archiveorg c", &out, &len));
    TEST_ASSERT_EQUAL_STRING("# xcat-map v1\n@archiveorg c\n", out);
    free(out);

    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, xc_map_add_line("hello\n", 6, "@archiveorg c", &out, &len));
    TEST_ASSERT_NULL(out);
    /* Not past the file limit. */
    char *big = malloc(XC_MAP_MAX_FILE);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, '#', XC_MAP_MAX_FILE);
    static const char header[] = "# xcat-map v1\n";
    memcpy(big, header, sizeof header);
    big[sizeof header - 1] = '#';
    TEST_ASSERT_EQUAL_INT(
        XC_ERR_LIMIT, xc_map_add_line(big, XC_MAP_MAX_FILE - 10, "@archiveorg abcdef", &out, &len));
    free(big);
}

static void test_remove(void)
{
    char *out = NULL;
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_remove_line(sample, sizeof sample - 1, 6, &out, &len));
    static const char want[] = "\xEF\xBB\xBF\r\n"
                               "# xcat-map v1\r\n"
                               "# my games\r\n"
                               "4D530004 = https://archive.org/download/x/a.iso\r\n"
                               "\r\n"
                               "@archiveorg my-item\r\n"
                               "@newthing whatever\r\n"
                               "@includex nope\r\n"
                               "no equals here";
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_size_t(sizeof want - 1, len);
    free(out);
    /* The last line, without a line ending. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_map_remove_line(sample, sizeof sample - 1, 10, &out, &len));
    TEST_ASSERT_EQUAL_size_t(sizeof sample - 1 - strlen("no equals here"), len);
    free(out);
    /* Not the header, a comment, a blank line, or past the end. */
    static const size_t no[] = {0, 1, 2, 3, 5, 11, 99};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG,
                              xc_map_remove_line(sample, sizeof sample - 1, no[i], &out, &len));
        TEST_ASSERT_NULL(out);
    }
    TEST_ASSERT_EQUAL_INT(XC_ERR_MAP, xc_map_remove_line("x\n", 2, 1, &out, &len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lines);
    RUN_TEST(test_new_line);
    RUN_TEST(test_add);
    RUN_TEST(test_remove);
    return UNITY_END();
}
