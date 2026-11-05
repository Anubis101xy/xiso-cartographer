#ifndef XCAT_UI_FAKE_DRAW_H
#define XCAT_UI_FAKE_DRAW_H

/*
 * A fake drawing backend for the widget tests: every code point is FAKE_CP_W pixels wide, a
 * line FAKE_LINE_H high, and every call is logged, with the string the backend is given (to
 * draw or to measure), so tests can check what was drawn and what never reached the backend.
 * A test fails if its calls overflow the log.
 */

#include <stddef.h>

#include "ui.h"

#define FAKE_CP_W 10
#define FAKE_LINE_H 20
#define FAKE_LOG 4096

typedef struct fake_call {
    char kind; /* 't' text drawn, 'w' width measured, 'f' fill, 's' save_backdrop, 'b' backdrop,
                  'p' pixels (r: where, c: the first pixel's colour) */
    ui_font font;
    ui_rect r; /* x, y of text; the rectangle of a fill */
    ui_color c;
    char text[128];
} fake_call;

extern fake_call fake_log[FAKE_LOG];
extern int fake_log_n;
extern const ui_draw fake_draw;

/* Empties the log. */
void fake_reset(void);

/* The index of the first text drawn that equals s, or -1. */
int fake_find_text(const char *s);

/* Whether s appears inside any string the backend was given, drawn or measured. */
int fake_saw(const char *s);

#endif /* XCAT_UI_FAKE_DRAW_H */
