/* Plays a UI script (script.h). Portable: no SDL or nxdk header. */
#include "script.h"

#include <stdio.h>
#include <string.h>

/* docs/formats/ui-script.md §6: every name ui_library_screen and ui_install_screen give. */
static const char *const screens[] = {
    "games",
    "installed",
    "settings",
    "folders",
    "logins",
    "sources",
    "keyboard-search",
    "keyboard-email",
    "keyboard-password",
    "keyboard-folder",
    "keyboard-source",
    "keyboard-title-id",
    "ask-install",
    "ask-exit",
    "ask-logout",
    "ask-forget",
    "ask-remove",
    "ask-kind",
    "message",
    "installing",
    "ask-stop",
    "install-ok",
    "install-failed",
    "ask-play",
    "nas",
    "nas-browse",
    "keyboard-nas-address",
    "keyboard-nas-user",
    "keyboard-nas-password",
    "ask-nas-use",
    "ask-delete",
};

bool ui_script_screen_known(const char *name)
{
    for (size_t i = 0; i < sizeof screens / sizeof screens[0]; i++) {
        if (strcmp(name, screens[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *ui_script_check(const xc_ui_script *s, char *msg, size_t size)
{
    for (size_t i = 0; i < s->count; i++) {
        const xc_ui_step *st = &s->steps[i];
        if (st->op == XC_UI_EXPECT && !ui_script_screen_known(st->text)) {
            (void)snprintf(msg, size, "line %lu: the app has no screen \"%s\"",
                           (unsigned long)st->line, st->text);
            return msg;
        }
    }
    return NULL;
}

ui_button ui_script_button(xc_ui_key key)
{
    static const ui_button map[XC_UI_KEY_COUNT_] = {
        UI_BTN_UP, UI_BTN_DOWN,  UI_BTN_LEFT, UI_BTN_RIGHT, UI_BTN_A,  UI_BTN_B,     UI_BTN_X,
        UI_BTN_Y,  UI_BTN_START, UI_BTN_BACK, UI_BTN_LT,    UI_BTN_RT, UI_BTN_WHITE, UI_BTN_BLACK,
    };
    return (unsigned)key < XC_UI_KEY_COUNT_ ? map[key] : UI_BTN_NONE;
}

static void where(ui_script *r)
{
    (void)snprintf(r->status, sizeof r->status, "Script: line %lu, command %lu of %lu",
                   (unsigned long)r->s->steps[r->next].line, (unsigned long)(r->next + 1U),
                   (unsigned long)r->s->count);
}

void ui_script_start(ui_script *r, const xc_ui_script *s)
{
    memset(r, 0, sizeof *r);
    r->s = s;
    r->state = UI_SCRIPT_RUNNING;
    if (s->count > 0) {
        where(r);
    }
}

/* now >= at, across the wrap at 2^32 ms. */
static bool due(uint32_t now, uint32_t at)
{
    return now - at < 0x80000000U;
}

/* On to the next command, or the end. */
static void step_done(ui_script *r)
{
    r->next++;
    r->started = false;
    if (r->next < r->s->count) {
        where(r);
    }
}

ui_button ui_script_next(ui_script *r, uint32_t now_ms, const char *screen, char *log,
                         size_t log_size)
{
    log[0] = '\0';
    if (r->state != UI_SCRIPT_RUNNING) {
        return UI_BTN_NONE;
    }
    if (r->next >= r->s->count) {
        r->state = UI_SCRIPT_PASSED;
        (void)snprintf(r->status, sizeof r->status,
                       "Script passed: %lu commands, %lu screens checked",
                       (unsigned long)r->s->count, (unsigned long)r->checks);
        (void)snprintf(log, log_size, "%s", r->status);
        return UI_BTN_NONE;
    }
    const xc_ui_step *st = &r->s->steps[r->next];
    switch (st->op) {
    case XC_UI_NOTE:
        (void)snprintf(log, log_size, "Script: %s", st->text);
        step_done(r);
        return UI_BTN_NONE;
    case XC_UI_WAIT:
        if (!r->started) {
            r->started = true;
            r->at_ms = now_ms + st->ms;
        }
        if (due(now_ms, r->at_ms)) {
            step_done(r);
        }
        return UI_BTN_NONE;
    case XC_UI_EXPECT:
        if (!r->started) {
            r->started = true;
            r->at_ms = now_ms + st->ms;
        }
        if (strcmp(screen, st->text) == 0) {
            r->checks++;
            (void)snprintf(log, log_size, "Script: line %lu: on %s", (unsigned long)st->line,
                           st->text);
            step_done(r);
        } else if (due(now_ms, r->at_ms)) {
            r->state = UI_SCRIPT_FAILED;
            (void)snprintf(r->status, sizeof r->status,
                           "Script FAILED at line %lu: expected %s, on %s after %lu ms",
                           (unsigned long)st->line, st->text, screen, (unsigned long)st->ms);
            (void)snprintf(log, log_size, "%s", r->status);
        }
        return UI_BTN_NONE;
    case XC_UI_PRESS:
        if (!r->started) {
            r->started = true;
            r->left = st->count;
            r->at_ms = now_ms + UI_SCRIPT_GAP_MS;
        }
        if (!due(now_ms, r->at_ms)) {
            return UI_BTN_NONE;
        }
        r->at_ms = now_ms + UI_SCRIPT_GAP_MS;
        if (--r->left == 0) {
            step_done(r);
        }
        return ui_script_button(st->key);
    }
    return UI_BTN_NONE;
}
