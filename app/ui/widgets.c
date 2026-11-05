/* The list, progress bar and dialog (widgets.h). Portable: no SDL or nxdk header. */
#include "widgets.h"

#include <string.h>

#include "theme.h"

#define ROW_GAP 6   /* between list lines */
#define TEXT_IN 10  /* text from the left of its row */
#define BUTTON_IN 6 /* a dialog button's text from its sides */
#define BAR_W 4     /* the list's scroll bar */
#define DIALOG_W 520
#define DIALOG_PAD 18
#define DIALOG_LINES 6 /* at most, of the message */

/* w * done / total for done <= total, without overflowing 64 bits. */
static int part(uint64_t done, uint64_t total, int w)
{
    if (total == 0 || w <= 0) {
        return 0;
    }
    if (done >= total) {
        return w;
    }
    while (total > UINT32_MAX) {
        total >>= 1;
        done >>= 1;
    }
    return (int)(done * (uint64_t)w / total);
}

/* ---- List */

int ui_list_row_h(const ui_draw *d)
{
    return d->line_height(d->ctx, UI_FONT_BODY) + ROW_GAP;
}

int ui_list_rows(const ui_draw *d, ui_rect r)
{
    int rows = r.h / ui_list_row_h(d);
    return rows > 0 ? rows : 1;
}

static void keep_shown(ui_list *l, int rows)
{
    if (rows < 1) {
        rows = 1;
    }
    if (l->cur < l->top) {
        l->top = l->cur;
    } else if (l->cur >= l->top + rows) {
        l->top = l->cur - rows + 1;
    }
    int max_top = l->count > rows ? l->count - rows : 0;
    if (l->top > max_top) {
        l->top = max_top;
    }
    if (l->top < 0) {
        l->top = 0;
    }
}

void ui_list_set_count(ui_list *l, int count, int rows)
{
    l->count = count > 0 ? count : 0;
    if (l->cur >= l->count) {
        l->cur = l->count - 1;
    }
    if (l->cur < 0) {
        l->cur = 0;
    }
    keep_shown(l, rows);
}

bool ui_list_input(ui_list *l, ui_button b, int rows)
{
    if (l->count <= 0) {
        return false;
    }
    if (rows < 1) {
        rows = 1;
    }
    int to = l->cur;
    if (b == UI_BTN_UP) {
        to--;
    } else if (b == UI_BTN_DOWN) {
        to++;
    } else if (b == UI_BTN_LT) {
        to -= rows;
    } else if (b == UI_BTN_RT) {
        to += rows;
    }
    if (to < 0) {
        to = 0;
    } else if (to >= l->count) {
        to = l->count - 1;
    }
    bool moved = to != l->cur;
    l->cur = to;
    keep_shown(l, rows);
    return moved;
}

/* The width of a line's text in a list drawn in r: the row less the scroll bar, if any. */
static int line_w(const ui_draw *d, const ui_list *l, ui_rect r)
{
    bool bar = l->count > ui_list_rows(d, r);
    return (bar ? r.w - BAR_W - 4 : r.w) - 2 * TEXT_IN;
}

int ui_list_overflow(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx)
{
    if (l->count <= 0 || l->cur < 0 || l->cur >= l->count) {
        return 0;
    }
    const char *s = line(ctx, l->cur);
    int over = d->text_width(d->ctx, UI_FONT_BOLD, s, strlen(s)) - line_w(d, l, r);
    return over > 0 ? over : 0;
}

int ui_slide_offset(uint32_t ms, int overflow)
{
    if (overflow <= 0) {
        return 0;
    }
    uint32_t travel = (uint32_t)overflow * UI_SLIDE_STEP_MS;
    uint32_t t = ms % (UI_SLIDE_WAIT_MS + travel + UI_SLIDE_HOLD_MS);
    if (t < UI_SLIDE_WAIT_MS) {
        return 0;
    }
    t -= UI_SLIDE_WAIT_MS;
    return t < travel ? (int)(t / UI_SLIDE_STEP_MS) + 1 : overflow;
}

void ui_list_draw(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx,
                  bool idle)
{
    ui_list_draw_slid(d, l, r, line, ctx, idle, 0);
}

void ui_list_draw_slid(const ui_draw *d, const ui_list *l, ui_rect r, ui_list_line *line, void *ctx,
                       bool idle, int slide)
{
    int row_h = ui_list_row_h(d);
    int rows = ui_list_rows(d, r);
    bool bar = l->count > rows;
    int w = bar ? r.w - BAR_W - 4 : r.w;
    d->clip(d->ctx, &r);
    for (int i = 0; i < rows && l->top + i < l->count; i++) {
        int n = l->top + i;
        ui_rect row = {r.x, r.y + i * row_h, w, row_h - 2};
        bool sel = n == l->cur;
        if (sel) {
            ui_panel(d, row, 6, idle ? ui_col_idle : ui_col_highlight,
                     idle ? ui_col_idle_border : ui_col_highlight_border);
        }
        ui_font f = sel ? UI_FONT_BOLD : UI_FONT_BODY;
        ui_color c = sel && idle ? ui_col_dim : ui_col_text;
        const char *s = line(ctx, n);
        if (sel && slide > 0) {
            /* The whole line, moved left and cut at the row's text edges. */
            ui_rect in = {row.x + TEXT_IN, row.y, row.w - 2 * TEXT_IN, row.h};
            d->clip(d->ctx, &in);
            (void)d->text(d->ctx, f, in.x - slide, row.y + 2, c, s, strlen(s));
            d->clip(d->ctx, &r);
        } else {
            (void)ui_text_fit(d, f, row.x + TEXT_IN, row.y + 2, row.w - 2 * TEXT_IN, c, s);
        }
    }
    if (bar) {
        ui_rect track = {r.x + r.w - BAR_W, r.y, BAR_W, r.h};
        int thumb_h = part((uint64_t)rows, (uint64_t)l->count, r.h);
        if (thumb_h < 12) {
            thumb_h = 12;
        }
        int y = part((uint64_t)l->top, (uint64_t)(l->count - rows), r.h - thumb_h);
        d->fill(d->ctx, track, ui_col_highlight);
        d->fill(d->ctx, (ui_rect){track.x, r.y + y, BAR_W, thumb_h}, ui_col_highlight_border);
    }
    d->clip(d->ctx, NULL);
}

/* ---- Progress bar */

int ui_percent(uint64_t done, uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    int p = part(done, total, 100);
    return p == 100 && done < total ? 99 : p;
}

void ui_progress(const ui_draw *d, ui_rect r, uint64_t done, uint64_t total, const char *label)
{
    ui_panel(d, r, 6, ui_col_panel, ui_col_border);
    int in = UI_BORDER + 2;
    ui_rect bar = {r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in};
    int w = part(done, total, bar.w);
    if (w > 0) {
        d->fill(d->ctx, (ui_rect){bar.x, bar.y, w, bar.h}, ui_col_highlight);
    }
    if (label != NULL) {
        int max_w = r.w - 2 * TEXT_IN;
        int tw = d->text_width(d->ctx, UI_FONT_BOLD, label, strlen(label));
        int x = r.x + (r.w - (tw < max_w ? tw : max_w)) / 2;
        int y = r.y + (r.h - d->line_height(d->ctx, UI_FONT_BOLD)) / 2;
        (void)ui_text_fit(d, UI_FONT_BOLD, x, y, max_w, ui_col_text, label);
    }
}

/* ---- Dialog */

static int dialog_choices(const ui_dialog *dg)
{
    int n = 0;
    while (n < UI_DIALOG_CHOICES && dg->choices[n] != NULL) {
        n++;
    }
    return n;
}

int ui_dialog_input(ui_dialog *dg, ui_button b)
{
    int n = dialog_choices(dg);
    if (b == UI_BTN_LEFT && dg->cur > 0) {
        dg->cur--;
    } else if (b == UI_BTN_RIGHT && dg->cur + 1 < n) {
        dg->cur++;
    } else if (b == UI_BTN_A && n > 0) {
        return dg->cur;
    } else if (b == UI_BTN_B) {
        return dg->cancel;
    }
    return UI_DIALOG_OPEN;
}

/* How many lines s takes wrapped at w, up to max. */
static int wrapped_lines(const ui_draw *d, ui_font f, const char *s, int w, int max)
{
    size_t n = strlen(s);
    size_t i = 0;
    int lines = 0;
    while (i < n && lines < max) {
        size_t next = 0;
        (void)ui_wrap(d, f, s + i, n - i, w, &next);
        i += next;
        lines++;
    }
    return lines;
}

void ui_dialog_draw(const ui_draw *d, const ui_dialog *dg)
{
    int inner_w = DIALOG_W - 2 * DIALOG_PAD;
    int title_h = d->line_height(d->ctx, UI_FONT_LABEL);
    int lh = d->line_height(d->ctx, UI_FONT_BODY);
    int lines = dg->message != NULL
                    ? wrapped_lines(d, UI_FONT_BODY, dg->message, inner_w, DIALOG_LINES)
                    : 0;
    int button_h = d->line_height(d->ctx, UI_FONT_LABEL) + 12;
    int h = DIALOG_PAD + title_h + 10 + lines * lh + 16 + button_h + DIALOG_PAD;
    ui_rect box = {(UI_SCREEN_W - DIALOG_W) / 2, (UI_SCREEN_H - h) / 2, DIALOG_W, h};
    ui_panel(d, box, UI_CHAMFER, ui_col_panel, ui_col_border);

    int x = box.x + DIALOG_PAD;
    int y = box.y + DIALOG_PAD;
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, inner_w, ui_col_accent, dg->title);
    y += title_h + 10;
    if (lines > 0) {
        (void)ui_text_wrap(d, UI_FONT_BODY, (ui_rect){x, y, inner_w, lines * lh}, ui_col_text,
                           dg->message);
        y += lines * lh;
    }
    y += 16;

    int n = dialog_choices(dg);
    if (n == 0) {
        return;
    }
    int gap = 10;
    int bw = (inner_w - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) {
        ui_rect b = {x + i * (bw + gap), y, bw, button_h};
        bool sel = i == dg->cur;
        ui_panel(d, b, 6, sel ? ui_col_highlight : ui_col_panel,
                 sel ? ui_col_highlight_border : ui_col_border);
        int max_w = bw - 2 * BUTTON_IN;
        int tw = d->text_width(d->ctx, UI_FONT_LABEL, dg->choices[i], strlen(dg->choices[i]));
        int tx = b.x + (bw - (tw < max_w ? tw : max_w)) / 2;
        (void)ui_text_fit(d, UI_FONT_LABEL, tx, b.y + 6, max_w, sel ? ui_col_text : ui_col_dim,
                          dg->choices[i]);
    }
}
