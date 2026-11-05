/* Tests for the theme (app/ui/theme.h, theme.c) on a fake backend that paints a pixel grid. */
#include "theme.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- A fake backend: fills paint characters into a grid; images are logged. */

#define GW 64
#define GH 48

static char grid[GH][GW];
static int fill_count[GH][GW]; /* how often the panel's fill colour painted each pixel */
static int image_w, image_h;   /* the fake background's size; 0 x 0: not loaded */
static int tinted_w;           /* UI_IMAGE_BACKGROUND_PANEL's width (image_h high); 0: none */
static ui_rect image_src;      /* of the last image drawn */
static int image_calls;
static int saves, backdrops;
static unsigned char bg_count[UI_SCREEN_H][UI_SCREEN_W]; /* background copies per pixel */
static int bg_pan = -1; /* if not -1, src.x - x of every copy: the pan */
static ui_color saved_shade;

static const ui_color fill_c = {1, 2, 3, 100};
static const ui_color border_c = {9, 9, 9, 255};

static void fake_fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    char mark = 'o'; /* another colour */
    if (memcmp(&c, &fill_c, sizeof c) == 0) {
        mark = 'f';
    } else if (memcmp(&c, &border_c, sizeof c) == 0) {
        mark = 'B';
    }
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            if (x >= 0 && y >= 0 && x < GW && y < GH) {
                grid[y][x] = mark;
                if (mark == 'f') {
                    fill_count[y][x]++;
                }
            }
        }
    }
}

/* Images are logged; the tinted background paints 'T' where it's drawn. */
static void fake_image(void *ctx, ui_image img, ui_rect src, int x, int y)
{
    (void)ctx;
    image_src = src;
    image_calls++;
    if (img == UI_IMAGE_BACKGROUND) {
        if (bg_pan != -1) {
            TEST_ASSERT_EQUAL_INT(bg_pan, src.x - x);
            TEST_ASSERT_EQUAL_INT(src.y, y);
        }
        for (int gy = y; gy < y + src.h; gy++) {
            for (int gx = x; gx < x + src.w; gx++) {
                TEST_ASSERT_TRUE(gx >= 0 && gy >= 0 && gx < UI_SCREEN_W && gy < UI_SCREEN_H);
                bg_count[gy][gx]++;
            }
        }
        return;
    }
    if (img != UI_IMAGE_BACKGROUND_PANEL) {
        return;
    }
    for (int gy = y; gy < y + src.h; gy++) {
        for (int gx = x; gx < x + src.w; gx++) {
            if (gx >= 0 && gy >= 0 && gx < GW && gy < GH) {
                grid[gy][gx] = 'T';
                fill_count[gy][gx]++;
            }
        }
    }
}

static void fake_image_size(void *ctx, ui_image img, int *w, int *h)
{
    (void)ctx;
    *w = img == UI_IMAGE_BACKGROUND_PANEL ? tinted_w : image_w;
    *h = img == UI_IMAGE_BACKGROUND_PANEL && tinted_w == 0 ? 0 : image_h;
}

static void fake_save_backdrop(void *ctx, ui_color shade)
{
    (void)ctx;
    saves++;
    saved_shade = shade;
}

static void fake_backdrop(void *ctx)
{
    (void)ctx;
    backdrops++;
}

static const ui_draw fake = {NULL,
                             fake_fill,
                             NULL,
                             NULL,
                             NULL,
                             NULL,
                             fake_image,
                             fake_image_size,
                             fake_save_backdrop,
                             fake_backdrop,
                             NULL};

static void clear(void)
{
    memset(grid, '.', sizeof grid);
    memset(fill_count, 0, sizeof fill_count);
}

/* ---- Tests */

static void test_pan_ends_and_middle(void)
{
    /* 800 px at 20 px/s is 40 s, and the two eases add half their length each. */
    const uint32_t leg = ui_pan_leg_ms(800);
    TEST_ASSERT_EQUAL_UINT32(40000U + UI_PAN_EASE_MS, leg);
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(0, 800));
    TEST_ASSERT_EQUAL_INT(400, ui_pan_x(leg / 2, 800));
    TEST_ASSERT_EQUAL_INT(800, ui_pan_x(leg, 800));
    TEST_ASSERT_EQUAL_INT(400, ui_pan_x(leg + leg / 2, 800));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(2 * leg, 800));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(12345, 0));
    TEST_ASSERT_EQUAL_INT(0, ui_pan_x(12345, -5));
    TEST_ASSERT_EQUAL_UINT32(0, ui_pan_leg_ms(0));
}

/* Checks a leg over `range` pixels, every 10 ms: never backwards, never faster than
   UI_PAN_SPEED, the way back mirrors the way there, and full speed between the eases. */
static void check_pan(int range)
{
    const uint32_t leg = ui_pan_leg_ms(range);
    int prev = 0;
    for (uint32_t t = 0; t <= leg; t += 10) {
        int x = ui_pan_x(t, range);
        TEST_ASSERT_TRUE(x >= prev);
        TEST_ASSERT_TRUE(x - prev <= 1); /* 20 px/s is 0.2 px per 10 ms */
        TEST_ASSERT_TRUE(x <= range);
        prev = x;
        if (t < leg) {
            TEST_ASSERT_EQUAL_INT(range - x, ui_pan_x(leg + t, range));
        }
    }
    TEST_ASSERT_EQUAL_INT(range, prev);
}

static void test_pan_is_smooth_and_symmetric(void)
{
    check_pan(800);
    check_pan(7); /* too short to reach full speed */
    check_pan(1);
    /* A short ease: the stop at an end lasts about a second, not tens. In the last 2 s of a
       leg it covers 30 px: 20 at full speed, then 10 slowing to a stop... */
    const uint32_t leg = ui_pan_leg_ms(800);
    TEST_ASSERT_INT_WITHIN(1, 30, 800 - ui_pan_x(leg - 2000U, 800));
    /* ...and at full speed, 1 s covers UI_PAN_SPEED pixels. */
    TEST_ASSERT_INT_WITHIN(1, (int)UI_PAN_SPEED, ui_pan_x(10000, 800) - ui_pan_x(9000, 800));
}

static void test_background(void)
{
    clear();
    image_w = 0;
    image_h = 0;
    image_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, ui_pan_range(&fake));
    ui_background(&fake, 100); /* no image: a fill */
    TEST_ASSERT_EQUAL_INT(0, image_calls);
    TEST_ASSERT_EQUAL_CHAR('o', grid[0][0]);

    image_w = 1440;
    image_h = 480;
    TEST_ASSERT_EQUAL_INT(800, ui_pan_range(&fake));
    ui_background(&fake, 100);
    TEST_ASSERT_EQUAL_INT(1, image_calls);
    TEST_ASSERT_EQUAL_INT(100, image_src.x);
    TEST_ASSERT_EQUAL_INT(UI_SCREEN_W, image_src.w);
    TEST_ASSERT_EQUAL_INT(UI_SCREEN_H, image_src.h);
    ui_background(&fake, 5000); /* clamped */
    TEST_ASSERT_EQUAL_INT(800, image_src.x);
    ui_background(&fake, -3);
    TEST_ASSERT_EQUAL_INT(0, image_src.x);

    image_w = 600; /* narrower than the screen: not used */
    TEST_ASSERT_EQUAL_INT(0, ui_pan_range(&fake));
}

static bool in_rect(ui_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* The background is copied once everywhere but inside the panels, clear of their cut
   corners, and the panels drawn after it cover what was left out. */
static void test_background_under(void)
{
    const ui_rect panels[] = {
        {2, 2, 40, 40},       /* in the grid, checked below */
        {300, 10, 100, 200},  /* beside the next, overlapping its rows */
        {380, 100, 200, 100}, /* overlapping the one before */
        {300, 260, 100, 20},  /* too short for a hole */
        {600, 400, 100, 200}, /* off the screen's edge */
    };
    const int n = (int)(sizeof panels / sizeof panels[0]);
    image_w = 1440;
    image_h = 480;
    tinted_w = 1440;
    bg_pan = 123;
    clear();
    memset(bg_count, 0, sizeof bg_count);
    ui_background_under(&fake, bg_pan, panels, n);
    for (int y = 0; y < UI_SCREEN_H; y++) {
        for (int x = 0; x < UI_SCREEN_W; x++) {
            bool hole = false;
            for (int i = 0; i < n; i++) {
                ui_rect h = {panels[i].x, panels[i].y + UI_CHAMFER, panels[i].w,
                             panels[i].h - 2 * UI_CHAMFER};
                hole = hole || in_rect(h, x, y);
            }
            TEST_ASSERT_EQUAL_INT(hole ? 0 : 1, bg_count[y][x]);
        }
    }
    ui_panel(&fake, panels[0], UI_CHAMFER, ui_col_panel, border_c);
    for (int y = 2 + UI_CHAMFER; y < 42 - UI_CHAMFER; y++) {
        for (int x = 2; x < 42; x++) {
            TEST_ASSERT_TRUE(grid[y][x] == 'T' || grid[y][x] == 'B');
        }
    }

    /* Without the tinted background the panels are blended: all of it is copied. */
    tinted_w = 0;
    memset(bg_count, 0, sizeof bg_count);
    ui_background_under(&fake, bg_pan, panels, n);
    for (int y = 0; y < UI_SCREEN_H; y++) {
        for (int x = 0; x < UI_SCREEN_W; x++) {
            TEST_ASSERT_EQUAL_INT(1, bg_count[y][x]);
        }
    }
    image_w = 0;
    image_h = 0;
    bg_pan = -1;
}

static void test_panel_shape(void)
{
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, fill_c, border_c);
    /* Cut corners: top left and bottom right stay empty, the others are square. */
    TEST_ASSERT_EQUAL_CHAR('.', grid[3][2]);
    TEST_ASSERT_EQUAL_CHAR('.', grid[3 + 19][2 + 29]);
    TEST_ASSERT_EQUAL_CHAR('B', grid[3][2 + 29]);      /* top right */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3 + 19][2]);      /* bottom left */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3][2 + 6]);       /* where the top edge starts */
    TEST_ASSERT_EQUAL_CHAR('.', grid[3][2 + 5]);       /* left of it, cut */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3 + 6][2]);       /* where the left edge starts */
    TEST_ASSERT_EQUAL_CHAR('f', grid[3 + 10][2 + 15]); /* inside */
    /* The diagonals are closed: every row has border at both ends, and nothing outside. */
    for (int y = 3; y < 3 + 20; y++) {
        int first = -1;
        int last = -1;
        for (int x = 0; x < GW; x++) {
            if (grid[y][x] != '.') {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        TEST_ASSERT_EQUAL_CHAR('B', grid[y][first]);
        TEST_ASSERT_EQUAL_CHAR('B', grid[y][last]);
        for (int x = first; x <= last; x++) {
            TEST_ASSERT_TRUE(grid[y][x] != '.');     /* no holes */
            TEST_ASSERT_TRUE(fill_count[y][x] <= 1); /* translucent fill painted once */
        }
    }
    TEST_ASSERT_EQUAL_CHAR('.', grid[2][10]);
    TEST_ASSERT_EQUAL_CHAR('.', grid[23][10]);
}

static void test_panel_square_and_tiny(void)
{
    clear();
    ui_panel(&fake, (ui_rect){0, 0, 10, 8}, 0, fill_c, border_c);
    TEST_ASSERT_EQUAL_CHAR('B', grid[0][0]);
    TEST_ASSERT_EQUAL_CHAR('B', grid[7][9]);
    TEST_ASSERT_EQUAL_CHAR('f', grid[4][4]);
    clear();
    ui_panel(&fake, (ui_rect){0, 0, 6, 4}, 50, fill_c, border_c); /* chamfer clamped */
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            TEST_ASSERT_TRUE(fill_count[y][x] <= 1);
            if (x >= 6 || y >= 4) {
                TEST_ASSERT_EQUAL_CHAR('.', grid[y][x]);
            }
        }
    }
}

/* After the background, panels in ui_col_panel are copied from the tinted background at the
   same pan, in the same shape; other fills, and panels over a backdrop, are blended. */
static void test_panel_tinted(void)
{
    image_w = 1440;
    image_h = 480;
    tinted_w = 1440;
    ui_background(&fake, 100);
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, ui_col_panel, border_c);
    TEST_ASSERT_EQUAL_CHAR('T', grid[3 + 10][2 + 15]);
    TEST_ASSERT_EQUAL_CHAR('.', grid[3][2]);      /* still cut */
    TEST_ASSERT_EQUAL_CHAR('B', grid[3][2 + 29]); /* border drawn over it */
    TEST_ASSERT_EQUAL_INT(2 + 100, image_src.x);  /* the same pan as the background */
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            TEST_ASSERT_TRUE(fill_count[y][x] <= 1);
        }
    }
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, fill_c, border_c); /* another fill: blended */
    TEST_ASSERT_EQUAL_CHAR('f', grid[3 + 10][2 + 15]);

    saves = 0;
    backdrops = 0;
    ui_backdrop_save(&fake);
    TEST_ASSERT_EQUAL_INT(1, saves);
    TEST_ASSERT_EQUAL_INT(1, backdrops); /* this frame shows it darkened too */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&saved_shade, &ui_col_shade, sizeof saved_shade));
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, ui_col_panel, border_c);
    TEST_ASSERT_EQUAL_CHAR('o', grid[3 + 10][2 + 15]); /* blended over the backdrop */

    ui_background(&fake, 0);
    backdrops = 0;
    ui_backdrop(&fake);
    TEST_ASSERT_EQUAL_INT(1, backdrops);
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, ui_col_panel, border_c);
    TEST_ASSERT_EQUAL_CHAR('o', grid[3 + 10][2 + 15]);

    tinted_w = 0; /* no tinted background: blended */
    ui_background(&fake, 0);
    clear();
    ui_panel(&fake, (ui_rect){2, 3, 30, 20}, 6, ui_col_panel, border_c);
    TEST_ASSERT_EQUAL_CHAR('o', grid[3 + 10][2 + 15]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pan_ends_and_middle);
    RUN_TEST(test_pan_is_smooth_and_symmetric);
    RUN_TEST(test_background);
    RUN_TEST(test_background_under);
    RUN_TEST(test_panel_shape);
    RUN_TEST(test_panel_square_and_tiny);
    RUN_TEST(test_panel_tinted);
    return UNITY_END();
}
