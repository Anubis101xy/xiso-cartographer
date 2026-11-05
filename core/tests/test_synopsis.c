/* Tests for synopses (xcat/synopsis.h, docs/formats/synopsis.md). */
#include "unity.h"
#include "xcat/synopsis.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static xc_err parse(const char *text, xc_synopsis *s)
{
    return xc_synopsis_parse(text, strlen(text), s);
}

static void test_fields(void)
{
    xc_synopsis s;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("<synopsis>\r\n"
                                       "<sourcename>Test Game-T</sourcename>\r\n"
                                       "<title>Test Game: Second Edition</title>\r\n"
                                       "<developer>Some Studio</developer>\r\n"
                                       "<publisher>A Publisher</publisher>\r\n"
                                       "<features_general>Players 1-4, 480p</features_general>\r\n"
                                       "<features_online>System Link 2-16</features_online>\r\n"
                                       "<esrb>T</esrb>\r\n"
                                       "<genre>Action, Racing</genre>\r\n"
                                       "<release_date>01 Nov 2004</release_date>\r\n"
                                       "<year>2004</year>\r\n"
                                       "<rating>7.9</rating>\r\n"
                                       "<titleid>58430002</titleid>\r\n"
                                       "<overview>Drive fast.\r\n  Then faster.</overview>\r\n"
                                       "</synopsis>\r\n",
                                       &s));
    TEST_ASSERT_EQUAL_STRING("Test Game: Second Edition", s.title);
    TEST_ASSERT_EQUAL_STRING("Some Studio", s.developer);
    TEST_ASSERT_EQUAL_STRING("A Publisher", s.publisher);
    TEST_ASSERT_EQUAL_STRING("Players 1-4, 480p", s.features);
    TEST_ASSERT_EQUAL_STRING("System Link 2-16", s.online);
    TEST_ASSERT_EQUAL_STRING("T", s.esrb);
    TEST_ASSERT_EQUAL_STRING("Action, Racing", s.genre);
    TEST_ASSERT_EQUAL_STRING("01 Nov 2004", s.release_date);
    TEST_ASSERT_EQUAL_STRING("2004", s.year);
    TEST_ASSERT_EQUAL_STRING("7.9", s.rating);
    TEST_ASSERT_EQUAL_STRING("58430002", s.title_id);
    TEST_ASSERT_EQUAL_STRING("Drive fast. Then faster.", s.overview); /* whitespace collapsed */
}

static void test_text(void)
{
    xc_synopsis s;
    /* A BOM, a declaration, a comment; entities, CDATA, markup inside, attributes. */
    TEST_ASSERT_EQUAL_INT(
        XC_OK,
        parse("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- made by hand -->\n"
              "<synopsis version=\"1\">"
              "<title lang=\"en\">Tom &amp; Jerry &lt;3 &#233;&#x20AC;&#0; &bogus;</title>"
              "<developer><![CDATA[A <b>&amp; B]]></developer>"
              "<publisher>One <i>Two</i>\tThree</publisher>"
              "<!-- <genre>commented out</genre> -->"
              "<year/>"
              "<esrb></esrb>"
              "<overview>\xC3\xA9t\xC3\xA9 \xFF bad \xE2\x82 cut</overview>"
              "<title>the second title doesn't count</title>"
              "</synopsis>",
              &s));
    TEST_ASSERT_EQUAL_STRING("Tom & Jerry <3 \xC3\xA9\xE2\x82\xAC&#0; &bogus;", s.title);
    TEST_ASSERT_EQUAL_STRING("A <b>&amp; B", s.developer);
    TEST_ASSERT_EQUAL_STRING("One Two Three", s.publisher);
    TEST_ASSERT_EQUAL_STRING("", s.genre);
    TEST_ASSERT_EQUAL_STRING("", s.year);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9t\xC3\xA9 ? bad ?? cut", s.overview);
}

static void test_cut(void)
{
    /* A field longer than its room: cut at a code point, never in one. */
    size_t n = 200;
    char *text = malloc(64 + 2 * n);
    TEST_ASSERT_NOT_NULL(text);
    char *p = text + sprintf(text, "<synopsis><esrb>");
    for (size_t i = 0; i < n; i++) {
        memcpy(p, "\xC3\xA9", 2);
        p += 2;
    }
    strcpy(p, "</esrb></synopsis>");
    xc_synopsis s;
    TEST_ASSERT_EQUAL_INT(XC_OK, parse(text, &s));
    TEST_ASSERT_EQUAL_size_t(14, strlen(s.esrb)); /* 7 of them in 16 bytes with the NUL */
    free(text);

    /* No end tag: to the file's end. No end of the root: what's there. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("<synopsis><title>Unended", &s));
    TEST_ASSERT_EQUAL_STRING("Unended", s.title);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("<synopsis><title>A</title><year>1999</year>", &s));
    TEST_ASSERT_EQUAL_STRING("1999", s.year);
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("<synopsis/>", &s));
    TEST_ASSERT_EQUAL_STRING("", s.title);
    /* </titlex> doesn't end <title>. */
    TEST_ASSERT_EQUAL_INT(XC_OK, parse("<synopsis><title>a</titlex>b</title></synopsis>", &s));
    TEST_ASSERT_EQUAL_STRING("ab", s.title);
}

static void test_refused(void)
{
    xc_synopsis s;
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("", &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("<game><title>x</title></game>", &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("<synopsis_Map><title>x</title>", &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("<synopsis", &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, parse("text <synopsis></synopsis>", &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT,
                          xc_synopsis_parse("<synopsis>", XC_SYNOPSIS_MAX_FILE + 1U, &s));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_synopsis_parse(NULL, 0, &s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fields);
    RUN_TEST(test_text);
    RUN_TEST(test_cut);
    RUN_TEST(test_refused);
    return UNITY_END();
}
