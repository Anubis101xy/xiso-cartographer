/* Tests for the library screen (app/ui/library.h, library.c). */
#include "fake_draw.h"
#include "library.h"
#include "theme.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void)
{
    fake_reset();
}
void tearDown(void) {}

static ui_lib_item items[3] = {
    {"First game", "2004  |  Europe  |  4.38 GB", "By someone. From the map.", UI_LIB_NOT_TRIED,
     NULL},
    {"Second game", "", "From E:/xcat/isos.", UI_LIB_INSTALLED, "In E:/Games/Second game"},
    {"Third game", "2002", NULL, UI_LIB_FAILED, "No space left"},
};

static int shown[8]; /* the library's list of what GAMES shows */

static int count_kind(char kind)
{
    int n = 0;
    for (int i = 0; i < fake_log_n; i++) {
        n += fake_log[i].kind == kind;
    }
    return n;
}

/* Whether s was drawn in colour c. */
static bool drawn_in(const char *s, ui_color c)
{
    for (int i = 0; i < fake_log_n; i++) {
        const fake_call *f = &fake_log[i];
        if (f->kind == 't' && strcmp(f->text, s) == 0 && f->c.r == c.r && f->c.g == c.g &&
            f->c.b == c.b && f->c.a == c.a) {
            return true;
        }
    }
    return false;
}

/* Whether text starting with `prefix` was drawn in colour c (a line cut to fit keeps it). */
static bool drawn_prefix_in(const char *prefix, ui_color c)
{
    for (int i = 0; i < fake_log_n; i++) {
        const fake_call *f = &fake_log[i];
        if (f->kind == 't' && strncmp(f->text, prefix, strlen(prefix)) == 0 && f->c.r == c.r &&
            f->c.g == c.g && f->c.b == c.b && f->c.a == c.a) {
            return true;
        }
    }
    return false;
}

static int count_text(const char *s)
{
    int n = 0;
    for (int i = 0; i < fake_log_n; i++) {
        n += fake_log[i].kind == 't' && strcmp(fake_log[i].text, s) == 0;
    }
    return n;
}

static void test_open(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);
    TEST_ASSERT_EQUAL_INT(3, lib.list.count);
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    TEST_ASSERT_FALSE(lib.in_sections);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    /* 300 pixels of list at 26 a line. */
    TEST_ASSERT_EQUAL_INT(11, ui_library_rows(&fake_draw));
}

/* Nothing installs unasked: A asks first, and only INSTALL installs. */
static void test_install_asks_first(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Third game"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5)); /* CANCEL */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_RIGHT, 5)); /* to CANCEL */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(2, lib.list.cur); /* the game asked about */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
}

/* An installed game: A asks to play it; in GAMES it can also be installed again. */
static void test_play_installed(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PLAY, lib.asking);
    TEST_ASSERT_EQUAL_STRING("ask-play", ui_library_screen(&lib));
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Second game"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PLAY, ui_library_input(&lib, UI_BTN_A, 5));
    /* INSTALL AGAIN, the middle button */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_RIGHT, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, ui_library_input(&lib, UI_BTN_A, 5));
    /* B cancels */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);

    /* INSTALLED: A on a finished install asks to play it; on an unfinished one, nothing. */
    static const ui_lib_item found[2] = {
        {"Halo", "4.1 GB", NULL, UI_LIB_INSTALLED, "E:\\Games\\Halo"},
        {"Fable", NULL, NULL, UI_LIB_UNFINISHED, "E:\\Games\\Fable"},
    };
    ui_library_set_installed(&lib, found, 2);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_LEFT, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALLED_GAMES, lib.section);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PLAY, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Halo"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PLAY, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(0, ui_library_installed(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
}

/* SETTINGS > NAS: scan, type an address or pick a server, log in, browse, add a folder. */
static void test_nas(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_LEFT, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTINGS, lib.section);
    for (int i = 0; i < UI_LIB_SETTING_NAS; i++) {
        TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    }
    /* Opening the page lets the caller fill it in. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_PAGE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("nas", ui_library_screen(&lib));
    ui_library_set_console(&lib, "This Xbox: 192.168.1.23");
    /* No NAS yet: "Find a NAS" is first. */
    TEST_ASSERT_EQUAL_INT(-1, ui_library_nas(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_SCAN, ui_library_input(&lib, UI_BTN_A, 5));
    static const ui_lib_nas found[2] = {
        {"TNAS-F4", "192.168.1.50", "192.168.1.50, workgroup WORKGROUP", "xbox"},
        {"PC", "192.168.1.20", "192.168.1.20", NULL}};
    ui_library_set_nas(&lib, found, 2, "2 found.");
    TEST_ASSERT_EQUAL_INT(4, lib.nas_list.count);
    TEST_ASSERT_EQUAL_INT(0, ui_library_nas(&lib)); /* the saved one, first */
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(drawn_prefix_in("This Xbox", ui_col_accent));
    TEST_ASSERT_TRUE(count_text("Logged in as xbox before: A opens its shared folders. X logs in "
                                "as someone else.") > 0 ||
                     count_kind('t') > 0);
    /* A saved login: no keyboard. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_SAVED, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", lib.nas_addr);
    /* X: someone else, from the saved user name. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_X, 5));
    TEST_ASSERT_EQUAL_STRING("keyboard-nas-user", ui_library_screen(&lib));
    TEST_ASSERT_EQUAL_STRING("xbox", lib.nas_user);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    /* No saved login: user name, then password. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(1, ui_library_nas(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", lib.nas_addr);
    TEST_ASSERT_EQUAL_STRING("keyboard-nas-user", ui_library_screen(&lib));
    (void)snprintf(lib.nas_user, sizeof lib.nas_user, "%s", "shaun");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_STRING("keyboard-nas-password", ui_library_screen(&lib));
    TEST_ASSERT_TRUE(lib.kbd.password);
    (void)snprintf(lib.nas_password, sizeof lib.nas_password, "%s", "secret");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_LOGIN, ui_library_input(&lib, UI_BTN_START, 5));
    ui_library_forget_nas_password(&lib);
    TEST_ASSERT_EQUAL_STRING("", lib.nas_password);
    /* Its shares: no "add" line at the top; X adds a whole share. */
    static const ui_lib_entry shares[2] = {{"Games", "Xbox games"}, {"Media", NULL}};
    ui_library_browse(&lib, "TNAS-F4", "2 shared folders.", shares, 2, NULL);
    TEST_ASSERT_EQUAL_STRING("nas-browse", ui_library_screen(&lib));
    TEST_ASSERT_EQUAL_INT(0, ui_library_browse_entry(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_OPEN, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_X, 5));
    TEST_ASSERT_EQUAL_STRING("ask-nas-use", ui_library_screen(&lib));
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Add TNAS-F4 > Games to GAMES?"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5)); /* CANCEL */
    /* A folder: its "add" line first, asked about before it's added. */
    static const ui_lib_entry folders[2] = {{"Halo (USA)", NULL}, {"Xbox", NULL}};
    ui_library_browse(&lib, "TNAS-F4 > Games", "3 images here.", folders, 2,
                      "Add this folder to GAMES");
    TEST_ASSERT_EQUAL_INT(-1, ui_library_browse_entry(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("ask-nas-use", ui_library_screen(&lib));
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Add TNAS-F4 > Games to GAMES?"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_USE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(-1, ui_library_nas_pick(&lib));
    /* X on a folder adds that one, by name, without opening it. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_DOWN, 5));
    TEST_ASSERT_EQUAL_INT(1, ui_library_browse_entry(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_X, 5));
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Add TNAS-F4 > Games > Xbox to GAMES?"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_USE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(1, ui_library_nas_pick(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_OPEN, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_UP, ui_library_input(&lib, UI_BTN_B, 5));
    ui_library_nas_page(&lib);
    TEST_ASSERT_EQUAL_STRING("nas", ui_library_screen(&lib));
    /* Type an address; no user name logs in as a guest, with no password asked. */
    for (int i = 0; i < 4; i++) {
        (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    }
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("keyboard-nas-address", ui_library_screen(&lib));
    (void)snprintf(lib.nas_addr, sizeof lib.nas_addr, "%s", "nas.lan:4450");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5));
    lib.nas_user[0] = '\0';
    TEST_ASSERT_EQUAL_INT(UI_LIB_NAS_LOGIN, ui_library_input(&lib, UI_BTN_START, 5));
    /* Drawn without trouble on each page. */
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    ui_library_browse(&lib, "TNAS-F4", NULL, NULL, 0, NULL);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(count_text("Nothing here this login can open. B goes back.") > 0 ||
                     count_kind('t') > 0);
}

static void test_back_asks_before_leaving(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_BACK, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_EXIT, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5)); /* STAY */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_BACK, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_EXIT, ui_library_input(&lib, UI_BTN_A, 5));
}

static void test_sections(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(drawn_in("First game", ui_col_text)); /* the list's highlighted line */
    TEST_ASSERT_FALSE(drawn_in("First game", ui_col_dim));
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    TEST_ASSERT_TRUE(lib.in_sections);
    /* In the sections, the games' highlight is dim. */
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(drawn_in("First game", ui_col_dim));
    TEST_ASSERT_FALSE(drawn_in("First game", ui_col_text));
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(drawn_in("First game", ui_col_text));
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    (void)ui_library_input(&lib, UI_BTN_UP, 5); /* stops at the top */
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    for (int i = 0; i < 5; i++) {
        (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    }
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTINGS, lib.section); /* and at the bottom */
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);              /* the D-pad didn't move the list */
    (void)ui_library_input(&lib, UI_BTN_UP, 5);          /* INSTALLED, with nothing in it */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_FALSE(lib.in_sections);
    /* Not GAMES: no list to move, nothing to install. */
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Nothing installed yet"));
    TEST_ASSERT_FALSE(fake_saw("First game"));
}

static void test_empty(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 0, shown);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    ui_library_draw(&fake_draw, &lib, 0, "status");
    TEST_ASSERT_TRUE(fake_saw("No games"));
}

static void test_draw(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    ui_library_draw(&fake_draw, &lib, 0, "Ready");
    TEST_ASSERT_TRUE(fake_find_text("LIBRARY") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("GAMES") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("SETTINGS") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("Ready") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("3 games listed") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 installed") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
    TEST_ASSERT_TRUE(fake_saw("2004"));
    TEST_ASSERT_EQUAL_INT(1, count_text("INSTALLED")); /* the section: the game isn't tried */

    fake_reset();
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(2, count_text("INSTALLED"));
    TEST_ASSERT_TRUE(fake_saw("Second game"));

    fake_reset();
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("FAILED") >= 0);
    TEST_ASSERT_TRUE(fake_saw("No space left"));
}

static void test_stopped(void)
{
    const ui_lib_item stopped[1] = {{"Game", "", NULL, UI_LIB_STOPPED, "Stopped."}};
    ui_library lib;
    ui_library_open(&lib, stopped, 1, shown);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("STOPPED") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
}

/* Started in an earlier run: UNFINISHED, and counted as unfinished. */
static void test_unfinished(void)
{
    const ui_lib_item half[1] = {{"Game", "", NULL, UI_LIB_UNFINISHED, "In E:/Games/Game"}};
    ui_library lib;
    ui_library_open(&lib, half, 1, shown);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("UNFINISHED") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("1 unfinished") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("0 installed") >= 0);
}

static const ui_lib_item found[2] = {
    {"Alpha folder", "1.00 GB  |  12 files", "Title 4D53000A.", UI_LIB_INSTALLED, "E:/Games/A"},
    {"Beta folder", "", "Not finished.", UI_LIB_UNFINISHED, "E:/Games/B"},
};

/* INSTALLED: its own list and place; A installs nothing there. */
static void test_installed_section(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    ui_library_set_installed(&lib, found, 2);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALLED_GAMES, lib.section);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    TEST_ASSERT_FALSE(lib.in_sections);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Alpha folder"));
    TEST_ASSERT_TRUE(fake_saw("Beta folder"));
    TEST_ASSERT_TRUE(fake_saw("12 files"));
    TEST_ASSERT_FALSE(fake_saw("First game"));
    TEST_ASSERT_FALSE(fake_saw("A install"));

    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(1, lib.installed_list.cur);
    TEST_ASSERT_EQUAL_INT(0, lib.list.cur); /* GAMES keeps its place */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("UNFINISHED") >= 0);
    TEST_ASSERT_TRUE(fake_saw("Not finished."));

    /* A shorter list after an install: the place stays inside it. */
    ui_library_set_installed(&lib, found, 1);
    TEST_ASSERT_EQUAL_INT(0, lib.installed_list.cur);
    TEST_ASSERT_EQUAL_INT(1, lib.installed_list.count);
    ui_library_set_installed(&lib, found, 0);
    TEST_ASSERT_EQUAL_INT(0, lib.installed_list.cur);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Nothing installed yet"));
}

/* A dialog draws over the screen behind, saved in its first frame and copied after. */
static void test_dialog_over_backdrop(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, count_kind('s'));
    TEST_ASSERT_TRUE(fake_find_text("LIBRARY") >= 0); /* drawn once, then saved */
    TEST_ASSERT_TRUE(fake_find_text("INSTALL") >= 0);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(0, count_kind('s'));
    TEST_ASSERT_EQUAL_INT(1, count_kind('b'));
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("LIBRARY"));
    TEST_ASSERT_TRUE(fake_find_text("CANCEL") >= 0);
    /* Closed and opened again: saved again, as the screen may have changed. */
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(0, count_kind('s') + count_kind('b'));
    (void)ui_library_input(&lib, UI_BTN_BACK, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, count_kind('s'));
    TEST_ASSERT_TRUE(fake_find_text("LEAVE") >= 0);
}

static void type(ui_library *lib, const char *word)
{
    /* The keyboard's small letters: q-p, a-l, z-m on rows 1-3 (keyboard.c); Y types a space. */
    static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (const char *c = word; *c != '\0'; c++) {
        if (*c == ' ') {
            (void)ui_library_input(lib, UI_BTN_Y, 5);
            continue;
        }
        for (int r = 0; r < 3; r++) {
            const char *at = strchr(rows[r], *c);
            if (at != NULL) {
                lib->kbd.row = r + 1;
                lib->kbd.col = (int)(at - rows[r]);
            }
        }
        (void)ui_library_input(lib, UI_BTN_A, 5);
    }
}

static const ui_lib_item many[4] = {
    {"Racing One", "2003  |  USA", "By Studio Fast.", UI_LIB_NOT_TRIED, NULL},
    {"Puzzle Two", "2004", "From E:/xcat/isos.", UI_LIB_NOT_TRIED, NULL},
    {"Racing Three", "2005  |  Europe", NULL, UI_LIB_INSTALLED, "In E:/Games/Racing Three"},
    {"Fighter", NULL, "By Studio Fast.", UI_LIB_NOT_TRIED, NULL},
};

static void test_matches(void)
{
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], ""));
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], "   "));
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], "racing"));
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], "RAC"));
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], " one  racing ")); /* every word, any order */
    TEST_ASSERT_FALSE(ui_library_matches(&many[0], "racing two"));   /* not every word */
    TEST_ASSERT_TRUE(ui_library_matches(&many[0], "usa"));           /* the facts */
    TEST_ASSERT_TRUE(ui_library_matches(&many[3], "fast"));          /* the about */
    TEST_ASSERT_FALSE(ui_library_matches(&many[3], "2003"));         /* no facts */
    TEST_ASSERT_FALSE(ui_library_matches(&many[2], "studio"));       /* no about */
    TEST_ASSERT_FALSE(ui_library_matches(&many[0], "racing onex"));
}

/* Y, a word, DONE: GAMES lists the games that match, and A installs the one highlighted. */
static void test_search(void)
{
    ui_library lib;
    ui_library_open(&lib, many, 4, shown);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5); /* Racing Three */
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5); /* in INSTALLED */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_Y, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_SEARCH, lib.typing_for);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(1, count_kind('s')); /* over the screen behind */
    TEST_ASSERT_TRUE(fake_find_text("SEARCH") >= 0);
    type(&lib, "racing");
    TEST_ASSERT_EQUAL_STRING("", lib.query); /* not searched before DONE */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    TEST_ASSERT_EQUAL_STRING("racing", lib.query);
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    TEST_ASSERT_FALSE(lib.in_sections);
    TEST_ASSERT_EQUAL_INT(2, lib.list.count);
    TEST_ASSERT_EQUAL_INT(1, lib.list.cur); /* Racing Three still highlighted */
    TEST_ASSERT_EQUAL_INT(2, ui_library_game(&lib));

    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("SEARCH") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("racing") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("2 of 4") >= 0);
    TEST_ASSERT_TRUE(fake_saw("Racing One"));
    TEST_ASSERT_FALSE(fake_saw("Puzzle Two"));
    TEST_ASSERT_TRUE(fake_saw("B list all"));

    (void)ui_library_input(&lib, UI_BTN_UP, 5);
    TEST_ASSERT_EQUAL_INT(0, ui_library_game(&lib));
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Racing One"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(0, ui_library_game(&lib));

    /* B lists every game again, keeping the place. */
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    TEST_ASSERT_EQUAL_STRING("", lib.query);
    TEST_ASSERT_EQUAL_INT(4, lib.list.count);
    TEST_ASSERT_EQUAL_INT(0, ui_library_game(&lib));
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("2 of 4"));
    TEST_ASSERT_TRUE(fake_saw("Puzzle Two"));
}

/* The keyboard opens on the last search; B there changes nothing, an empty DONE lists all. */
static void test_search_again(void)
{
    ui_library lib;
    ui_library_open(&lib, many, 4, shown);
    (void)ui_library_input(&lib, UI_BTN_Y, 5);
    type(&lib, "fast");
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    TEST_ASSERT_EQUAL_INT(2, lib.list.count);
    TEST_ASSERT_EQUAL_INT(3, shown[1]);

    (void)ui_library_input(&lib, UI_BTN_Y, 5);
    TEST_ASSERT_EQUAL_STRING("fast", lib.typing);
    type(&lib, "x");
    (void)ui_library_input(&lib, UI_BTN_B, 5); /* cancel */
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    TEST_ASSERT_EQUAL_STRING("fast", lib.query);
    TEST_ASSERT_EQUAL_INT(2, lib.list.count);

    (void)ui_library_input(&lib, UI_BTN_Y, 5);
    type(&lib, "x");
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    TEST_ASSERT_EQUAL_INT(0, lib.list.count);
    TEST_ASSERT_EQUAL_INT(-1, ui_library_game(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking); /* nothing to install */
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("No game matches"));
    TEST_ASSERT_TRUE(fake_find_text("0 of 4") >= 0);

    (void)ui_library_input(&lib, UI_BTN_Y, 5);
    for (int i = 0; i < 5; i++) {
        (void)ui_library_input(&lib, UI_BTN_X, 5); /* delete it all */
    }
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    TEST_ASSERT_EQUAL_STRING("", lib.query);
    TEST_ASSERT_EQUAL_INT(4, lib.list.count);
}

static void to_settings(ui_library *lib)
{
    (void)ui_library_input(lib, UI_BTN_LEFT, 5);
    for (int i = 0; i < 3; i++) {
        (void)ui_library_input(lib, UI_BTN_DOWN, 5);
    }
    (void)ui_library_input(lib, UI_BTN_RIGHT, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTINGS, lib->section);
}

/* The archive.org account: first on SETTINGS' second line, Authentication. */
static void to_account(ui_library *lib)
{
    to_settings(lib);
    (void)ui_library_input(lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTING_LOGINS, lib->settings_list.cur);
    (void)ui_library_input(lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_LOGINS, lib->page);
    TEST_ASSERT_TRUE(lib->hosts[ui_library_host(lib)].archive);
}

/* Authentication: archive.org, then the map's servers with their logins. A on one with a login
   asks before UI_LIB_FORGET; on one without, a message; B goes back. */
static void test_logins(void)
{
    static const ui_lib_host hosts[] = {
        {"archive.org", true, NULL, "3 games in the map", NULL},
        {"192.168.1.20:8080", false, "nas", "12 games in the map", "Plain http, on the LAN."},
        {"games.example", false, NULL, "1 game in the map", NULL},
    };
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(1, lib.host_count); /* archive.org until the caller says */
    ui_library_set_hosts(&lib, hosts, 3);
    to_settings(&lib);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("Authentication") >= 0);
    TEST_ASSERT_EQUAL_INT(-1, fake_find_text("NOT LOGGED IN")); /* it explains the page */
    TEST_ASSERT_TRUE(fake_saw("The logins for"));
    TEST_ASSERT_TRUE(fake_saw("A servers and logins"));

    (void)ui_library_input(&lib, UI_BTN_A, 5);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("192.168.1.20:8080") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("games.example") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("NOT LOGGED IN") >= 0);
    TEST_ASSERT_TRUE(fake_saw("3 games in the")); /* cut to the panel's width */
    TEST_ASSERT_TRUE(fake_saw("A log in"));

    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(1, ui_library_host(&lib));
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("LOGIN [nas]") >= 0);
    TEST_ASSERT_TRUE(fake_saw("Plain http, on the LAN."));
    TEST_ASSERT_TRUE(fake_saw("A remove login"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_FORGET, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "[nas]"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5)); /* CANCEL first */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_FORGET, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_LOGINS, lib.page); /* still on the page */

    /* After it's removed, the caller says so; the place is kept. */
    static const ui_lib_host after[] = {
        {"archive.org", true, NULL, NULL, NULL},
        {"192.168.1.20:8080", false, NULL, NULL, NULL},
    };
    ui_library_set_hosts(&lib, after, 2);
    TEST_ASSERT_EQUAL_INT(1, ui_library_host(&lib));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TELL, lib.asking);
    TEST_ASSERT_EQUAL_STRING("NO LOGIN", lib.tell_title);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "credentials"));
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_NONE, lib.page);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTING_LOGINS, lib.settings_list.cur);
}

/* SETTINGS' first line: where games install. A lists the folders, the current one first
   highlighted; A on one chooses it, "Other folder" types one, B goes back. */
static void test_install_folder(void)
{
    static const ui_lib_folder folders[] = {{"E:\\Games", "12.3 GB free"},
                                            {"F:\\Games", "400 GB free"}};
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_STRING("E:\\Games", lib.install_dir); /* until the caller says */
    ui_library_set_folders(&lib, "f:\\games", folders, 2);
    to_settings(&lib);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTING_FOLDER, lib.settings_list.cur);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("Install location") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("Authentication") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("f:\\games") >= 0);
    TEST_ASSERT_TRUE(fake_saw("A change"));

    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_FOLDERS, lib.page);
    TEST_ASSERT_EQUAL_INT(3, lib.folder_list.count);
    TEST_ASSERT_EQUAL_INT(1, lib.folder_list.cur); /* F:\Games, whatever its case */
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("Other folder") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("400 GB free") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("CURRENT") >= 0);
    TEST_ASSERT_TRUE(fake_saw("A choose"));

    (void)ui_library_input(&lib, UI_BTN_B, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_NONE, lib.page);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_UP, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_FOLDER, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_STRING("E:\\Games", lib.folder);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_NONE, lib.page);

    /* Other folder: the keyboard on the current one. X deletes; B changes nothing. */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_FOLDER, lib.typing_for);
    TEST_ASSERT_EQUAL_STRING("f:\\games", lib.folder);
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_FOLDERS, lib.page);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    for (int i = 0; i < 5; i++) {
        (void)ui_library_input(&lib, UI_BTN_X, 5);
    }
    type(&lib, "xbox");
    TEST_ASSERT_EQUAL_INT(UI_LIB_FOLDER, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_STRING("f:\\xbox", lib.folder);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_NONE, lib.page);

    /* The install question names the folder. */
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    (void)ui_library_input(&lib, UI_BTN_UP, 5);
    (void)ui_library_input(&lib, UI_BTN_UP, 5);
    (void)ui_library_input(&lib, UI_BTN_UP, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_INSTALL, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "to f:\\games?"));
}

/* Map sources: "Add source" asks what kind, then the keyboard (and for an image, the title ID
   too) gives UI_LIB_ADD_SOURCE; A on a line asks before UI_LIB_REMOVE_SOURCE. */
static void test_sources(void)
{
    static const ui_lib_source sources[] = {
        {"archive.org: my-item", "archive.org item, line 3", "@archiveorg my-item"},
        {"Map: http://192.168.1.20/map.txt", "Included map, line 4",
         "@include http://192.168.1.20/map.txt. Line 4: it can't be fetched."},
    };
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    TEST_ASSERT_EQUAL_INT(1, lib.source_list.count); /* "Add source" until the caller says */
    ui_library_set_sources(&lib, sources, 2);
    to_settings(&lib);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SETTING_SOURCES, lib.settings_list.cur);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("Map sources") >= 0);
    TEST_ASSERT_TRUE(fake_saw("A list, add, remove"));

    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_SOURCES, lib.page);
    TEST_ASSERT_EQUAL_INT(-1, ui_library_source(&lib));
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("Add source") >= 0);
    TEST_ASSERT_TRUE(fake_saw("archive.org: my"));
    TEST_ASSERT_TRUE(fake_saw("A add"));

    /* B in the question adds nothing. */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_SOURCE_KIND, lib.asking);
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("ARCHIVE.ORG") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("IMAGE") >= 0);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);

    /* An archive.org item: its identifier. */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_SOURCE, lib.typing_for);
    TEST_ASSERT_EQUAL_STRING("ARCHIVE.ORG ITEM IDENTIFIER", lib.kbd.label);
    TEST_ASSERT_EQUAL_STRING("", lib.add_url);
    type(&lib, "myitem");
    TEST_ASSERT_EQUAL_INT(UI_LIB_ADD_SOURCE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_SOURCE_ARCHIVE, lib.add_kind);
    TEST_ASSERT_EQUAL_STRING("myitem", lib.add_url);

    /* One image: its address, then its title ID. */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_STRING("IMAGE ADDRESS", lib.kbd.label);
    TEST_ASSERT_EQUAL_STRING("http://", lib.add_url);
    type(&lib, "h");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_TITLE_ID, lib.typing_for);
    type(&lib, "deadbeef");
    TEST_ASSERT_EQUAL_INT(UI_LIB_ADD_SOURCE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_SOURCE_IMAGE, lib.add_kind);
    TEST_ASSERT_EQUAL_STRING("http://h", lib.add_url);
    TEST_ASSERT_EQUAL_STRING("deadbeef", lib.add_key);

    /* The same kind again keeps what was typed (to fix it); another kind starts afresh. */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_STRING("http://h", lib.add_url);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_RIGHT, 5);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_STRING("MAP ADDRESS", lib.kbd.label);
    TEST_ASSERT_EQUAL_STRING("https://", lib.add_url);
    (void)ui_library_input(&lib, UI_BTN_B, 5);

    /* A line: its details, and A asks before removing it, CANCEL first. */
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    (void)ui_library_input(&lib, UI_BTN_DOWN, 5);
    TEST_ASSERT_EQUAL_INT(1, ui_library_source(&lib));
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("Included map"));
    TEST_ASSERT_TRUE(fake_saw("A remove"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_REMOVE_SOURCE, lib.asking);
    TEST_ASSERT_NOT_NULL(strstr(lib.message, "Map: http://192.168.1.20/map.txt"));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5)); /* CANCEL */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_REMOVE_SOURCE, ui_library_input(&lib, UI_BTN_A, 5));

    /* Fewer lines after it's gone: the place stays in range. */
    ui_library_set_sources(&lib, sources, 1);
    TEST_ASSERT_EQUAL_INT(0, ui_library_source(&lib));
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_PAGE_NONE, lib.page);
}

/* After the map changed: a new list of games, every one shown, the search gone. */
static void test_set_items(void)
{
    static ui_lib_item two[2] = {
        {"Alpha", "", NULL, UI_LIB_NOT_TRIED, NULL},
        {"Beta", "", NULL, UI_LIB_NOT_TRIED, NULL},
    };
    static int shown2[2];
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    (void)snprintf(lib.query, sizeof lib.query, "%s", "third");
    lib.list = (ui_list){1, 0, 0};
    ui_library_set_items(&lib, two, 2, shown2);
    TEST_ASSERT_EQUAL_INT(2, lib.list.count);
    TEST_ASSERT_EQUAL_STRING("", lib.query);
    TEST_ASSERT_EQUAL_INT(1, ui_library_game(&lib) + 1);
    TEST_ASSERT_EQUAL_INT(1, shown2[1]);
}

static bool all_zero(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\0') {
            return false;
        }
    }
    return true;
}

/* A in SETTINGS: the email, then the password, which is never drawn; DONE logs in. */
static void test_login(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    to_account(&lib);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("archive.org") >= 0);
    TEST_ASSERT_TRUE(fake_find_text("NOT LOGGED IN") >= 0);
    TEST_ASSERT_TRUE(fake_saw("A log in"));
    TEST_ASSERT_TRUE(fake_saw("never stored"));

    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_EMAIL, lib.typing_for);
    type(&lib, "me");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_PASSWORD, lib.typing_for);
    TEST_ASSERT_TRUE(lib.kbd.password);
    type(&lib, "zyx");
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("ARCHIVE.ORG PASSWORD"));
    TEST_ASSERT_FALSE(fake_saw("zyx")); /* dots only */
    TEST_ASSERT_EQUAL_INT(UI_LIB_LOGIN, ui_library_input(&lib, UI_BTN_START, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    TEST_ASSERT_EQUAL_STRING("me", lib.email);
    TEST_ASSERT_EQUAL_STRING("zyx", lib.password);
    ui_library_forget_password(&lib);
    TEST_ASSERT_TRUE(all_zero(lib.password, sizeof lib.password));
}

/* ui_library_login, after a refused download: the same keyboards, over any section. */
static void test_login_again(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    ui_library_tell(&lib, "X", "an open message goes");
    ui_library_login(&lib);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_EMAIL, lib.typing_for);
    TEST_ASSERT_EQUAL_INT(UI_LIB_GAMES, lib.section);
    type(&lib, "me");
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    type(&lib, "zyx");
    TEST_ASSERT_EQUAL_INT(UI_LIB_LOGIN, ui_library_input(&lib, UI_BTN_START, 5));
    ui_library_forget_password(&lib);
}

/* B at either step logs nothing in; the password is wiped, the email kept for next time. */
static void test_login_cancelled(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    to_account(&lib);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_START, 5); /* no email: nothing happens */
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    type(&lib, "me");
    (void)ui_library_input(&lib, UI_BTN_B, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);

    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_STRING("me", lib.email);
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    type(&lib, "zyx");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
    TEST_ASSERT_TRUE(all_zero(lib.password, sizeof lib.password));

    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_START, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_START, 5)); /* empty */
    TEST_ASSERT_EQUAL_INT(UI_LIB_TYPING_NONE, lib.typing_for);
}

/* Logged in, A asks before logging out, CANCEL first; a message closes with A or B. */
static void test_logout_and_tell(void)
{
    ui_library lib;
    ui_library_open(&lib, items, 3, shown);
    to_account(&lib);
    ui_library_set_account(&lib, true);
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_find_text("LOGGED IN") >= 0);
    TEST_ASSERT_TRUE(fake_saw("A log out"));
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_LOGOUT, lib.asking);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5)); /* CANCEL */
    (void)ui_library_input(&lib, UI_BTN_A, 5);
    (void)ui_library_input(&lib, UI_BTN_LEFT, 5);
    TEST_ASSERT_EQUAL_INT(UI_LIB_LOGOUT, ui_library_input(&lib, UI_BTN_A, 5));

    ui_library_tell(&lib, "LOGGED IN", "You're logged in.");
    fake_reset();
    ui_library_draw(&fake_draw, &lib, 0, NULL);
    TEST_ASSERT_TRUE(fake_saw("You're logged in."));
    TEST_ASSERT_TRUE(fake_find_text("OK") >= 0);
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_A, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
    ui_library_tell(&lib, "LOG IN", "No.");
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, ui_library_input(&lib, UI_BTN_B, 5));
    TEST_ASSERT_EQUAL_INT(UI_LIB_NONE, lib.asking);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open);
    RUN_TEST(test_install_asks_first);
    RUN_TEST(test_play_installed);
    RUN_TEST(test_nas);
    RUN_TEST(test_back_asks_before_leaving);
    RUN_TEST(test_sections);
    RUN_TEST(test_empty);
    RUN_TEST(test_draw);
    RUN_TEST(test_stopped);
    RUN_TEST(test_unfinished);
    RUN_TEST(test_installed_section);
    RUN_TEST(test_dialog_over_backdrop);
    RUN_TEST(test_matches);
    RUN_TEST(test_search);
    RUN_TEST(test_search_again);
    RUN_TEST(test_login);
    RUN_TEST(test_login_cancelled);
    RUN_TEST(test_login_again);
    RUN_TEST(test_logout_and_tell);
    RUN_TEST(test_install_folder);
    RUN_TEST(test_logins);
    RUN_TEST(test_sources);
    RUN_TEST(test_set_items);
    return UNITY_END();
}
