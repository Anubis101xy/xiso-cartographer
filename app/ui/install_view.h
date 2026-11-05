#ifndef XCAT_UI_INSTALL_VIEW_H
#define XCAT_UI_INSTALL_VIEW_H

/*
 * The install screen (M8, docs/ui-style.md#install): the game being installed, what the
 * install is doing, a progress bar, how much is done, the speed and the time left. B asks
 * before stopping; a stopped install resumes when it's started again. When the install ends,
 * a dialog says how it went. Plain C over ui_draw, tested on the PC (test_ui_install).
 */

#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "ui.h"
#include "widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_inst_phase {
    UI_INST_CONNECTING, /* before the install: the network, the server, the file */
    UI_INST_CHECKING,   /* reading the image's directory tables, checking space and names */
    UI_INST_WRITING,
    UI_INST_VERIFYING, /* reading every file back */
} ui_inst_phase;

typedef enum ui_inst_action {
    UI_INST_NONE,  /* carry on */
    UI_INST_STOP,  /* the user said to stop the install */
    UI_INST_RETRY, /* after a failure: try again */
    UI_INST_DONE,  /* after the end: back to the library */
    UI_INST_LOGIN, /* after a refused login: log in again, then carry on (ADR 0010) */
} ui_inst_action;

typedef struct ui_install_view {
    const char *name; /* the game: any UTF-8, the caller's */
    const char *dir;  /* the install folder it goes to, the caller's; NULL: E:\Games */
    ui_inst_phase phase;
    uint64_t done, total; /* bytes of the phase's whole; total 0: not known yet */
    uint32_t file, files; /* the file being worked on (1-based) of how many; files 0: unknown */
    uint32_t speed;       /* in tenths of MB/s; 0: not known yet */
    char status[160];     /* under the details: retries, the server's steps; may be "" */
    bool stopping;        /* the "stop?" dialog is open */
    bool ended;           /* the result dialog is open */
    bool login;           /* ... and offers LOG IN instead of RETRY */
    bool backdrop;        /* the screen behind the dialog is saved (ui_backdrop_save) */
    ui_dialog dialog;
    char message[320]; /* the dialog's */
} ui_install_view;

/* Starts the screen for `name`, connecting. */
void ui_install_open(ui_install_view *v, const char *name);

/* Sets the status line (printf-style); "" clears it. */
void ui_install_status(ui_install_view *v, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Ends the install: a dialog titled INSTALLED or NOT INSTALLED with `message` (copied), and
   OK, or RETRY and BACK. */
void ui_install_end(ui_install_view *v, bool ok, const char *message);

/* Ends a failed install that trying again can't help (xc_err_retry): NOT INSTALLED with
   `message` (copied), and BACK alone. */
void ui_install_end_back(ui_install_view *v, const char *message);

/* Ends a failed install whose server refused the login: NOT INSTALLED with `message` (copied),
   and LOG IN and BACK. LOG IN returns UI_INST_LOGIN. */
void ui_install_end_login(ui_install_view *v, const char *message);

/* One button press: while installing, B opens the "stop?" dialog, which returns UI_INST_STOP
   if the user confirms; after the end, the result dialog's choice. */
ui_inst_action ui_install_input(ui_install_view *v, ui_button b);

/* The name of what the screen shows, for the UI script (docs/formats/ui-script.md §6):
   "installing", "ask-stop", "install-ok" or "install-failed". */
const char *ui_install_screen(const ui_install_view *v);

/* Draws the screen with the background at pan; over an open dialog, the screen behind stands
   still (ui_backdrop). */
void ui_install_draw(const ui_draw *d, ui_install_view *v, int pan);

/* Sizes for people, decimal like the console's dashboard: "740 KB", "12.3 MB", "4.38 GB",
   "1.82 TB". */
void ui_format_size(char *out, size_t size, uint64_t bytes);

/* The time left at `speed` tenths of MB/s for `left` bytes: "about 12 minutes left", "about 2
   hours 5 minutes left", "less than a minute left"; "" if the speed is 0. */
void ui_format_left(char *out, size_t size, uint64_t left, uint32_t speed);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_INSTALL_VIEW_H */
