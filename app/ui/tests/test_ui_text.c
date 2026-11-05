/* Tests for the UI text helpers (app/ui/ui.h, text.c) on a fake drawing backend. */
#include "ui.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- A fake backend: every code point is 10 px wide, a line 20 px high; draws are logged. */

#define CP_W 10
#define LINE_H 20

typedef struct drawn {
    int x, y;
    char text[128];
} drawn;

static drawn log_[32];
static int log_n;

static int cps(const char *s, size_t n)
{
    int count = 0;
    for (size_t i = 0; i < n; i = ui_utf8_next(s, n, i)) {
        count++;
    }
    return count;
}

static void fake_fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    (void)r;
    (void)c;
}

static int fake_text(void *ctx, ui_font f, int x, int y, ui_color c, const char *s, size_t n)
{
    (void)ctx;
    (void)f;
    (void)c;
    if (log_n < (int)(sizeof log_ / sizeof log_[0])) {
        drawn *d = &log_[log_n++];
        d->x = x;
        d->y = y;
        (void)snprintf(d->text, sizeof d->text, "%.*s", (int)n, s);
    }
    return cps(s, n) * CP_W;
}

static int fake_width(void *ctx, ui_font f, const char *s, size_t n)
{
    (void)ctx;
    (void)f;
    return cps(s, n) * CP_W;
}

static int fake_line(void *ctx, ui_font f)
{
    (void)ctx;
    (void)f;
    return LINE_H;
}

static void fake_clip(void *ctx, const ui_rect *r)
{
    (void)ctx;
    (void)r;
}

static const ui_draw fake = {NULL, fake_fill, fake_text, fake_width, fake_line, fake_clip,
                             NULL, NULL,      NULL,      NULL,       NULL};
static const ui_color white = {255, 255, 255, 255};

/* The text of every draw since the last call, joined with "|". */
static const char *drawn_text(void)
{
    static char out[512];
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < log_n && len < sizeof out; i++) {
        int n = snprintf(out + len, sizeof out - len, "%s%s", i > 0 ? "|" : "", log_[i].text);
        len += n > 0 ? (size_t)n : 0;
    }
    log_n = 0;
    return out;
}

/* ---- Tests */

static void test_safe_area(void)
{
    ui_rect r = ui_safe_area();
    TEST_ASSERT_EQUAL_INT(32, r.x);
    TEST_ASSERT_EQUAL_INT(24, r.y);
    TEST_ASSERT_EQUAL_INT(576, r.w);
    TEST_ASSERT_EQUAL_INT(432, r.h);
}

static void test_utf8_next(void)
{
    const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xAE";
    size_t n = strlen(s);
    TEST_ASSERT_EQUAL_size_t(1, ui_utf8_next(s, n, 0));
    TEST_ASSERT_EQUAL_size_t(3, ui_utf8_next(s, n, 1));
    TEST_ASSERT_EQUAL_size_t(6, ui_utf8_next(s, n, 3));
    TEST_ASSERT_EQUAL_size_t(10, ui_utf8_next(s, n, 6));
    /* Not UTF-8: one byte each, so drawing never stalls. */
    TEST_ASSERT_EQUAL_size_t(1, ui_utf8_next("\x80x", 2, 0));
    TEST_ASSERT_EQUAL_size_t(1, ui_utf8_next("\xC3x", 2, 0));
    TEST_ASSERT_EQUAL_size_t(1, ui_utf8_next("\xE2\x82", 2, 0)); /* cut short */
    TEST_ASSERT_EQUAL_size_t(1, ui_utf8_next("\xFF", 1, 0));
}

static void test_fit(void)
{
    TEST_ASSERT_EQUAL_size_t(5, ui_fit(&fake, UI_FONT_BODY, "hello", 5, 50));
    TEST_ASSERT_EQUAL_size_t(3, ui_fit(&fake, UI_FONT_BODY, "hello", 5, 39));
    TEST_ASSERT_EQUAL_size_t(0, ui_fit(&fake, UI_FONT_BODY, "hello", 5, 9));
    TEST_ASSERT_EQUAL_size_t(0, ui_fit(&fake, UI_FONT_BODY, "hello", 5, 0));
    TEST_ASSERT_EQUAL_size_t(0, ui_fit(&fake, UI_FONT_BODY, "hello", 5, -5));
    /* Never inside a code point: "h\xC3\xA9llo" in 20 px is "h" + "é", 3 bytes. */
    TEST_ASSERT_EQUAL_size_t(3, ui_fit(&fake, UI_FONT_BODY, "h\xC3\xA9llo", 6, 20));
    TEST_ASSERT_EQUAL_size_t(1, ui_fit(&fake, UI_FONT_BODY, "h\xC3\xA9llo", 6, 19));
}

static void test_text_fit(void)
{
    log_n = 0;
    TEST_ASSERT_EQUAL_INT(50, ui_text_fit(&fake, UI_FONT_BODY, 0, 0, 50, white, "hello"));
    TEST_ASSERT_EQUAL_STRING("hello", drawn_text());
    /* Cut: room is kept for the ellipsis, and a space before it is dropped. */
    TEST_ASSERT_EQUAL_INT(60, ui_text_fit(&fake, UI_FONT_BODY, 0, 0, 60, white, "hello world"));
    TEST_ASSERT_EQUAL_STRING("hello|" UI_ELLIPSIS, drawn_text());
    TEST_ASSERT_EQUAL_INT(50, ui_text_fit(&fake, UI_FONT_BODY, 0, 0, 59, white, "abcd efgh"));
    TEST_ASSERT_EQUAL_STRING("abcd|" UI_ELLIPSIS, drawn_text());
    /* Only the ellipsis fits; nothing fits. */
    TEST_ASSERT_EQUAL_INT(10, ui_text_fit(&fake, UI_FONT_BODY, 0, 0, 15, white, "hello"));
    TEST_ASSERT_EQUAL_STRING(UI_ELLIPSIS, drawn_text());
    TEST_ASSERT_EQUAL_INT(0, ui_text_fit(&fake, UI_FONT_BODY, 0, 0, 5, white, "hello"));
    TEST_ASSERT_EQUAL_STRING("", drawn_text());
}

/* The lines s wraps into at max_w, joined with "|". */
static const char *wrapped(const char *s, int max_w)
{
    static char out[256];
    out[0] = '\0';
    size_t n = strlen(s);
    for (size_t at = 0; at < n;) {
        size_t next = 0;
        size_t len = ui_wrap(&fake, UI_FONT_BODY, s + at, n - at, max_w, &next);
        TEST_ASSERT_TRUE(next > 0); /* always moves on */
        (void)snprintf(out + strlen(out), sizeof out - strlen(out), "%s%.*s", at > 0 ? "|" : "",
                       (int)len, s + at);
        at += next;
    }
    return out;
}

static void test_wrap(void)
{
    TEST_ASSERT_EQUAL_STRING("the quick|brown fox", wrapped("the quick brown fox", 100));
    TEST_ASSERT_EQUAL_STRING("the quick brown fox", wrapped("the quick brown fox", 190));
    TEST_ASSERT_EQUAL_STRING("the|quick|brown|fox", wrapped("the quick brown fox", 50));
    TEST_ASSERT_EQUAL_STRING("abcde|fghij|k", wrapped("abcdefghijk", 50)); /* a long word */
    TEST_ASSERT_EQUAL_STRING("a|b|c", wrapped("abc", 5)); /* narrower than a code point */
    TEST_ASSERT_EQUAL_STRING("ab|cd", wrapped("ab\ncd", 100));
    TEST_ASSERT_EQUAL_STRING("|ab", wrapped("\nab", 100));
    TEST_ASSERT_EQUAL_STRING("ab||cd", wrapped("ab\n\ncd", 100));
    TEST_ASSERT_EQUAL_STRING("one|two", wrapped("one   two", 40)); /* spaces at the break */
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9\xC3\xA9|\xC3\xA9", wrapped("\xC3\xA9\xC3\xA9\xC3\xA9", 20));
}

static void test_text_wrap(void)
{
    ui_rect r = {10, 100, 100, 45}; /* two lines of 20 px */
    log_n = 0;
    TEST_ASSERT_EQUAL_INT(2, ui_text_wrap(&fake, UI_FONT_BODY, r, white, "the quick brown fox"));
    TEST_ASSERT_EQUAL_STRING("the quick|brown fox", drawn_text());
    /* More than fits: the last line ends with an ellipsis, made room for if needed. */
    TEST_ASSERT_EQUAL_INT(
        2, ui_text_wrap(&fake, UI_FONT_BODY, r, white, "the quick brown fox jumps over"));
    TEST_ASSERT_EQUAL_STRING("the quick|brown fox|" UI_ELLIPSIS, drawn_text());
    TEST_ASSERT_EQUAL_INT(2,
                          ui_text_wrap(&fake, UI_FONT_BODY, r, white, "the quick abcdefghij klm"));
    TEST_ASSERT_EQUAL_STRING("the quick|abcdefghi|" UI_ELLIPSIS, drawn_text());
    /* Positions: lines start at r.x, LINE_H apart. */
    (void)ui_text_wrap(&fake, UI_FONT_BODY, r, white, "a b");
    TEST_ASSERT_EQUAL_INT(10, log_[0].x);
    TEST_ASSERT_EQUAL_INT(100, log_[0].y);
    log_n = 0;
    (void)ui_text_wrap(&fake, UI_FONT_BODY, r, white, "a\nb");
    TEST_ASSERT_EQUAL_INT(120, log_[1].y);
    log_n = 0;
    ui_rect none = {0, 0, 100, 19};
    TEST_ASSERT_EQUAL_INT(0, ui_text_wrap(&fake, UI_FONT_BODY, none, white, "a"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_safe_area);
    RUN_TEST(test_utf8_next);
    RUN_TEST(test_fit);
    RUN_TEST(test_text_fit);
    RUN_TEST(test_wrap);
    RUN_TEST(test_text_wrap);
    return UNITY_END();
}
