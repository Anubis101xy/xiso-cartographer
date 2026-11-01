#ifndef XCAT_UI_LIBRARY_H
#define XCAT_UI_LIBRARY_H

/*
 * The library screen (M8, docs/ui-style.md#library): the sections on the left, the games in a
 * list, the highlighted game's details beside it, and a footer. A asks before installing the
 * highlighted game (the map is a catalog: nothing installs unasked), and Back asks before
 * leaving for the dashboard. GAMES lists what can be installed, INSTALLED what's in the install
 * folder, and SETTINGS where games install (docs/formats/settings.md), Authentication (the
 * servers the map uses and their logins, with the archive.org account: log in and out, ADR
 * 0010), Map sources (the map's own lines, added and removed: map.md §9) and NAS (finding a
 * NAS on the network, logging in and choosing a shared folder: smb.md, ADR 0018). A on an
 * installed game asks to play it. Y searches GAMES
 * with the on-screen
 * keyboard (keyboard.h), which also takes the email and password. Plain C over ui_draw like the
 * widgets, tested on the PC (test_ui_library).
 */

#include <stdbool.h>

#include "input.h"
#include "keyboard.h"
#include "ui.h"
#include "widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_lib_state {
    UI_LIB_NOT_TRIED,  /* not installed, and nothing started */
    UI_LIB_INSTALLED,  /* installed and verified, in this run or an earlier one */
    UI_LIB_FAILED,     /* tried in this run, and it failed */
    UI_LIB_STOPPED,    /* tried in this run, and the user stopped it */
    UI_LIB_UNFINISHED, /* started in an earlier run and not finished: installing carries on */
} ui_lib_state;

/* One game, as the caller describes it. The strings are the caller's and must stay alive. */
typedef struct ui_lib_item {
    const char *name;  /* the list's line and the details' heading: any UTF-8 */
    const char *facts; /* under the heading, small: year, region, size; may be "" */
    const char *about; /* wrapped below: who made it, the release, where it comes from */
    ui_lib_state state;
    const char *note; /* what happened, or where it's installed; may be NULL */
} ui_lib_item;

typedef enum ui_lib_section {
    UI_LIB_GAMES,
    UI_LIB_INSTALLED_GAMES,
    UI_LIB_SETTINGS,
    UI_LIB_SECTION_COUNT_
} ui_lib_section;

typedef enum ui_lib_action {
    UI_LIB_NONE,          /* redraw if the screen says it changed */
    UI_LIB_INSTALL,       /* install items[ui_library_game()]: the user said yes */
    UI_LIB_EXIT,          /* back to the dashboard: the user said yes */
    UI_LIB_LOGIN,         /* log in with email and password, then ui_library_forget_password */
    UI_LIB_LOGOUT,        /* log out of archive.org: the user said yes */
    UI_LIB_TELL,          /* (internal: a message with OK is open) */
    UI_LIB_FOLDER,        /* install to lib->folder from now on: the user chose it */
    UI_LIB_FORGET,        /* remove the login of hosts[ui_library_host()]: the user said yes */
    UI_LIB_ADD_SOURCE,    /* add a source of kind add_kind from add_url (and add_key) */
    UI_LIB_REMOVE_SOURCE, /* remove sources[ui_library_source()]: the user said yes */
    UI_LIB_SOURCE_KIND,   /* (internal: the dialog asking what kind of source to add) */
    UI_LIB_PLAY,          /* start the highlighted installed game: in GAMES ui_library_game(),
                             in INSTALLED ui_library_installed() (lib->section says which) */
    UI_LIB_NAS_PAGE,      /* the NAS page opened: give it its NASes (ui_library_set_nas) and
                             say where the console is (ui_library_set_console) */
    UI_LIB_NAS_SCAN,      /* scan the network for a NAS, then ui_library_set_nas */
    UI_LIB_NAS_LOGIN,     /* log in to lib->nas_addr as lib->nas_user ("": a guest) with
                             lib->nas_password and list its shares (ui_library_browse); then
                             ui_library_forget_nas_password */
    UI_LIB_NAS_SAVED,     /* log in to lib->nas_addr with its saved login and list its shares */
    UI_LIB_NAS_OPEN,      /* open browse entry ui_library_browse_entry() */
    UI_LIB_NAS_UP,        /* B while browsing: one level up (the shares, then the NAS page) */
    UI_LIB_NAS_USE,       /* add a NAS folder to the map, the user said yes: the one browsed
                             (ui_library_nas_pick() -1), or browse entry ui_library_nas_pick() */
} ui_lib_action;

/* What kind of source to add to the map. */
typedef enum ui_lib_source_kind {
    UI_LIB_SOURCE_ARCHIVE, /* @archiveorg <identifier> */
    UI_LIB_SOURCE_MAP,     /* @include <url> */
    UI_LIB_SOURCE_IMAGE,   /* <title ID> = <url> */
} ui_lib_source_kind;

typedef enum ui_lib_typing {
    UI_LIB_TYPING_NONE, /* the keyboard is closed */
    UI_LIB_TYPING_SEARCH,
    UI_LIB_TYPING_EMAIL,
    UI_LIB_TYPING_PASSWORD,
    UI_LIB_TYPING_FOLDER,
    UI_LIB_TYPING_SOURCE,   /* the identifier or URL of a new source */
    UI_LIB_TYPING_TITLE_ID, /* a new image's title ID */
    UI_LIB_TYPING_NAS_ADDRESS,
    UI_LIB_TYPING_NAS_USER,
    UI_LIB_TYPING_NAS_PASSWORD,
} ui_lib_typing;

/* SETTINGS' lines. */
typedef enum ui_lib_setting {
    UI_LIB_SETTING_FOLDER,  /* where games install */
    UI_LIB_SETTING_LOGINS,  /* Authentication: the servers and their logins */
    UI_LIB_SETTING_SOURCES, /* Map sources: the map's own lines */
    UI_LIB_SETTING_NAS,     /* NAS: find one, log in, choose a shared folder */
    UI_LIB_SETTING_COUNT_
} ui_lib_setting;

/* What SETTINGS shows in its list. */
typedef enum ui_lib_page {
    UI_LIB_PAGE_NONE,    /* its lines */
    UI_LIB_PAGE_FOLDERS, /* the folders to install to, then "Other folder" */
    UI_LIB_PAGE_LOGINS,  /* Authentication's servers */
    UI_LIB_PAGE_SOURCES, /* "Add source", then the map's sources */
    UI_LIB_PAGE_NAS,     /* the NASes (saved, then found), "Find a NAS", "Type a NAS address" */
    UI_LIB_PAGE_BROWSE,  /* a NAS's shares, or a folder's folders after "Add this folder" */
} ui_lib_page;

/* A server on the Authentication page, as the caller describes it. */
typedef struct ui_lib_host {
    const char *name;  /* "archive.org", "192.168.1.20:8080" */
    bool archive;      /* archive.org: A logs in or out (logged_in says which) */
    const char *login; /* else the credentials section that logs in to it, or NULL */
    const char *note;  /* under it, small: what the map gets from it; may be NULL */
    const char *about; /* wrapped below it; may be NULL */
} ui_lib_host;

/* A line of the map on the Map sources page, as the caller describes it. */
typedef struct ui_lib_source {
    const char *name;  /* the list's line: "archive.org: my-item", "Map: http://..." */
    const char *note;  /* under it, small: what kind it is, its line; may be NULL */
    const char *about; /* wrapped below it: the whole line, and any problem; may be NULL */
} ui_lib_source;

/* A NAS on the NAS page, saved or found by a scan, as the caller describes it. */
typedef struct ui_lib_nas {
    const char *name;    /* the list's line: its NetBIOS name, else its address */
    const char *address; /* what logging in connects to: "192.168.1.50" or "nas:4450" */
    const char *note;    /* under it, small; may be NULL */
    const char *login;   /* the saved login's user name ("a guest" for one), or NULL: none */
} ui_lib_nas;

/* A share or folder while browsing a NAS, as the caller describes it. */
typedef struct ui_lib_entry {
    const char *name;
    const char *note; /* may be NULL */
} ui_lib_entry;

/* A folder SETTINGS offers to install to, as the caller describes it. */
typedef struct ui_lib_folder {
    const char *path; /* "F:\Games" */
    const char *note; /* under it, small: how much is free; may be NULL */
} ui_lib_folder;

#define UI_LIB_QUERY_MAX 64     /* a search's size, with the NUL */
#define UI_LIB_EMAIL_MAX 255    /* with the NUL: xcat/login.h's 254 bytes */
#define UI_LIB_PASSWORD_MAX 129 /* with the NUL: xcat/login.h's 128 bytes */
#define UI_LIB_FOLDER_MAX 67    /* with the NUL: xcat/settings.h's XC_SETTINGS_DIR_SIZE */
#define UI_LIB_URL_MAX 512      /* a new source's identifier or URL, with the NUL */
#define UI_LIB_KEY_MAX 32       /* a new image's title ID, with the NUL */
#define UI_LIB_NAS_ADDR_MAX 64  /* a NAS's address with its port, with the NUL */
#define UI_LIB_NAS_USER_MAX 129 /* with the NUL: xcat/smb.h's 128 bytes */

typedef struct ui_library {
    const ui_lib_item *items; /* GAMES: every game */
    int count;
    int *shown;   /* the caller's, room for count: the items GAMES lists, in order */
    ui_list list; /* over shown */
    char query[UI_LIB_QUERY_MAX];  /* what GAMES is searched for; "" lists every game */
    char typing[UI_LIB_QUERY_MAX]; /* the search keyboard's, while it's open */
    ui_lib_typing typing_for;      /* what the keyboard is open for */
    ui_kbd kbd;
    bool logged_in;                     /* SETTINGS: the credentials file has [archive] */
    char email[UI_LIB_EMAIL_MAX];       /* kept for the next login */
    char password[UI_LIB_PASSWORD_MAX]; /* wiped when the login is done or cancelled */
    char tell_title[32];                /* an open message's */
    const char *install_dir;            /* where games install: the caller's */
    const ui_lib_folder *folders;       /* the folders SETTINGS offers: the caller's */
    int folder_count;
    ui_list settings_list;    /* SETTINGS' lines (ui_lib_setting) */
    ui_lib_page page;         /* what SETTINGS lists */
    ui_list folder_list;      /* UI_LIB_PAGE_FOLDERS: over folders, then "Other folder" */
    const ui_lib_host *hosts; /* UI_LIB_PAGE_LOGINS: the caller's */
    int host_count;
    ui_list host_list;
    const ui_lib_source *sources; /* UI_LIB_PAGE_SOURCES: the caller's */
    int source_count;
    ui_list source_list;         /* over "Add source", then sources */
    ui_lib_source_kind add_kind; /* UI_LIB_ADD_SOURCE's */
    char add_url[UI_LIB_URL_MAX];
    char add_key[UI_LIB_KEY_MAX];
    char folder[UI_LIB_FOLDER_MAX]; /* UI_LIB_FOLDER's, and the keyboard's for "Other folder" */
    const ui_lib_nas *nas;          /* UI_LIB_PAGE_NAS: saved and found NASes, the caller's */
    int nas_count;
    const char *nas_status;   /* how the last scan went, the caller's; may be NULL */
    const char *console_line; /* "This Xbox: 192.168.1.23", the caller's; may be NULL */
    ui_list nas_list;         /* over nas, then "Find a NAS", "Type a NAS address" */
    char nas_addr[UI_LIB_NAS_ADDR_MAX];
    char nas_user[UI_LIB_NAS_USER_MAX];
    char nas_password[UI_LIB_PASSWORD_MAX]; /* wiped when the login is done or cancelled */
    const ui_lib_entry *browse;             /* UI_LIB_PAGE_BROWSE: the caller's */
    int browse_count;
    const char *browse_use;   /* the line that adds the folder browsed, first; NULL: none */
    const char *browse_title; /* where we are, the caller's */
    const char *browse_about; /* what's here, the caller's; may be NULL */
    ui_list browse_list;
    int nas_pick; /* UI_LIB_NAS_USE's folder: -1 the one browsed, else a browse entry */
    const ui_lib_item *installed; /* INSTALLED: the folders in install_dir */
    int installed_count;
    ui_list installed_list;
    ui_lib_section section;
    bool in_sections;     /* the D-pad moves between the sections, not the games */
    ui_lib_action asking; /* what the open dialog asks; UI_LIB_NONE: none is open */
    bool backdrop;        /* the screen behind the dialog or keyboard is saved */
    ui_dialog dialog;
    char message[256]; /* the dialog's */
} ui_library;

/* Starts the screen on the first game of GAMES, listing every game, with nothing in INSTALLED.
   `shown` has room for `count` ints and stays the library's while it's open. */
void ui_library_open(ui_library *lib, const ui_lib_item *items, int count, int *shown);

/* The index in items of the game highlighted in GAMES, or -1 if GAMES lists none. */
int ui_library_game(const ui_library *lib);

/* True if every word of `query` (split at spaces) is in the item's name, facts or about,
   ASCII case-insensitive. A query of only spaces, or "", matches everything. */
bool ui_library_matches(const ui_lib_item *it, const char *query);

/* Gives INSTALLED its list (again, after an install), keeping its place where it can. */
void ui_library_set_installed(ui_library *lib, const ui_lib_item *items, int count);

/* Says where games install, and which folders SETTINGS offers (the strings stay the caller's).
   Until then games install to E:\Games and SETTINGS offers just that. */
void ui_library_set_folders(ui_library *lib, const char *install_dir, const ui_lib_folder *folders,
                            int count);

/* Gives Authentication its servers (the strings stay the caller's), keeping its place where it
   can. Until then it lists archive.org alone. */
void ui_library_set_hosts(ui_library *lib, const ui_lib_host *hosts, int count);

/* The index in hosts of the server highlighted on the Authentication page. */
int ui_library_host(const ui_library *lib);

/* Gives Map sources its lines (the strings stay the caller's), keeping its place where it
   can. Until then it lists only "Add source". */
void ui_library_set_sources(ui_library *lib, const ui_lib_source *sources, int count);

/* The index in sources of the line highlighted on the Map sources page, or -1 for "Add
   source". */
int ui_library_source(const ui_library *lib);

/* The index in the INSTALLED list of the highlighted folder. */
int ui_library_installed(const ui_library *lib);

/* Gives the NAS page its NASes, saved ones first (the strings stay the caller's), with the
   first highlighted, and a line saying how the last scan went (may be NULL). */
void ui_library_set_nas(ui_library *lib, const ui_lib_nas *nas, int count, const char *status);

/* Says where the console is on the network, for the NAS pages ("This Xbox: 192.168.1.23");
   the string stays the caller's. NULL: nothing is said. */
void ui_library_set_console(ui_library *lib, const char *line);

/* The index in nas of the NAS highlighted on the NAS page, or -1 for another line. */
int ui_library_nas(const ui_library *lib);

/* Shows a NAS's shares, or a folder's folders, with the first line highlighted: first the
   line `use` ("Add this folder to GAMES") if it isn't NULL. The strings stay the caller's. */
void ui_library_browse(ui_library *lib, const char *title, const char *about,
                       const ui_lib_entry *entries, int count, const char *use);

/* The index in the browse entries of the highlighted line, or -1 for the `use` line. */
int ui_library_browse_entry(const ui_library *lib);

/* UI_LIB_NAS_USE's folder: -1 for the one browsed, else the index of a browse entry. */
int ui_library_nas_pick(const ui_library *lib);

/* Back from browsing to the NAS page. */
void ui_library_nas_page(ui_library *lib);

/* Wipes the NAS password typed for UI_LIB_NAS_LOGIN, once the login has used it. */
void ui_library_forget_nas_password(ui_library *lib);

/* Gives GAMES a new list of games (after the map changed): every game listed, the search and
   the highlight cleared. `shown` has room for `count` ints. */
void ui_library_set_items(ui_library *lib, const ui_lib_item *items, int count, int *shown);

/* Says whether the archive.org account is logged in (SETTINGS). */
void ui_library_set_account(ui_library *lib, bool logged_in);

/* Opens the keyboard for the email, then the password, as A in SETTINGS does when logged out:
   for logging in again after a download was refused. */
void ui_library_login(ui_library *lib);

/* Wipes the password typed for UI_LIB_LOGIN, once the login has used it. */
void ui_library_forget_password(ui_library *lib);

/* Opens a message with an OK button over the screen. `message` is copied. */
void ui_library_tell(ui_library *lib, const char *title, const char *message);

/* How many list lines the screen shows. */
int ui_library_rows(const ui_draw *d);

/* One button press. In a list, the D-pad, LT and RT move (ui_list_input), Left goes to the
   sections, A in GAMES asks to install the highlighted game, B in a searched GAMES lists every
   game again, and Back asks to leave. In the sections, Up and Down choose one, and Right or A go
   back to the list. Y, anywhere but a dialog, opens the keyboard on the search: DONE searches
   GAMES and shows it, keeping the highlighted game if it still matches; B there changes nothing.
   In SETTINGS, Up and Down choose a line. A on the install folder lists the folders to choose
   from, the current one highlighted: A on one gives UI_LIB_FOLDER, A on "Other folder" opens
   the keyboard on the current folder and DONE gives UI_LIB_FOLDER; B goes back. A on
   Authentication lists the servers. A on archive.org asks for the email, then the password
   (dots only), and DONE on a password gives UI_LIB_LOGIN; B wipes it. Logged in, A asks before
   UI_LIB_LOGOUT. A on another server with a login asks before UI_LIB_FORGET; without one, a
   message says how logins are added. B goes back. A on Map sources lists "Add source", then
   the map's lines. A on "Add source" asks what kind (ARCHIVE.ORG, MAP or IMAGE; B closes it),
   then opens the keyboard for the identifier or URL, and for an image the title ID too; DONE
   gives UI_LIB_ADD_SOURCE, and B there adds nothing (what was typed stays for the next try).
   A on a line asks before UI_LIB_REMOVE_SOURCE. A on NAS gives UI_LIB_NAS_PAGE and lists the
   NASes, then "Find a NAS on the network" (A gives UI_LIB_NAS_SCAN) and "Type a NAS address"
   (the keyboard). A on a NAS with a saved login gives UI_LIB_NAS_SAVED; A on one without, X
   on any, or DONE on an address, asks the user name (DONE on none logs in as a guest), then
   the password (dots only), and gives UI_LIB_NAS_LOGIN; B wipes the password. While browsing,
   A on a line gives UI_LIB_NAS_OPEN, A on the `use` line or X on a line asks before
   UI_LIB_NAS_USE, and B gives UI_LIB_NAS_UP. A on an installed game, in GAMES or INSTALLED, asks
   before UI_LIB_PLAY. In a dialog or the keyboard, their buttons. */
ui_lib_action ui_library_input(ui_library *lib, ui_button b, int rows);

/* The name of what the screen shows, for the UI script (docs/formats/ui-script.md §6): an open
   keyboard ("keyboard-search"), else an open dialog ("ask-install", "message"), else the section
   or SETTINGS' page ("games", "folders"). */
const char *ui_library_screen(const ui_library *lib);

/* Draws the screen with the background at pan (ui_pan_x), and `status` (may be NULL) in the
   footer. Under an open dialog or the keyboard, the screen behind stands still (ui_backdrop). */
void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_LIBRARY_H */
