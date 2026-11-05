/* The fake drawing backend of the widget tests (fake_draw.h). */
#include "fake_draw.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

fake_call fake_log[FAKE_LOG];
int fake_log_n;

static int cps(const char *s, size_t n)
{
    int count = 0;
    for (size_t i = 0; i < n; i = ui_utf8_next(s, n, i)) {
        count++;
    }
    return count;
}

/* The next entry of the log, empty but for its kind. */
static fake_call *next_call(char kind)
{
    if (fake_log_n >= FAKE_LOG) {
        TEST_FAIL_MESSAGE("the fake backend's log is full: raise FAKE_LOG");
    }
    fake_call *c = &fake_log[fake_log_n++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

static void fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    fake_call *call = next_call('f');
    call->r = r;
    call->c = c;
}

static int text(void *ctx, ui_font f, int x, int y, ui_color c, const char *s, size_t n)
{
    (void)ctx;
    fake_call *call = next_call('t');
    call->font = f;
    call->r = (ui_rect){x, y, cps(s, n) * FAKE_CP_W, FAKE_LINE_H};
    call->c = c;
    (void)snprintf(call->text, sizeof call->text, "%.*s", (int)n, s);
    return cps(s, n) * FAKE_CP_W;
}

static int width(void *ctx, ui_font f, const char *s, size_t n)
{
    (void)ctx;
    fake_call *call = next_call('w');
    call->font = f;
    (void)snprintf(call->text, sizeof call->text, "%.*s", (int)n, s);
    return cps(s, n) * FAKE_CP_W;
}

static int line_height(void *ctx, ui_font f)
{
    (void)ctx;
    (void)f;
    return FAKE_LINE_H;
}

static void clip(void *ctx, const ui_rect *r)
{
    (void)ctx;
    (void)r;
}

static void image(void *ctx, ui_image img, ui_rect src, int x, int y)
{
    (void)ctx;
    (void)img;
    (void)src;
    (void)x;
    (void)y;
}

static void image_size(void *ctx, ui_image img, int *w, int *h)
{
    (void)ctx;
    (void)img;
    *w = 0;
    *h = 0;
}

static void save_backdrop(void *ctx, ui_color shade)
{
    (void)ctx;
    next_call('s')->c = shade;
}

static void backdrop(void *ctx)
{
    (void)ctx;
    (void)next_call('b');
}

static void pixels(void *ctx, const uint32_t *px, int w, int h, int x, int y)
{
    (void)ctx;
    fake_call *c = next_call('p');
    c->r = (ui_rect){x, y, w, h};
    c->c = (ui_color){(uint8_t)(px[0] >> 16U), (uint8_t)(px[0] >> 8U), (uint8_t)px[0], 255};
}

const ui_draw fake_draw = {NULL,  fill,       text,          width,    line_height, clip,
                           image, image_size, save_backdrop, backdrop, pixels};

void fake_reset(void)
{
    fake_log_n = 0;
}

int fake_find_text(const char *s)
{
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind == 't' && strcmp(fake_log[i].text, s) == 0) {
            return i;
        }
    }
    return -1;
}

int fake_saw(const char *s)
{
    for (int i = 0; i < fake_log_n; i++) {
        if (fake_log[i].kind != 'f' && strstr(fake_log[i].text, s) != NULL) {
            return 1;
        }
    }
    return 0;
}
