/* Tests for playing a UI script (app/ui/script.h, script.c) and the screens' names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install_view.h"
#include "library.h"
#include "script.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static xc_ui_script script; /* 60 KB: not on the stack */

static void load(const char *text)
{
    xc_ui_script_error err;
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_ui_script_parse(text, strlen(text), &script, &err),
                                  err.message);
}

static void test_plays_presses_waits_and_notes(void)
{
    load("# xcat-ui-script v1\nnote Start\npress down 2\nwait 1000\npress a\n");
    ui_script r;
    ui_script_start(&r, &script);
    TEST_ASSERT_EQUAL_INT(UI_SCRIPT_RUNNING, r.state);
    TEST_ASSERT_EQUAL_STRING("Script: line 2, command 1 of 4", r.status);
    char log[200];

    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 0, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("Script: Start", log);
    /* Each press comes UI_SCRIPT_GAP_MS after the one before. */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 0, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("", log);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 299, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_DOWN, ui_script_next(&r, 300, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 400, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_DOWN, ui_script_next(&r, 600, "games", log, sizeof log));
    /* The wait. */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 700, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("Script: line 4, command 3 of 4", r.status);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1699, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1700, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1700, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_A, ui_script_next(&r, 2000, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 2001, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_SCRIPT_PASSED, r.state);
    TEST_ASSERT_EQUAL_STRING("Script passed: 4 commands, 0 screens checked", log);
    TEST_ASSERT_EQUAL_STRING(log, r.status);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 9000, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("", log);
}

static void test_expect_waits_for_its_screen(void)
{
    load("# xcat-ui-script v1\nexpect install-ok 5000\nexpect games 100\npress b\n");
    ui_script r;
    ui_script_start(&r, &script);
    char log[200];
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE,
                          ui_script_next(&r, 0xFFFFF000U, "installing", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("", log);
    /* Across the wrap at 2^32 ms. */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 100, "installing", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_SCRIPT_RUNNING, r.state);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 200, "install-ok", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("Script: line 2: on install-ok", log);
    TEST_ASSERT_EQUAL_UINT32(1, r.checks);

    /* The next expect times out: the script stops there, and presses nothing more. */
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1000, "install-ok", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1099, "install-ok", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_SCRIPT_RUNNING, r.state);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 1100, "install-ok", log, sizeof log));
    TEST_ASSERT_EQUAL_INT(UI_SCRIPT_FAILED, r.state);
    TEST_ASSERT_EQUAL_STRING("Script FAILED at line 3: expected games, on install-ok after 100 ms",
                             log);
    TEST_ASSERT_EQUAL_STRING(log, r.status);
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 5000, "games", log, sizeof log));
    TEST_ASSERT_EQUAL_STRING("", log);
}

static void test_every_button(void)
{
    char text[64];
    for (int k = 0; k < XC_UI_KEY_COUNT_; k++) {
        (void)snprintf(text, sizeof text, "# xcat-ui-script v1\npress %s\n",
                       xc_ui_key_name((xc_ui_key)k));
        load(text);
        ui_script r;
        ui_script_start(&r, &script);
        char log[200];
        TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_next(&r, 0, "games", log, sizeof log));
        ui_button b = ui_script_next(&r, UI_SCRIPT_GAP_MS, "games", log, sizeof log);
        TEST_ASSERT_EQUAL_INT_MESSAGE(UI_BTN_UP + k, b, text); /* same order, after NONE */
        TEST_ASSERT_TRUE(b > UI_BTN_NONE && b < UI_BTN_COUNT_);
    }
    TEST_ASSERT_EQUAL_INT(UI_BTN_NONE, ui_script_button(XC_UI_KEY_COUNT_));
}

static void test_check_names(void)
{
    char msg[160];
    load("# xcat-ui-script v1\nexpect games\nexpect install-ok\nexpect keyboard-title-id\n");
    TEST_ASSERT_NULL(ui_script_check(&script, msg, sizeof msg));
    load("# xcat-ui-script v1\nexpect games\n\nexpect game\n");
    TEST_ASSERT_EQUAL_STRING("line 4: the app has no screen \"game\"",
                             ui_script_check(&script, msg, sizeof msg));
}

static ui_lib_item items[] = {
    {"Alpha", "", "", UI_LIB_NOT_TRIED, NULL, false},
    {"Beta", "", "", UI_LIB_NOT_TRIED, NULL, false},
};

/* The library's names, as the buttons reach them; each one the script knows. */
static void see(const ui_library *lib, const char *want)
{
    TEST_ASSERT_EQUAL_STRING(want, ui_library_screen(lib));
    TEST_ASSERT_TRUE_MESSAGE(ui_script_screen_known(want), want);
}

static void test_library_names(void)
{
    static ui_library lib;
    int shown[2];
    ui_library_open(&lib, items, 2, shown);
    see(&lib, "games");
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "ask-install");
    ui_library_input(&lib, UI_BTN_B, 8);
    see(&lib, "games");
    ui_library_input(&lib, UI_BTN_Y, 8);
    see(&lib, "keyboard-search");
    ui_library_input(&lib, UI_BTN_B, 8);
    ui_library_input(&lib, UI_BTN_BACK, 8);
    see(&lib, "ask-exit");
    ui_library_input(&lib, UI_BTN_B, 8);
    ui_library_input(&lib, UI_BTN_LEFT, 8); /* to the sections */
    ui_library_input(&lib, UI_BTN_DOWN, 8);
    see(&lib, "installed");
    ui_library_input(&lib, UI_BTN_DOWN, 8);
    see(&lib, "settings");
    ui_library_input(&lib, UI_BTN_RIGHT, 8);
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "folders");
    ui_library_input(&lib, UI_BTN_B, 8);
    see(&lib, "settings");
    ui_library_input(&lib, UI_BTN_DOWN, 8);
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "logins");
    ui_library_input(&lib, UI_BTN_A, 8); /* archive.org, logged out: the email */
    see(&lib, "keyboard-email");
    ui_library_input(&lib, UI_BTN_B, 8);
    ui_library_input(&lib, UI_BTN_B, 8);
    ui_library_input(&lib, UI_BTN_DOWN, 8);
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "sources");
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "ask-kind");
    ui_library_input(&lib, UI_BTN_A, 8);
    see(&lib, "keyboard-source");
    ui_library_input(&lib, UI_BTN_B, 8);
    see(&lib, "sources");
    ui_library_tell(&lib, "NOTE", "Something.");
    see(&lib, "message");
}

static void test_install_names(void)
{
    static ui_install_view v;
    ui_install_open(&v, "Alpha");
    TEST_ASSERT_EQUAL_STRING("installing", ui_install_screen(&v));
    ui_install_input(&v, UI_BTN_B);
    TEST_ASSERT_EQUAL_STRING("ask-stop", ui_install_screen(&v));
    ui_install_input(&v, UI_BTN_B); /* CONTINUE */
    TEST_ASSERT_EQUAL_STRING("installing", ui_install_screen(&v));
    ui_install_end(&v, true, "Done.");
    TEST_ASSERT_EQUAL_STRING("install-ok", ui_install_screen(&v));
    ui_install_end(&v, false, "No.");
    TEST_ASSERT_EQUAL_STRING("install-failed", ui_install_screen(&v));
    ui_install_end_back(&v, "No.");
    TEST_ASSERT_EQUAL_STRING("install-failed", ui_install_screen(&v));
    ui_install_end_login(&v, "No.");
    TEST_ASSERT_EQUAL_STRING("install-failed", ui_install_screen(&v));
    TEST_ASSERT_TRUE(ui_script_screen_known("installing") && ui_script_screen_known("ask-stop") &&
                     ui_script_screen_known("install-ok") &&
                     ui_script_screen_known("install-failed"));
}

/* The walkthrough the exit test runs (tools/ui/walkthrough.txt) reads, and names only screens
   the app has. */
static void test_walkthrough(void)
{
    FILE *f = fopen(XCAT_WALKTHROUGH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, XCAT_WALKTHROUGH);
    static char text[XC_UI_SCRIPT_MAX_FILE + 1];
    size_t n = fread(text, 1, sizeof text, f);
    (void)fclose(f);
    xc_ui_script_error err;
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_ui_script_parse(text, n, &script, &err), err.message);
    TEST_ASSERT_TRUE(script.count > 0);
    char msg[160];
    TEST_ASSERT_NULL_MESSAGE(ui_script_check(&script, msg, sizeof msg), msg);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plays_presses_waits_and_notes);
    RUN_TEST(test_expect_waits_for_its_screen);
    RUN_TEST(test_every_button);
    RUN_TEST(test_check_names);
    RUN_TEST(test_library_names);
    RUN_TEST(test_install_names);
    RUN_TEST(test_walkthrough);
    return UNITY_END();
}
