#ifndef XCAT_UI_WIDGETS_H
#define XCAT_UI_WIDGETS_H

/*
 * The widgets of ADR 0015 decision 2, in the look of docs/ui-style.md: a list that scrolls
 * and pages, a progress bar and a dialog with up to three choices. (Wrapped text is
 * ui_text_wrap, ui.h; the on-screen keyboard is keyboard.h.) Each widget's state is a plain
 * struct the screen owns; *_input changes it for one button press and *_draw draws it, so
 * they're tested on the PC.
 */

#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- List: one line per item, the highlighted one in a small panel. */

typedef struct ui_list {
    int count; /* lines */
    int cur;   /* the highlighted line, 0 if count is 0 */
    int top;   /* the first line shown */
} ui_list;

/* The height of a line, and how many lines fit in r (at least 1). */
int ui_list_row_h(const ui_draw *d);
int ui_list_rows(const ui_draw *d, ui_rect r);

/* Sets the number of lines (after a search, say), keeping cur and top in range. */
void ui_list_set_count(ui_list *l, int count, int rows);

/* Up and Down move one line, LT and RT a page of `rows` lines; the highlight stops at the
   ends, and the list scrolls to keep it shown. Returns whether cur changed. Other buttons
   are the screen's. */
bool ui_list_input(ui_list *l, ui_button b, int rows);

/* The text of line i (0 <= i < count), any UTF-8; `ctx` is ui_list_draw's. */
typedef const char *ui_list_line(void *ctx, int i);

/* Draws the lines shown inside r (the inside of a panel), cut with an ellipsis if they're too
   wide, and a scroll bar at r's right edge when not all lines fit. The highlighted line is in
   bold in a small panel: bright, or dim (ui_col_idle, ui_col_idle_border, text in ui_col_dim)
   when `idle`, while the buttons move something else. */
void ui_list_draw(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx,
                  bool idle);

/* How many pixels the highlighted line is wider than its row as ui_list_draw draws it in r
   (so how far it can slide); 0 if it fits, or if there are no lines. */
int ui_list_overflow(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx);

/* ui_list_draw with the highlighted line moved `slide` pixels to the left, whole and cut at its
   row's edges, rather than ended with an ellipsis; slide 0 is ui_list_draw. */
void ui_list_draw_slid(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx,
                       bool idle, int slide);

/* How far a line `overflow` pixels too wide has slid `ms` after it was highlighted: still for
   UI_SLIDE_WAIT_MS, then one pixel every UI_SLIDE_STEP_MS to its end, still there for
   UI_SLIDE_HOLD_MS, then back to the start, and again. 0 if overflow is 0 or less. */
#define UI_SLIDE_WAIT_MS 1200U
#define UI_SLIDE_STEP_MS 30U
#define UI_SLIDE_HOLD_MS 1500U
int ui_slide_offset(uint32_t ms, int overflow);

/* ---- Progress bar. */

/* done / total in whole percent, rounded down: 100 only once done reaches total. 0 if total
   is 0. */
int ui_percent(uint64_t done, uint64_t total);

/* A bar in r filled done / total of the way (empty if total is 0), with `label` (may be NULL)
   centred on it. */
void ui_progress(const ui_draw *d, ui_rect r, uint64_t done, uint64_t total, const char *label);

/* ---- Dialog: a title, a message and one to three choices. */

#define UI_DIALOG_CHOICES 3
#define UI_DIALOG_OPEN (-1)

typedef struct ui_dialog {
    const char *title;                      /* the app's own words, in capitals */
    const char *message;                    /* any UTF-8, wrapped; may be NULL */
    const char *choices[UI_DIALOG_CHOICES]; /* the app's own words; NULL after the last */
    int cur;                                /* the highlighted choice */
    int cancel;                             /* the choice B gives, or UI_DIALOG_OPEN: none */
} ui_dialog;

/* Left and Right move between the choices; A returns the highlighted one, B returns `cancel`.
   Anything else, or B without a cancel, returns UI_DIALOG_OPEN. */
int ui_dialog_input(ui_dialog *dg, ui_button b);

/* Draws the dialog in a panel centred on the screen. Draw it over ui_backdrop (theme.h): the
   screen behind it, darkened and standing still. */
void ui_dialog_draw(const ui_draw *d, const ui_dialog *dg);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_WIDGETS_H */
