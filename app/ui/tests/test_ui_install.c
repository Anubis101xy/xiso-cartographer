/* Tests for the install screen (app/ui/install_view.h, install_view.c). */
#include "fake_draw.h"
#include "install_view.h"
#include "unity.h"

#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static void test_format_size(void)
{
    char s[24];
    ui_format_size(s, sizeof s, 0);
    TEST_ASSERT_EQUAL_STRING("0 bytes", s);
    ui_format_size(s, sizeof s, 999);
    TEST_ASSERT_EQUAL_STRING("999 bytes", s);
    ui_format_size(s, sizeof s, 1000);
    TEST_ASSERT_EQUAL_STRING("1.00 KB", s);
    ui_format_size(s, sizeof s, 740999);
    TEST_ASSERT_EQUAL_STRING("740 KB", s);
    ui_format_size(s, sizeof s, 999999);
    TEST_ASSERT_EQUAL_STRING("999 KB", s); /* rounded down, never up to "1000 KB" */
    ui_format_size(s, sizeof s, 12345678);
    TEST_ASSERT_EQUAL_STRING("12.3 MB", s);
    ui_format_size(s, sizeof s, 4380000000ULL);
    TEST_ASSERT_EQUAL_STRING("4.38 GB", s);
    ui_format_size(s, sizeof s, 8547991552ULL); /* a dual-layer disc */
    TEST_ASSERT_EQUAL_STRING("8.54 GB", s);
    ui_format_size(s, sizeof s, 999999999999ULL);
    TEST_ASSERT_EQUAL_STRING("999 GB", s);
    ui_format_size(s, sizeof s, 2000398934016ULL); /* a "2 TB" drive */
    TEST_ASSERT_EQUAL_STRING("2.00 TB", s);
    ui_format_size(s, sizeof s, 12000000000000000ULL); /* no bigger unit */
    TEST_ASSERT_EQUAL_STRING("12000 TB", s);
}

static void test_format_left(void)
{
    char s[48];
    ui_format_left(s, sizeof s, 1000000, 0);
    TEST_ASSERT_EQUAL_STRING("", s);
    ui_format_left(s, sizeof s, 30000000, 100); /* 30 MB at 10 MB/s */
    TEST_ASSERT_EQUAL_STRING("less than a minute left", s);
    ui_format_left(s, sizeof s, 60000000, 10); /* 60 MB at 1 MB/s: a minute */
    TEST_ASSERT_EQUAL_STRING("about 1 minute left", s);
    ui_format_left(s, sizeof s, 600000000, 10);
    TEST_ASSERT_EQUAL_STRING("about 10 minutes left", s);
    ui_format_left(s, sizeof s, 601000000, 10); /* rounded up */
    TEST_ASSERT_EQUAL_STRING("about 11 minutes left", s);
    ui_format_left(s, sizeof s, 3600000000ULL, 10);
    TEST_ASSERT_EQUAL_STRING("about 1 hour left", s);
    ui_format_left(s, sizeof s, 7500000000ULL, 10);
    TEST_ASSERT_EQUAL_STRING("about 2 hours 5 minutes left", s);
    ui_format_left(s, sizeof s, 3660000000ULL, 10);
    TEST_ASSERT_EQUAL_STRING("about 1 hour 1 minute left", s);
}

/* B asks before stopping, and CONTINUE is the default. */
static void test_stop_asks_first(void)
{
    ui_install_view v;
    ui_install_open(&v, "A game");
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_A));
    TEST_ASSERT_FALSE(v.stopping);
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_B));
    TEST_ASSERT_TRUE(v.stopping);
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_A)); /* CONTINUE */
    TEST_ASSERT_FALSE(v.stopping);
    (void)ui_install_input(&v, UI_BTN_B);
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_B)); /* B: CONTINUE */
    TEST_ASSERT_FALSE(v.stopping);
    (void)ui_install_input(&v, UI_BTN_B);
    (void)ui_install_input(&v, UI_BTN_LEFT);
    TEST_ASSERT_EQUAL_INT(UI_INST_STOP, ui_install_input(&v, UI_BTN_A));
    TEST_ASSERT_FALSE(v.stopping);
}

static void test_end(void)
{
    ui_install_view v;
    ui_install_open(&v, "A game");
    ui_install_end(&v, true, "In E:/Games/A game, every file checked.");
    TEST_ASSERT_TRUE(v.ended);
    TEST_ASSERT_EQUAL_STRING("INSTALLED", v.dialog.title);
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_RIGHT));
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_A));

    ui_install_open(&v, "A game");
    ui_install_end(&v, false, "No space left on the hard disk.");
    TEST_ASSERT_EQUAL_STRING("NOT INSTALLED", v.dialog.title);
    TEST_ASSERT_EQUAL_INT(UI_INST_RETRY, ui_install_input(&v, UI_BTN_A));
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_B));
    (void)ui_install_input(&v, UI_BTN_RIGHT);
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_A)); /* BACK */
    /* While it shows, B doesn't ask to stop. */
    (void)ui_install_input(&v, UI_BTN_B);
    TEST_ASSERT_FALSE(v.stopping);

    /* A refused login: LOG IN instead of RETRY, and B still goes back. */
    ui_install_end_login(&v, "The login for [archive] is missing or expired.");
    TEST_ASSERT_EQUAL_STRING("NOT INSTALLED", v.dialog.title);
    TEST_ASSERT_EQUAL_STRING("LOG IN", v.dialog.choices[0]);
    TEST_ASSERT_EQUAL_STRING("BACK", v.dialog.choices[1]);
    TEST_ASSERT_EQUAL_INT(UI_INST_LOGIN, ui_install_input(&v, UI_BTN_A));
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_B));
    fake_reset();
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("LOG IN") >= 0);
    /* The next ending is an ordinary one again. */
    ui_install_end(&v, false, "No space left on the hard disk.");
    TEST_ASSERT_EQUAL_INT(UI_INST_RETRY, ui_install_input(&v, UI_BTN_A));

    /* Nothing to retry: BACK alone, with A or B. */
    ui_install_end_back(&v, "Not an Xbox disc image.");
    TEST_ASSERT_EQUAL_STRING("NOT INSTALLED", v.dialog.title);
    TEST_ASSERT_EQUAL_STRING("BACK", v.dialog.choices[0]);
    TEST_ASSERT_NULL(v.dialog.choices[1]);
    TEST_ASSERT_EQUAL_INT(UI_INST_NONE, ui_install_input(&v, UI_BTN_RIGHT));
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_A));
    TEST_ASSERT_EQUAL_INT(UI_INST_DONE, ui_install_input(&v, UI_BTN_B));
    fake_reset();
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("RETRY"));
}

static void test_draw(void)
{
    ui_install_view v;
    ui_install_open(&v, "A game");
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("CONNECTING") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("A game") >= 0);
    TEST_ASSERT_FALSE(fake_saw("%")); /* nothing known yet */
    TEST_ASSERT_FALSE(fake_saw("File"));

    fake_reset();
    v.phase = UI_INST_WRITING;
    v.done = 1500000000ULL;
    v.total = 4380000000ULL;
    v.file = 3;
    v.files = 120;
    v.speed = 81;
    ui_install_status(&v, "Retrying in %d s", 4);
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("WRITING FILES") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("34 %") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1.50 GB of 4.38 GB") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("File 3 of 120") >= 0);
    TEST_ASSERT_TRUE(fake_saw("8.1 MB/s, about 6 minutes left"));
    TEST_ASSERT_TRUE(fake_find_text("Retrying in 4 s") >= 0);
    TEST_ASSERT_TRUE(fake_saw("files to E:\\Games.")); /* no folder given */
    fake_reset();
    v.dir = "F:\\Games";
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_saw("files to F:\\Games."));

    /* The stop dialog over the screen behind, saved once. */
    (void)ui_install_input(&v, UI_BTN_B);
    fake_reset();
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_TRUE(fake_find_text("STOP") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("WRITING FILES") >= 0);
    fake_reset();
    ui_install_draw(&fake_draw, &v, 0);
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("WRITING FILES"));
    TEST_ASSERT_TRUE(fake_find_text("CONTINUE") >= 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_format_size);
    RUN_TEST(test_format_left);
    RUN_TEST(test_stop_asks_first);
    RUN_TEST(test_end);
    RUN_TEST(test_draw);
    return UNITY_END();
}
