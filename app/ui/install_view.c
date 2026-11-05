/* The install screen (install_view.h). */
#include "install_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"

#define PAD 14

static const ui_rect tab = {32, 24, 240, 46};
static const ui_rect body = {32, 80, 576, 320};
static const ui_rect footer = {32, 408, 576, 48};

void ui_install_open(ui_install_view *v, const char *name)
{
    memset(v, 0, sizeof *v);
    v->name = name;
    v->phase = UI_INST_CONNECTING;
}

void ui_install_status(ui_install_view *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(v->status, sizeof v->status, fmt, ap);
    va_end(ap);
    if (n < 0) {
        v->status[0] = '\0';
    }
}

void ui_install_end(ui_install_view *v, bool ok, const char *message)
{
    (void)snprintf(v->message, sizeof v->message, "%s", message != NULL ? message : "");
    v->dialog = ok ? (ui_dialog){"INSTALLED", v->message, {"OK", NULL, NULL}, 0, 0}
                   : (ui_dialog){"NOT INSTALLED", v->message, {"RETRY", "BACK", NULL}, 0, 1};
    v->stopping = false;
    v->ended = true;
    v->login = false;
    v->backdrop = false;
}

void ui_install_end_back(ui_install_view *v, const char *message)
{
    ui_install_end(v, false, message);
    v->dialog = (ui_dialog){"NOT INSTALLED", v->message, {"BACK", NULL, NULL}, 0, 0};
}

void ui_install_end_login(ui_install_view *v, const char *message)
{
    ui_install_end(v, false, message);
    v->dialog.choices[0] = "LOG IN";
    v->login = true;
}

const char *ui_install_screen(const ui_install_view *v)
{
    if (v->ended) {
        return strcmp(v->dialog.title, "INSTALLED") == 0 ? "install-ok" : "install-failed";
    }
    return v->stopping ? "ask-stop" : "installing";
}

ui_inst_action ui_install_input(ui_install_view *v, ui_button b)
{
    if (v->ended) {
        int c = ui_dialog_input(&v->dialog, b);
        if (c == UI_DIALOG_OPEN) {
            return UI_INST_NONE;
        }
        /* OK alone, or RETRY (LOG IN) and BACK. */
        if (v->dialog.choices[1] == NULL || c != 0) {
            return UI_INST_DONE;
        }
        return v->login ? UI_INST_LOGIN : UI_INST_RETRY;
    }
    if (v->stopping) {
        int c = ui_dialog_input(&v->dialog, b);
        if (c == UI_DIALOG_OPEN) {
            return UI_INST_NONE;
        }
        v->stopping = false;
        return c == 0 ? UI_INST_STOP : UI_INST_NONE;
    }
    if (b == UI_BTN_B) {
        v->dialog = (ui_dialog){"STOP",
                                "Stop installing? What's written so far is kept, and the "
                                "install carries on from there when you start it again.",
                                {"STOP", "CONTINUE", NULL},
                                1,
                                1};
        v->stopping = true;
        v->backdrop = false;
    }
    return UI_INST_NONE;
}

static const char *phase_name(ui_inst_phase p)
{
    switch (p) {
    case UI_INST_CONNECTING:
        return "CONNECTING";
    case UI_INST_CHECKING:
        return "CHECKING THE IMAGE";
    case UI_INST_WRITING:
        return "WRITING FILES";
    case UI_INST_VERIFYING:
        return "VERIFYING";
    }
    return "";
}

static const char *phase_hint(const ui_install_view *v, char *buf, size_t size)
{
    switch (v->phase) {
    case UI_INST_CONNECTING:
        return "Reaching the server and opening the file.";
    case UI_INST_CHECKING:
        return "Reading the image's folders, and checking the names and the free space.";
    case UI_INST_WRITING:
        (void)snprintf(buf, size, "Writing the game's files to %s.",
                       v->dir != NULL ? v->dir : "E:\\Games");
        return buf;
    case UI_INST_VERIFYING:
        return "Reading every file back to check it.";
    }
    return "";
}

static void draw_screen(const ui_draw *d, const ui_install_view *v, int pan)
{
    const ui_rect panels[] = {tab, body, footer};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]));

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "INSTALL");

    ui_panel(d, body, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = body.x + PAD + 6;
    int w = body.w - 2 * PAD - 12;
    int y = body.y + PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int lh = d->line_height(d->ctx, UI_FONT_BODY);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent,
                      v->name != NULL ? v->name : "") *
         bh;
    y += 8;
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_text, phase_name(v->phase));
    y += d->line_height(d->ctx, UI_FONT_LABEL);
    char hint[128];
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, phase_hint(v, hint, sizeof hint));
    y += sh + 10;

    char pct[16] = "";
    if (v->total > 0) {
        (void)snprintf(pct, sizeof pct, "%d %%", ui_percent(v->done, v->total));
    }
    ui_progress(d, (ui_rect){x, y, w, 34}, v->done, v->total, v->total > 0 ? pct : NULL);
    y += 34 + 10;

    char line[96];
    if (v->total > 0) {
        char done[24];
        char total[24];
        ui_format_size(done, sizeof done, v->done);
        ui_format_size(total, sizeof total, v->total);
        (void)snprintf(line, sizeof line, "%s of %s", done, total);
        (void)ui_text_fit(d, UI_FONT_BODY, x, y, w, ui_col_text, line);
    }
    y += lh;
    if (v->files > 0) {
        (void)snprintf(line, sizeof line, "File %u of %u", (unsigned)v->file, (unsigned)v->files);
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    }
    y += sh;
    if (v->speed > 0) {
        char left[48];
        ui_format_left(left, sizeof left, v->total > v->done ? v->total - v->done : 0, v->speed);
        (void)snprintf(line, sizeof line, "%u.%u MB/s, %s", (unsigned)(v->speed / 10U),
                       (unsigned)(v->speed % 10U), left);
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    }
    y += sh + 6;
    int bottom = body.y + body.h - PAD;
    if (v->status[0] != '\0' && y < bottom) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim, v->status);
    }

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int by = footer.y + (footer.h - sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_dim,
                      "B stop (the install resumes when you start it again)");
}

void ui_install_draw(const ui_draw *d, ui_install_view *v, int pan)
{
    if (!v->stopping && !v->ended) {
        v->backdrop = false;
        draw_screen(d, v, pan);
        return;
    }
    if (v->backdrop) {
        ui_backdrop(d);
    } else {
        draw_screen(d, v, pan);
        ui_backdrop_save(d);
        v->backdrop = true;
    }
    ui_dialog_draw(d, &v->dialog);
}

void ui_format_size(char *out, size_t size, uint64_t bytes)
{
    static const char *const units[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1000U) {
        (void)snprintf(out, size, "%u bytes", (unsigned)bytes);
        return;
    }
    uint64_t unit = 1000U;
    int u = 0;
    while (u < 3 && bytes / unit >= 1000U) {
        unit *= 1000U;
        u++;
    }
    /* Three digits, rounded down: 4.38, 12.3, 740. */
    unsigned long long hundredths = bytes / (unit / 100U);
    unsigned long long whole = hundredths / 100U;
    if (whole >= 100U) {
        (void)snprintf(out, size, "%llu %s", whole, units[u]);
    } else if (whole >= 10U) {
        (void)snprintf(out, size, "%llu.%llu %s", whole, (hundredths % 100U) / 10U, units[u]);
    } else {
        (void)snprintf(out, size, "%llu.%02llu %s", whole, hundredths % 100U, units[u]);
    }
}

void ui_format_left(char *out, size_t size, uint64_t left, uint32_t speed)
{
    if (speed == 0) {
        (void)snprintf(out, size, "%s", "");
        return;
    }
    uint64_t per_s = (uint64_t)speed * 100000U; /* tenths of MB/s in bytes a second */
    uint64_t s = (left + per_s - 1U) / per_s;
    if (s < 60U) {
        (void)snprintf(out, size, "%s", "less than a minute left");
        return;
    }
    unsigned long long minutes = (s + 59U) / 60U;
    if (minutes < 60U) {
        (void)snprintf(out, size, "about %llu minute%s left", minutes, minutes == 1U ? "" : "s");
        return;
    }
    unsigned long long hours = minutes / 60U;
    minutes %= 60U;
    if (minutes == 0U) {
        (void)snprintf(out, size, "about %llu hour%s left", hours, hours == 1U ? "" : "s");
        return;
    }
    (void)snprintf(out, size, "about %llu hour%s %llu minute%s left", hours, hours == 1U ? "" : "s",
                   minutes, minutes == 1U ? "" : "s");
}
