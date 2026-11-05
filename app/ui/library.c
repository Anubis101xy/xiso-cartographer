/* The library screen (library.h). */
#include "library.h"

#include <stdio.h>
#include <string.h>

#include "install_view.h" /* ui_format_size */
#include "theme.h"

#define PAD 10

static const char *const section_names[UI_LIB_SECTION_COUNT_] = {"GAMES", "INSTALLED", "SETTINGS"};

/* The title-safe area (32, 24, 576 x 432), as docs/ui-style.md#layout. */
static const ui_rect tab = {32, 24, 170, 46};
static const ui_rect nav = {32, 80, 170, 196};
static const ui_rect summary = {32, 284, 170, 116};
static const ui_rect list = {210, 80, 212, 320};
static const ui_rect detail = {430, 80, 178, 320};
static const ui_rect footer = {32, 408, 576, 48};

/* The list's lines, inside its panel: 8 pixels in at the sides, PAD at the top and bottom. */
static const ui_rect list_in = {218, 90, 196, 300};

/* Right of the title tab: the addresses and the drives, or the search while GAMES is searched. */
static const ui_rect top_box = {210, 24, 398, 46};
static const ui_rect search_box = {210, 24, 398, 46};

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

void ui_library_open(ui_library *lib, const ui_lib_item *items, int count, int *shown)
{
    *lib = (ui_library){0};
    lib->items = items;
    lib->count = count;
    lib->shown = shown;
    for (int i = 0; i < count; i++) {
        shown[i] = i;
    }
    lib->list.count = count;
    static const ui_lib_folder games = {"E:\\Games", NULL};
    lib->install_dir = games.path;
    lib->apps_dir = "E:\\Apps";
    lib->folders = &games;
    lib->folder_count = 1;
    lib->settings_list.count = UI_LIB_SETTING_COUNT_;
    static const ui_lib_host archive = {"archive.org", true, NULL, NULL, NULL};
    lib->hosts = &archive;
    lib->host_count = 1;
    lib->host_list.count = 1;
    lib->source_list.count = 1; /* "Add source" */
    lib->nas_list.count = 2;    /* "Find a NAS on the network", "Type a NAS address" */
    lib->nas_pick = -1;
}

void ui_library_set_items(ui_library *lib, const ui_lib_item *items, int count, int *shown)
{
    lib->items = items;
    lib->count = count;
    lib->shown = shown;
    for (int i = 0; i < count; i++) {
        shown[i] = i;
    }
    lib->list = (ui_list){count, 0, 0};
    lib->query[0] = '\0';
}

/* Keeps a list's place in range after its lines changed. */
static void keep_place(ui_list *l, int count)
{
    l->count = count;
    if (l->cur >= count) {
        l->cur = count > 0 ? count - 1 : 0;
    }
    if (l->top > l->cur) {
        l->top = l->cur;
    }
}

void ui_library_set_sources(ui_library *lib, const ui_lib_source *sources, int count)
{
    lib->sources = sources;
    lib->source_count = count;
    keep_place(&lib->source_list, count + 1);
}

int ui_library_source(const ui_library *lib)
{
    return lib->source_list.cur - 1;
}

bool ui_library_to_apps(const ui_library *lib)
{
    return lib->to_apps;
}

int ui_library_installed(const ui_library *lib)
{
    return lib->installed_list.cur;
}

void ui_library_set_nas(ui_library *lib, const ui_lib_nas *nas, int count, const char *status)
{
    lib->nas = nas;
    lib->nas_count = count;
    lib->nas_status = status;
    lib->nas_list = (ui_list){count + 2, 0, 0}; /* the first NAS, else "Find a NAS" */
}

void ui_library_set_console(ui_library *lib, const char *line)
{
    lib->console_line = line;
}

/* The NAS page's lines after the NASes. */
#define NAS_FIND (-1)
#define NAS_TYPE (-2)

/* What line `cur` of the NAS page is: a NAS (its index), NAS_FIND or NAS_TYPE. */
static int nas_line(const ui_library *lib, int cur)
{
    return cur < lib->nas_count ? cur : cur == lib->nas_count ? NAS_FIND : NAS_TYPE;
}

int ui_library_nas(const ui_library *lib)
{
    int n = nas_line(lib, lib->nas_list.cur);
    return n >= 0 ? n : -1;
}

void ui_library_browse(ui_library *lib, const char *title, const char *about,
                       const ui_lib_entry *entries, int count, const char *use)
{
    lib->browse = entries;
    lib->browse_count = count;
    lib->browse_use = use;
    lib->browse_title = title;
    lib->browse_about = about;
    lib->browse_list = (ui_list){count + (use != NULL ? 1 : 0), 0, 0};
    lib->page = UI_LIB_PAGE_BROWSE;
}

int ui_library_browse_entry(const ui_library *lib)
{
    return lib->browse_list.cur - (lib->browse_use != NULL ? 1 : 0);
}

int ui_library_nas_pick(const ui_library *lib)
{
    return lib->nas_pick;
}

void ui_library_nas_page(ui_library *lib)
{
    lib->page = UI_LIB_PAGE_NAS;
}

void ui_library_forget_nas_password(ui_library *lib)
{
    volatile char *p = lib->nas_password; /* a store the compiler can't drop */
    for (size_t i = 0; i < sizeof lib->nas_password; i++) {
        p[i] = '\0';
    }
}

void ui_library_set_hosts(ui_library *lib, const ui_lib_host *hosts, int count)
{
    lib->hosts = hosts;
    lib->host_count = count;
    keep_place(&lib->host_list, count);
}

int ui_library_host(const ui_library *lib)
{
    return lib->host_list.cur;
}

void ui_library_set_folders(ui_library *lib, const char *install_dir, const ui_lib_folder *folders,
                            int count)
{
    lib->install_dir = install_dir;
    lib->folders = folders;
    lib->folder_count = count;
    lib->page = UI_LIB_PAGE_NONE;
}

static bool same_folder(const char *a, const char *b)
{
    for (; *a != '\0' && lower(*a) == lower(*b); a++, b++) {
    }
    return *a == *b;
}

/* Lists the folders, the one games install to highlighted (or "Other folder" if it's none of
   them). */
static void choose_folder(ui_library *lib, int rows)
{
    int cur = lib->folder_count;
    for (int i = 0; i < lib->folder_count; i++) {
        if (same_folder(lib->folders[i].path, lib->install_dir)) {
            cur = i;
        }
    }
    lib->folder_list = (ui_list){lib->folder_count + 1, cur, cur >= rows ? cur - rows + 1 : 0};
    lib->page = UI_LIB_PAGE_FOLDERS;
}

int ui_library_game(const ui_library *lib)
{
    return lib->list.count > 0 ? lib->shown[lib->list.cur] : -1;
}

/* True if s holds w[0, n), ASCII case-insensitive. */
static bool has_word(const char *s, const char *w, size_t n)
{
    if (s == NULL) {
        return false;
    }
    for (; *s != '\0'; s++) {
        size_t i = 0;
        while (i < n && s[i] != '\0' && lower(s[i]) == lower(w[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

int ui_library_name_cmp(const char *a, const char *b)
{
    const char *p = a;
    const char *q = b;
    while (*p != '\0' && *q != '\0') {
        if (digit(*p) && digit(*q)) {
            /* By value: leading zeros skipped, then the longer run is larger, else by digits. */
            while (*p == '0' && digit(p[1])) {
                p++;
            }
            while (*q == '0' && digit(q[1])) {
                q++;
            }
            size_t np = 0;
            size_t nq = 0;
            while (digit(p[np])) {
                np++;
            }
            while (digit(q[nq])) {
                nq++;
            }
            if (np != nq) {
                return np < nq ? -1 : 1;
            }
            int c = memcmp(p, q, np);
            if (c != 0) {
                return c < 0 ? -1 : 1;
            }
            p += np;
            q += nq;
            continue;
        }
        unsigned char x = (unsigned char)lower(*p);
        unsigned char y = (unsigned char)lower(*q);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        p++;
        q++;
    }
    if (*p != *q) {
        return *p == '\0' ? -1 : 1;
    }
    int c = strcmp(a, b); /* the same once folded: settle it by bytes */
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

bool ui_library_matches(const ui_lib_item *it, const char *query)
{
    const char *p = query;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            return true;
        }
        size_t n = strcspn(p, " ");
        if (!has_word(it->name, p, n) && !has_word(it->facts, p, n) && !has_word(it->about, p, n)) {
            return false;
        }
        p += n;
    }
}

/* Lists the games matching lib->query, keeping the highlighted one if it's among them. */
static void search(ui_library *lib, int rows)
{
    int was = ui_library_game(lib);
    int n = 0;
    int cur = 0;
    for (int i = 0; i < lib->count; i++) {
        if (ui_library_matches(&lib->items[i], lib->query)) {
            if (i == was) {
                cur = n;
            }
            lib->shown[n++] = i;
        }
    }
    lib->list = (ui_list){n, cur, cur >= rows ? cur - rows + 1 : 0};
}

void ui_library_set_account(ui_library *lib, bool logged_in)
{
    lib->logged_in = logged_in;
}

void ui_library_forget_password(ui_library *lib)
{
    volatile char *p = lib->password; /* a store the compiler can't drop */
    for (size_t i = 0; i < sizeof lib->password; i++) {
        p[i] = '\0';
    }
}

void ui_library_tell(ui_library *lib, const char *title, const char *message)
{
    (void)snprintf(lib->tell_title, sizeof lib->tell_title, "%s", title);
    (void)snprintf(lib->message, sizeof lib->message, "%s", message);
    lib->dialog = (ui_dialog){lib->tell_title, lib->message, {"OK", NULL, NULL}, 0, 0};
    lib->asking = UI_LIB_TELL;
    lib->typing_for = UI_LIB_TYPING_NONE;
    lib->backdrop = false;
}

void ui_library_set_installed(ui_library *lib, const ui_lib_item *items, int count)
{
    lib->installed = items;
    lib->installed_count = count;
    ui_list *l = &lib->installed_list;
    l->count = count;
    if (l->cur >= count) {
        l->cur = count > 0 ? count - 1 : 0;
    }
    if (l->top > l->cur) {
        l->top = l->cur;
    }
}

void ui_library_set_top(ui_library *lib, const ui_lib_top *top)
{
    lib->top = top != NULL ? *top : (ui_lib_top){0};
}

void ui_library_set_disks(ui_library *lib, const ui_lib_disk *disks, int count)
{
    lib->disks = disks;
    lib->disk_count = disks != NULL && count > 0 ? count : 0;
}

/* The highlighted line of the section shown, or NULL. */
bool ui_library_shown_item(const ui_library *lib, ui_lib_section *section, int *item)
{
    *section = lib->section;
    if (lib->section == UI_LIB_GAMES && lib->list.count > 0) {
        *item = ui_library_game(lib);
        return true;
    }
    if (lib->section == UI_LIB_INSTALLED_GAMES && lib->installed_count > 0) {
        *item = lib->installed_list.cur;
        return true;
    }
    *item = -1;
    return false;
}

void ui_library_set_art(ui_library *lib, ui_lib_section section, int item, const ui_lib_art *art)
{
    lib->art = art;
    lib->art_section = section;
    lib->art_item = item;
}

/* The art given for the item shown, or NULL. */
static const ui_lib_art *shown_art(const ui_library *lib)
{
    ui_lib_section section;
    int item = -1;
    if (lib->art == NULL || !ui_library_shown_item(lib, &section, &item)) {
        return NULL;
    }
    return section == lib->art_section && item == lib->art_item ? lib->art : NULL;
}

static const ui_lib_item *current(const ui_library *lib)
{
    if (lib->section == UI_LIB_GAMES && lib->list.count > 0) {
        return &lib->items[ui_library_game(lib)];
    }
    if (lib->section == UI_LIB_INSTALLED_GAMES && lib->installed_count > 0) {
        return &lib->installed[lib->installed_list.cur];
    }
    return NULL;
}

int ui_library_rows(const ui_draw *d)
{
    return ui_list_rows(d, list_in);
}

/* An item that was started and isn't finished: installing carries on, or it can be deleted. */
static bool unfinished(const ui_lib_item *it)
{
    return it->state == UI_LIB_UNFINISHED || it->state == UI_LIB_FAILED ||
           it->state == UI_LIB_STOPPED;
}

/* The game the delete question is about, in GAMES or INSTALLED. */
static const ui_lib_item *highlighted(const ui_library *lib)
{
    return lib->section == UI_LIB_GAMES ? &lib->items[ui_library_game(lib)]
                                        : &lib->installed[ui_library_installed(lib)];
}

static void ask(ui_library *lib, ui_lib_action what)
{
    if (what == UI_LIB_INSTALL) {
        const ui_lib_item *it = &lib->items[ui_library_game(lib)];
        if (unfinished(it)) {
            (void)snprintf(lib->message, sizeof lib->message,
                           "%s isn't finished. Carry on where it stopped, or delete what's on "
                           "the hard disk so far?",
                           it->name);
            lib->dialog =
                (ui_dialog){"NOT FINISHED", lib->message, {"CARRY ON", "DELETE", "CANCEL"}, 0, 2};
        } else if (it->app) {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Install %s to %s? It looks like an app. AS A GAME puts it in %s.",
                           it->name, lib->apps_dir, lib->install_dir);
            lib->dialog =
                (ui_dialog){"INSTALL", lib->message, {"INSTALL", "AS A GAME", "CANCEL"}, 0, 2};
        } else {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Install %s to %s? AS AN APP puts it in %s. B stops it; it carries "
                           "on later.",
                           it->name, lib->install_dir, lib->apps_dir);
            lib->dialog =
                (ui_dialog){"INSTALL", lib->message, {"INSTALL", "AS AN APP", "CANCEL"}, 0, 2};
        }
    } else if (what == UI_LIB_DELETE) {
        const ui_lib_item *it = highlighted(lib);
        if (it->state == UI_LIB_INSTALLED) {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Delete %s? %s Saved games stay: they're in E:\\UDATA, not in its "
                           "folder.",
                           it->name, it->note != NULL ? it->note : "");
        } else {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Delete what the unfinished install of %s left? %s Installing it "
                           "again starts over.",
                           it->name, it->note != NULL ? it->note : "");
        }
        lib->dialog = (ui_dialog){"DELETE", lib->message, {"DELETE", "KEEP", NULL}, 1, 1};
    } else if (what == UI_LIB_LOGOUT) {
        lib->dialog = (ui_dialog){"LOG OUT",
                                  "Log out of archive.org on this console? Items that need the "
                                  "login won't download until you log in again.",
                                  {"LOG OUT", "CANCEL", NULL},
                                  1,
                                  1};
    } else if (what == UI_LIB_FORGET) {
        const ui_lib_host *h = &lib->hosts[ui_library_host(lib)];
        (void)snprintf(lib->message, sizeof lib->message,
                       "Remove the login [%s] from this console? %s, and every other server it "
                       "is for, won't get it until it's added to the credentials file again.",
                       h->login, h->name);
        lib->dialog = (ui_dialog){"REMOVE LOGIN", lib->message, {"REMOVE", "CANCEL", NULL}, 1, 1};
    } else if (what == UI_LIB_REMOVE_SOURCE) {
        (void)snprintf(lib->message, sizeof lib->message,
                       "Remove this line from the map? %s. Its games leave the list.",
                       lib->sources[ui_library_source(lib)].name);
        lib->dialog = (ui_dialog){"REMOVE SOURCE", lib->message, {"REMOVE", "CANCEL", NULL}, 1, 1};
    } else if (what == UI_LIB_PLAY) {
        const ui_lib_item *it = lib->section == UI_LIB_GAMES
                                    ? &lib->items[ui_library_game(lib)]
                                    : &lib->installed[ui_library_installed(lib)];
        (void)snprintf(lib->message, sizeof lib->message,
                       "Play %s? This app closes and the game starts.", it->name);
        lib->dialog =
            lib->section == UI_LIB_GAMES
                ? (ui_dialog){"PLAY", lib->message, {"PLAY", "INSTALL AGAIN", "CANCEL"}, 0, 2}
                : (ui_dialog){"PLAY", lib->message, {"PLAY", "DELETE", "CANCEL"}, 0, 2};
    } else if (what == UI_LIB_NAS_USE) {
        /* Name exactly the folder that's added: the one browsed, or the highlighted one. */
        const char *where = lib->browse_title != NULL ? lib->browse_title : "";
        int e = lib->nas_pick;
        if (e >= 0 && e < lib->browse_count) {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Add %s%s%s to GAMES? Its images and game folders, and those one "
                           "folder down, are listed there.",
                           where, where[0] != '\0' ? " > " : "", lib->browse[e].name);
        } else {
            (void)snprintf(lib->message, sizeof lib->message,
                           "Add %s to GAMES? Its images and game folders, and those one folder "
                           "down, are listed there.",
                           where[0] != '\0' ? where : "this folder");
        }
        lib->dialog = (ui_dialog){"ADD NAS FOLDER", lib->message, {"ADD", "CANCEL", NULL}, 0, 1};
    } else if (what == UI_LIB_SOURCE_KIND) {
        /* Three choices and no CANCEL: B gives 3, a choice past them, which adds nothing. */
        lib->dialog = (ui_dialog){"ADD SOURCE",
                                  "What do you want to add to the map? B cancels.",
                                  {"ARCHIVE.ORG", "MAP", "IMAGE"},
                                  0,
                                  3};
    } else {
        lib->dialog =
            (ui_dialog){"LEAVE", "Return to the dashboard?", {"LEAVE", "STAY", NULL}, 0, 1};
    }
    lib->asking = what;
    lib->backdrop = false;
}

static void open_kbd(ui_library *lib, ui_lib_typing what)
{
    if (what == UI_LIB_TYPING_SEARCH) {
        ui_kbd_open(&lib->kbd, "SEARCH", lib->typing, sizeof lib->typing, false);
    } else if (what == UI_LIB_TYPING_EMAIL) {
        ui_kbd_open(&lib->kbd, "ARCHIVE.ORG EMAIL", lib->email, sizeof lib->email, false);
    } else if (what == UI_LIB_TYPING_FOLDER) {
        (void)snprintf(lib->folder, sizeof lib->folder, "%s", lib->install_dir);
        ui_kbd_open(&lib->kbd, "INSTALL FOLDER", lib->folder, sizeof lib->folder, false);
    } else if (what == UI_LIB_TYPING_SOURCE) {
        static const char *const labels[] = {"ARCHIVE.ORG ITEM IDENTIFIER", "MAP ADDRESS",
                                             "IMAGE ADDRESS"};
        ui_kbd_open(&lib->kbd, labels[lib->add_kind], lib->add_url, sizeof lib->add_url, false);
    } else if (what == UI_LIB_TYPING_TITLE_ID) {
        ui_kbd_open(&lib->kbd, "TITLE ID, LIKE 4D530004", lib->add_key, sizeof lib->add_key, false);
    } else if (what == UI_LIB_TYPING_NAS_ADDRESS) {
        if (lib->nas_addr[0] == '\0') {
            (void)snprintf(lib->nas_addr, sizeof lib->nas_addr, "%s", "192.168.1.");
        }
        ui_kbd_open(&lib->kbd, "NAS ADDRESS, LIKE 192.168.1.50", lib->nas_addr,
                    sizeof lib->nas_addr, false);
    } else if (what == UI_LIB_TYPING_NAS_USER) {
        ui_kbd_open(&lib->kbd, "NAS USER NAME (NONE FOR A GUEST)", lib->nas_user,
                    sizeof lib->nas_user, false);
    } else if (what == UI_LIB_TYPING_NAS_PASSWORD) {
        ui_library_forget_nas_password(lib);
        ui_kbd_open(&lib->kbd, "NAS PASSWORD", lib->nas_password, sizeof lib->nas_password, true);
    } else {
        ui_library_forget_password(lib);
        ui_kbd_open(&lib->kbd, "ARCHIVE.ORG PASSWORD", lib->password, sizeof lib->password, true);
    }
    lib->typing_for = what;
    lib->backdrop = false;
}

void ui_library_login(ui_library *lib)
{
    lib->asking = UI_LIB_NONE;
    open_kbd(lib, UI_LIB_TYPING_EMAIL);
}

/* The keyboard closed with DONE or B, or is still open. */
static ui_lib_action typed(ui_library *lib, ui_kbd_result r, int rows)
{
    if (r == UI_KBD_EDITING) {
        return UI_LIB_NONE;
    }
    ui_lib_typing what = lib->typing_for;
    lib->typing_for = UI_LIB_TYPING_NONE;
    if (what == UI_LIB_TYPING_PASSWORD) {
        if (r == UI_KBD_DONE && lib->password[0] != '\0') {
            return UI_LIB_LOGIN; /* the caller logs in, then forgets the password */
        }
        ui_library_forget_password(lib);
    } else if (what == UI_LIB_TYPING_NAS_PASSWORD) {
        if (r == UI_KBD_DONE) {
            return UI_LIB_NAS_LOGIN; /* an empty password is a password too */
        }
        ui_library_forget_nas_password(lib);
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_NAS_ADDRESS && lib->nas_addr[0] != '\0') {
        open_kbd(lib, UI_LIB_TYPING_NAS_USER);
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_NAS_USER) {
        if (lib->nas_user[0] == '\0') {
            ui_library_forget_nas_password(lib);
            return UI_LIB_NAS_LOGIN; /* as a guest */
        }
        open_kbd(lib, UI_LIB_TYPING_NAS_PASSWORD);
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_EMAIL && lib->email[0] != '\0') {
        open_kbd(lib, UI_LIB_TYPING_PASSWORD);
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_SOURCE && lib->add_url[0] != '\0') {
        if (lib->add_kind != UI_LIB_SOURCE_IMAGE) {
            return UI_LIB_ADD_SOURCE; /* the caller checks it */
        }
        open_kbd(lib, UI_LIB_TYPING_TITLE_ID);
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_TITLE_ID && lib->add_key[0] != '\0') {
        return UI_LIB_ADD_SOURCE;
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_FOLDER && lib->folder[0] != '\0') {
        lib->page = UI_LIB_PAGE_NONE;
        return UI_LIB_FOLDER; /* the caller checks it */
    } else if (r == UI_KBD_DONE && what == UI_LIB_TYPING_SEARCH) {
        (void)snprintf(lib->query, sizeof lib->query, "%s", lib->typing);
        lib->section = UI_LIB_GAMES;
        lib->in_sections = false;
        search(lib, rows);
    }
    return UI_LIB_NONE;
}

/* A on a server of the Authentication page. */
static void host_chosen(ui_library *lib)
{
    const ui_lib_host *h = &lib->hosts[ui_library_host(lib)];
    if (h->archive && lib->logged_in) {
        ask(lib, UI_LIB_LOGOUT);
    } else if (h->archive) {
        open_kbd(lib, UI_LIB_TYPING_EMAIL);
    } else if (h->login != NULL) {
        ask(lib, UI_LIB_FORGET);
    } else {
        char msg[sizeof lib->message];
        (void)snprintf(msg, sizeof msg,
                       "%s has no login. Logins for servers other than archive.org are added by "
                       "hand to E:\\Apps\\xcat\\credentials (see docs/formats/credentials.md).",
                       h->name);
        ui_library_tell(lib, "NO LOGIN", msg);
    }
}

/* The kind of source to add was chosen: the keyboard, on what was typed for that kind before,
   or on how its address starts. */
static void source_kind(ui_library *lib, int c)
{
    if (c < 0 || c > UI_LIB_SOURCE_IMAGE) {
        return; /* B */
    }
    if ((int)lib->add_kind != c || lib->add_url[0] == '\0') {
        static const char *const starts[] = {"", "https://", "http://"};
        lib->add_kind = (ui_lib_source_kind)c;
        (void)snprintf(lib->add_url, sizeof lib->add_url, "%s", starts[c]);
        lib->add_key[0] = '\0';
    }
    open_kbd(lib, UI_LIB_TYPING_SOURCE);
}

/* A press in SETTINGS' list, or in one of its pages. */
static ui_lib_action settings_input(ui_library *lib, ui_button b, int rows)
{
    if (lib->page == UI_LIB_PAGE_NAS) {
        int n = nas_line(lib, lib->nas_list.cur);
        if (b == UI_BTN_B) {
            lib->page = UI_LIB_PAGE_NONE;
        } else if (b == UI_BTN_A && n == NAS_FIND) {
            return UI_LIB_NAS_SCAN;
        } else if (b == UI_BTN_A && n == NAS_TYPE) {
            open_kbd(lib, UI_LIB_TYPING_NAS_ADDRESS);
        } else if ((b == UI_BTN_A || b == UI_BTN_X) && n >= 0) {
            const ui_lib_nas *nas = &lib->nas[n];
            (void)snprintf(lib->nas_addr, sizeof lib->nas_addr, "%s", nas->address);
            if (b == UI_BTN_A && nas->login != NULL) {
                return UI_LIB_NAS_SAVED;
            }
            /* Someone else (X), or no login yet: the user name, the saved one to start from. */
            (void)snprintf(lib->nas_user, sizeof lib->nas_user, "%s",
                           nas->login != NULL && strcmp(nas->login, "a guest") != 0 ? nas->login
                                                                                    : "");
            open_kbd(lib, UI_LIB_TYPING_NAS_USER);
        } else {
            (void)ui_list_input(&lib->nas_list, b, rows);
        }
        return UI_LIB_NONE;
    }
    if (lib->page == UI_LIB_PAGE_BROWSE) {
        int e = ui_library_browse_entry(lib);
        if (b == UI_BTN_B) {
            return UI_LIB_NAS_UP;
        }
        if (b == UI_BTN_A && lib->browse_list.count > 0 && e < 0) {
            lib->nas_pick = -1;
            ask(lib, UI_LIB_NAS_USE);
            return UI_LIB_NONE;
        }
        if (b == UI_BTN_A && lib->browse_list.count > 0) {
            return UI_LIB_NAS_OPEN;
        }
        if (b == UI_BTN_X && e >= 0 && e < lib->browse_count) {
            lib->nas_pick = e; /* add the highlighted folder without opening it */
            ask(lib, UI_LIB_NAS_USE);
            return UI_LIB_NONE;
        }
        (void)ui_list_input(&lib->browse_list, b, rows);
        return UI_LIB_NONE;
    }
    if (lib->page == UI_LIB_PAGE_SOURCES) {
        if (b == UI_BTN_B) {
            lib->page = UI_LIB_PAGE_NONE;
        } else if (b == UI_BTN_A) {
            ask(lib, ui_library_source(lib) < 0 ? UI_LIB_SOURCE_KIND : UI_LIB_REMOVE_SOURCE);
        } else {
            (void)ui_list_input(&lib->source_list, b, rows);
        }
    } else if (lib->page == UI_LIB_PAGE_LOGINS) {
        if (b == UI_BTN_B) {
            lib->page = UI_LIB_PAGE_NONE;
        } else if (b == UI_BTN_A && lib->host_count > 0) {
            host_chosen(lib);
        } else {
            (void)ui_list_input(&lib->host_list, b, rows);
        }
    } else if (lib->page == UI_LIB_PAGE_FOLDERS) {
        int cur = lib->folder_list.cur;
        if (b == UI_BTN_B) {
            lib->page = UI_LIB_PAGE_NONE;
        } else if (b == UI_BTN_A && cur < lib->folder_count) {
            (void)snprintf(lib->folder, sizeof lib->folder, "%s", lib->folders[cur].path);
            lib->page = UI_LIB_PAGE_NONE;
            return UI_LIB_FOLDER;
        } else if (b == UI_BTN_A) {
            open_kbd(lib, UI_LIB_TYPING_FOLDER);
        } else {
            (void)ui_list_input(&lib->folder_list, b, rows);
        }
    } else if (b != UI_BTN_A) {
        (void)ui_list_input(&lib->settings_list, b, rows);
    } else if (lib->settings_list.cur == UI_LIB_SETTING_FOLDER) {
        choose_folder(lib, rows);
    } else if (lib->settings_list.cur == UI_LIB_SETTING_SOURCES) {
        lib->page = UI_LIB_PAGE_SOURCES;
    } else if (lib->settings_list.cur == UI_LIB_SETTING_NAS) {
        lib->page = UI_LIB_PAGE_NAS;
        return UI_LIB_NAS_PAGE;
    } else {
        lib->page = UI_LIB_PAGE_LOGINS;
    }
    return UI_LIB_NONE;
}

ui_lib_action ui_library_input(ui_library *lib, ui_button b, int rows)
{
    if (lib->asking != UI_LIB_NONE) {
        int c = ui_dialog_input(&lib->dialog, b);
        if (c == UI_DIALOG_OPEN) {
            return UI_LIB_NONE;
        }
        ui_lib_action what = lib->asking;
        lib->asking = UI_LIB_NONE;
        if (what == UI_LIB_SOURCE_KIND) {
            source_kind(lib, c);
            return UI_LIB_NONE;
        }
        if (what == UI_LIB_PLAY && lib->section == UI_LIB_GAMES && c == 1) {
            lib->to_apps = lib->items[ui_library_game(lib)].app;
            return UI_LIB_INSTALL; /* INSTALL AGAIN */
        }
        if ((what == UI_LIB_PLAY && lib->section != UI_LIB_GAMES && c == 1) ||
            (what == UI_LIB_INSTALL && unfinished(&lib->items[ui_library_game(lib)]) && c == 1)) {
            ask(lib, UI_LIB_DELETE); /* DELETE: asked once more */
            return UI_LIB_NONE;
        }
        if (what == UI_LIB_INSTALL && (c == 0 || c == 1)) {
            const ui_lib_item *it = &lib->items[ui_library_game(lib)];
            /* INSTALL goes where the item suggests, the other button to the other folder. A
               game that isn't finished carries on where it is (the caller knows where). */
            lib->to_apps = unfinished(it) ? it->app : (c == 0) == it->app;
            return UI_LIB_INSTALL;
        }
        return c == 0 && what != UI_LIB_TELL ? what : UI_LIB_NONE;
    }
    if (lib->typing_for != UI_LIB_TYPING_NONE) {
        return typed(lib, ui_kbd_input(&lib->kbd, b), rows);
    }
    if ((b == UI_BTN_WHITE || b == UI_BTN_BLACK) && lib->section != UI_LIB_SETTINGS) {
        /* The details' text a line up or down; draw_detail keeps it in range. */
        if (b == UI_BTN_WHITE && lib->detail_scroll > 0) {
            lib->detail_scroll--;
        } else if (b == UI_BTN_BLACK && lib->detail_lines > lib->detail_fit) {
            lib->detail_scroll++;
        }
        return UI_LIB_NONE;
    }
    if (b == UI_BTN_Y) {
        (void)snprintf(lib->typing, sizeof lib->typing, "%s", lib->query);
        open_kbd(lib, UI_LIB_TYPING_SEARCH);
    } else if (b == UI_BTN_BACK) {
        ask(lib, UI_LIB_EXIT);
    } else if (lib->in_sections) {
        if (b == UI_BTN_UP && lib->section > 0) {
            lib->section--;
        } else if (b == UI_BTN_DOWN && lib->section + 1 < UI_LIB_SECTION_COUNT_) {
            lib->section++;
        } else if (b == UI_BTN_RIGHT || b == UI_BTN_A) {
            lib->in_sections = false;
        }
    } else if (b == UI_BTN_LEFT) {
        lib->in_sections = true;
        lib->page = UI_LIB_PAGE_NONE;
    } else if (lib->section == UI_LIB_GAMES) {
        if (b == UI_BTN_A && lib->list.count > 0) {
            bool installed = lib->items[ui_library_game(lib)].state == UI_LIB_INSTALLED;
            ask(lib, installed ? UI_LIB_PLAY : UI_LIB_INSTALL);
        } else if (b == UI_BTN_B && lib->query[0] != '\0') {
            lib->query[0] = '\0';
            search(lib, rows);
        } else {
            (void)ui_list_input(&lib->list, b, rows);
        }
    } else if (lib->section == UI_LIB_INSTALLED_GAMES && b == UI_BTN_A) {
        if (lib->installed_count > 0) {
            bool done = lib->installed[ui_library_installed(lib)].state == UI_LIB_INSTALLED;
            ask(lib, done ? UI_LIB_PLAY : UI_LIB_DELETE);
        }
    } else if (lib->section == UI_LIB_INSTALLED_GAMES) {
        (void)ui_list_input(&lib->installed_list, b, rows);
    } else if (lib->section == UI_LIB_SETTINGS) {
        return settings_input(lib, b, rows);
    }
    return UI_LIB_NONE;
}

static const char *item_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->items[lib->shown[i]].name;
}

static const char *installed_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->installed[i].name;
}

static void draw_sections(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, nav, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    for (int i = 0; i < UI_LIB_SECTION_COUNT_; i++) {
        ui_rect item = {nav.x + 8, nav.y + PAD + i * (lh + 16), nav.w - 16, lh + 10};
        bool cur = i == (int)lib->section;
        if (cur) {
            ui_panel(d, item, 8, ui_col_highlight,
                     lib->in_sections ? ui_col_highlight_border : ui_col_border);
        }
        (void)ui_text_fit(d, UI_FONT_LABEL, item.x + 10, item.y + 5, item.w - 14,
                          cur ? ui_col_text : ui_col_dim, section_names[i]);
    }
}

#define DISK_BAR_H 5   /* the bar under a drive's line */
#define DISK_BAR_GAP 2 /* above and below it */
#define DISK_GAP 4     /* between drives */

/* How much of drive k is used, as a bar w wide. */
static void draw_disk_bar(const ui_draw *d, int x, int y, int w, const ui_lib_disk *k)
{
    d->fill(d->ctx, (ui_rect){x, y, w, DISK_BAR_H}, ui_col_idle);
    uint64_t used = k->free_bytes < k->total_bytes ? k->total_bytes - k->free_bytes : 0U;
    int u = (int)(used * (uint64_t)w / k->total_bytes);
    if (u == 0 && used > 0U) {
        u = 1; /* a drive with anything on it never looks empty */
    }
    if (u > 0) {
        d->fill(d->ctx, (ui_rect){x, y, u, DISK_BAR_H}, ui_col_border);
    }
}

/* INSTALLED's summary: each drive's free space and a bar of what's used, then its size if
   every drive's fits. Drives that don't fit are left out. */
static void draw_disks(const ui_draw *d, const ui_library *lib, int x, int y, int w)
{
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int bottom = summary.y + summary.h - PAD;
    int n = lib->disk_count;
    int with_size = sh + DISK_BAR_GAP + DISK_BAR_H + DISK_BAR_GAP + sh;
    bool sizes = n * with_size + (n - 1) * DISK_GAP <= bottom - y;
    char size[24];
    char line[48];
    for (int i = 0; i < n && y + sh <= bottom; i++) {
        const ui_lib_disk *k = &lib->disks[i];
        ui_format_size(size, sizeof size, k->free_bytes);
        (void)snprintf(line, sizeof line, "%s %s free", k->name != NULL ? k->name : "", size);
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
        y += sh;
        if (k->total_bytes > 0U && y + DISK_BAR_GAP + DISK_BAR_H <= bottom) {
            draw_disk_bar(d, x, y + DISK_BAR_GAP, w, k);
            y += DISK_BAR_GAP + DISK_BAR_H;
            if (sizes) {
                ui_format_size(size, sizeof size, k->total_bytes);
                (void)snprintf(line, sizeof line, "of %s", size);
                (void)ui_text_fit(d, UI_FONT_SMALL, x, y + DISK_BAR_GAP, w, ui_col_dim, line);
                y += DISK_BAR_GAP + sh;
            }
        }
        y += DISK_GAP;
    }
}

#define PAIR_GAP 6   /* between a label and its value */
#define ITEM_GAP 16  /* between one pair and the next */
#define TOP_BAR_W 48 /* a drive's bar when there are several */

/* The width of "LABEL value" in the small font. */
static int pair_width(const ui_draw *d, const char *label, const char *value)
{
    return d->text_width(d->ctx, UI_FONT_SMALL, label, strlen(label)) + PAIR_GAP +
           d->text_width(d->ctx, UI_FONT_SMALL, value, strlen(value));
}

/* "LABEL value" in at most w pixels, the label dim; returns the width drawn. */
static int draw_pair(const ui_draw *d, int x, int y, int w, const char *label, const char *value,
                     ui_color c)
{
    int lw = ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, label);
    if (lw + PAIR_GAP >= w) {
        return lw;
    }
    return lw + PAIR_GAP +
           ui_text_fit(d, UI_FONT_SMALL, x + lw + PAIR_GAP, y, w - lw - PAIR_GAP, c, value);
}

/* The top panel: the console's address left and the NAS's right, then each drive's free space
   with a bar of how full it is (one drive: its size too, and the bar takes the rest). */
static void draw_top(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, top_box, UI_CHAMFER, ui_col_panel, ui_col_border);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int x = top_box.x + 16;
    int right = top_box.x + top_box.w - 16;
    int y = top_box.y + (top_box.h - 2 * sh) / 2;

    const char *xbox = lib->top.xbox != NULL && lib->top.xbox[0] != '\0' ? lib->top.xbox : "-";
    char nas[UI_LIB_NAS_ADDR_MAX + 8];
    (void)snprintf(nas, sizeof nas, "%s%s", lib->top.nas != NULL ? lib->top.nas : "none",
                   lib->top.nas != NULL && lib->top.nas_off ? " off" : "");
    int xw = pair_width(d, "XBOX", xbox);
    int nw = pair_width(d, "NAS", nas);
    int room = right - x - ITEM_GAP;
    if (xw + nw > room) { /* too long for both: each gets half, or what the other leaves */
        int half = room / 2;
        if (xw <= half) {
            nw = room - xw;
        } else if (nw <= half) {
            xw = room - nw;
        } else {
            xw = half;
            nw = room - half;
        }
    }
    (void)draw_pair(d, x, y, xw, "XBOX", xbox, ui_col_text);
    (void)draw_pair(d, right - nw, y, nw, "NAS", nas,
                    lib->top.nas != NULL && lib->top.nas_off ? ui_col_accent : ui_col_text);

    y += sh;
    if (lib->disk_count == 0) {
        (void)draw_pair(d, x, y, right - x, "DRIVE", "space not read", ui_col_dim);
        return;
    }
    int cx = x;
    for (int i = 0; i < lib->disk_count && cx < right; i++) {
        const ui_lib_disk *k = &lib->disks[i];
        char free_s[24];
        char total_s[24];
        char value[64];
        ui_format_size(free_s, sizeof free_s, k->free_bytes);
        if (lib->disk_count == 1 && k->total_bytes > 0U) {
            ui_format_size(total_s, sizeof total_s, k->total_bytes);
            (void)snprintf(value, sizeof value, "%s free of %s", free_s, total_s);
        } else {
            (void)snprintf(value, sizeof value, "%s free", free_s);
        }
        cx += draw_pair(d, cx, y, right - cx, k->name != NULL ? k->name : "", value, ui_col_text);
        int bw = lib->disk_count == 1 ? right - cx - 12 : TOP_BAR_W;
        if (k->total_bytes > 0U && bw >= 24 && cx + 8 + bw <= right) {
            draw_disk_bar(d, cx + 8, y + (sh - DISK_BAR_H) / 2 + 1, bw, k);
            cx += 8 + bw;
        }
        cx += ITEM_GAP;
    }
}

static void draw_summary(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, summary, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = summary.x + 14;
    int w = summary.w - 24;
    int y = summary.y + PAD;
    if (lib->section == UI_LIB_INSTALLED_GAMES && lib->disk_count > 0) {
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, "HARD DRIVE");
        draw_disks(d, lib, x, y + d->line_height(d->ctx, UI_FONT_LABEL) + 4, w);
        return;
    }
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, "SUMMARY");
    y += d->line_height(d->ctx, UI_FONT_LABEL) + 4;
    int installed = 0;
    int unfinished = 0;
    for (int i = 0; i < lib->count; i++) {
        installed += lib->items[i].state == UI_LIB_INSTALLED;
        unfinished += lib->items[i].state == UI_LIB_FAILED ||
                      lib->items[i].state == UI_LIB_STOPPED ||
                      lib->items[i].state == UI_LIB_UNFINISHED;
    }
    char line[48];
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    (void)snprintf(line, sizeof line, "%d game%s listed", lib->count, lib->count == 1 ? "" : "s");
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    y += sh;
    (void)snprintf(line, sizeof line, "%d installed", installed);
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    y += sh;
    if (unfinished > 0) {
        (void)snprintf(line, sizeof line, "%d unfinished", unfinished);
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, line);
    }
}

/* The about text from its line `skip` on, wrapped into r; returns how many lines it has in
   all at r's width. */
static int draw_about(const ui_draw *d, ui_rect r, int skip, const char *s)
{
    size_t n = strlen(s);
    size_t at = 0;
    int lines = 0;
    size_t from = n;
    while (at < n) {
        if (lines == skip) {
            from = at;
        }
        size_t next = 0;
        (void)ui_wrap(d, UI_FONT_SMALL, s + at, n - at, r.w, &next);
        at += next > 0 ? next : 1;
        lines++;
    }
    if (from < n && r.h > 0) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, r, ui_col_dim, s + from);
    }
    return lines;
}

static void draw_detail(const ui_draw *d, ui_library *lib, const ui_lib_item *it)
{
    ui_panel(d, detail, UI_CHAMFER, ui_col_panel, ui_col_border);
    lib->detail_lines = 0;
    lib->detail_fit = 0;
    if (it != lib->detail_of) {
        lib->detail_of = it;
        lib->detail_scroll = 0;
    }
    if (it == NULL) {
        return;
    }
    const ui_lib_art *art = shown_art(lib);
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    if (art != NULL && art->px != NULL && art->w > 0 && art->h > 0) {
        /* The picture, centred at the top, cut to the largest it may be. */
        int pw = art->w < UI_LIB_ART_W ? art->w : UI_LIB_ART_W;
        int ph = art->h < UI_LIB_ART_H ? art->h : UI_LIB_ART_H;
        ui_rect box = {detail.x + (detail.w - pw) / 2, y, pw, ph};
        d->clip(d->ctx, &box);
        d->pixels(d->ctx, art->px, art->w, art->h, box.x, box.y);
        d->clip(d->ctx, NULL);
        y += ph + 8;
    }
    const char *name =
        art != NULL && art->title != NULL && art->title[0] != '\0' ? art->title : it->name;
    const char *facts = art != NULL && art->facts != NULL ? art->facts : it->facts;
    const char *about = art != NULL && art->about != NULL ? art->about : it->about;
    /* The name on up to two lines. */
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent, name) * bh;
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    if (facts != NULL && facts[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, facts);
        y += sh;
    }
    y += 6;
    if (it->state != UI_LIB_NOT_TRIED) {
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w,
                          it->state == UI_LIB_INSTALLED ? ui_col_accent : ui_col_text,
                          it->state == UI_LIB_INSTALLED    ? "INSTALLED"
                          : it->state == UI_LIB_STOPPED    ? "STOPPED"
                          : it->state == UI_LIB_UNFINISHED ? "UNFINISHED"
                                                           : "FAILED");
        y += d->line_height(d->ctx, UI_FONT_LABEL);
        if (it->note != NULL && it->note[0] != '\0') {
            y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 3 * sh}, ui_col_text, it->note) *
                 sh;
        }
        y += 6;
    }
    if (about != NULL && y < bottom) {
        /* White and Black move it a line at a time when it doesn't all fit. */
        lib->detail_fit = sh > 0 ? (bottom - y) / sh : 0;
        int total = draw_about(d, (ui_rect){x, y, w, 0}, 0, about);
        int most = total > lib->detail_fit ? total - lib->detail_fit : 0;
        if (lib->detail_scroll > most) {
            lib->detail_scroll = most;
        }
        lib->detail_lines =
            draw_about(d, (ui_rect){x, y, w, bottom - y}, lib->detail_scroll, about);
    }
}

static const char *setting_name(void *ctx, int i)
{
    (void)ctx;
    static const char *const names[UI_LIB_SETTING_COUNT_] = {"Install location", "Authentication",
                                                             "Map sources", "NAS"};
    return names[i];
}

static const char *folder_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return i < lib->folder_count ? lib->folders[i].path : "Other folder";
}

/* SETTINGS' details for the install folder, or for the folder highlighted in the list. */
static void draw_folder(const ui_draw *d, ui_library *lib)
{
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    if (lib->page != UI_LIB_PAGE_FOLDERS) {
        (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, ui_col_accent, "Install location");
        y += bh + 6;
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, lib->install_dir);
        y += lh;
        y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 2 * sh}, ui_col_text,
                          "A chooses where games install.") *
             sh;
        y += 6;
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim,
                           "Games already installed stay where they are. INSTALLED lists the "
                           "folder chosen here.");
        return;
    }
    int cur = lib->folder_list.cur;
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent,
                      folder_name(lib, cur)) *
         bh;
    if (cur == lib->folder_count) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y + 6, w, bottom - y - 6}, ui_col_text,
                           "A types a folder on E:, F: or G:, like F:\\Games\\Xbox. It's made if "
                           "it isn't there.");
        return;
    }
    const char *note = lib->folders[cur].note;
    if (note != NULL && note[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, note);
        y += sh;
    }
    y += 6;
    bool here = same_folder(lib->folders[cur].path, lib->install_dir);
    if (here) {
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, "CURRENT");
        y += lh;
    }
    (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_text,
                       here ? "Games install here now." : "A installs games here from now on.");
}

static const char *source_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return i == 0 ? "Add source" : lib->sources[i - 1].name;
}

/* Map sources' details: the line in SETTINGS, "Add source", or the map's line highlighted. */
static void draw_sources(const ui_draw *d, const ui_library *lib)
{
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    const char *title = "Map sources";
    const char *text = "The lines of E:\\xcat\\map.txt that list your games. A lists them.";
    const char *more = "Add an archive.org item, a map or an image, or remove one. The games "
                       "are listed again after each change.";
    const char *note = NULL;
    if (lib->page == UI_LIB_PAGE_SOURCES && ui_library_source(lib) < 0) {
        title = "Add source";
        text = "An archive.org item (its name), a map (its address), or one image (its address "
               "and title ID).";
        more = "It goes at the end of the map.";
    } else if (lib->page == UI_LIB_PAGE_SOURCES) {
        const ui_lib_source *s = &lib->sources[ui_library_source(lib)];
        title = s->name;
        note = s->note;
        text = s->about;
        more = "A removes this line from the map.";
    }
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent, title) * bh;
    if (note != NULL && note[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, note);
        y += sh;
    }
    y += 6;
    if (text != NULL) {
        y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 8 * sh}, ui_col_text, text) * sh;
        y += 6;
    }
    if (y < bottom) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim, more);
    }
}

static const char *nas_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    int n = nas_line(lib, i);
    return n == NAS_FIND   ? "Find a NAS on the network"
           : n == NAS_TYPE ? "Type a NAS address"
                           : lib->nas[n].name;
}

static const char *browse_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    if (lib->browse_use != NULL) {
        if (i == 0) {
            return lib->browse_use;
        }
        i--;
    }
    return lib->browse[i].name;
}

/* NAS's details: the line in SETTINGS, the NAS page's line, or where browsing is. The last
   line says where the console is on the network, when the caller has said. */
static void draw_nas(const ui_draw *d, const ui_library *lib)
{
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    if (lib->console_line != NULL && lib->console_line[0] != '\0') {
        bottom -= sh;
        (void)ui_text_fit(d, UI_FONT_SMALL, x, bottom, w, ui_col_accent, lib->console_line);
        bottom -= 6;
    }
    const char *title = "NAS";
    const char *note = NULL;
    const char *text = "Games on your NAS: find it on the network, log in once, and add the "
                       "folder your games are in. A starts.";
    const char *more = "Its images (.iso, .xiso, .zip) and game folders (with a default.xbe) "
                       "are listed in GAMES and install from it over the network.";
    char line[160];
    if (lib->page == UI_LIB_PAGE_NAS) {
        int n = nas_line(lib, lib->nas_list.cur);
        if (n == NAS_FIND) {
            title = "Find a NAS on the network";
            text = "Asks every address of this network for file sharing (SMB), a few at a time. "
                   "It takes about ten seconds.";
            more = lib->nas_status;
        } else if (n == NAS_TYPE) {
            title = "Type a NAS address";
            text = "The NAS's IP address, like 192.168.1.50, as its settings or your router "
                   "show it. Add :port for another port than 445.";
            more = "Then its user name and password. The login is saved for next time.";
        } else {
            const ui_lib_nas *nas = &lib->nas[n];
            title = nas->name;
            note = nas->note;
            if (nas->login != NULL) {
                (void)snprintf(line, sizeof line,
                               "Logged in as %s before: A opens its shared folders. X logs in "
                               "as someone else.",
                               nas->login);
                text = line;
            } else {
                text = "A logs in: its user name, then its password. No user name logs in as a "
                       "guest. The login is saved, so you type it once.";
            }
            more = "Only the password's NT hash is kept, in E:\\Apps\\xcat\\credentials. "
                   "SETTINGS > Authentication removes it.";
        }
    } else if (lib->page == UI_LIB_PAGE_BROWSE) {
        int e = ui_library_browse_entry(lib);
        const char *where = lib->browse_title != NULL ? lib->browse_title : "";
        if (e < 0) {
            title = where;
            text = lib->browse_about;
            more = "A adds this folder to GAMES (it asks first). B goes up.";
        } else if (e < lib->browse_count) {
            title = lib->browse[e].name;
            note = lib->browse[e].note;
            if (lib->browse_use == NULL) {
                text = "A opens this shared folder. X adds all of it to GAMES.";
            } else {
                text = "A opens it. X adds it to GAMES without opening it: its images and game "
                       "folders, and those one folder down.";
            }
            (void)snprintf(line, sizeof line, "In %s. B goes up.", where);
            more = line;
        } else {
            title = where;
            text = lib->browse_about;
            more = "B goes up.";
        }
    }
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent, title) * bh;
    if (note != NULL && note[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, note);
        y += sh;
    }
    y += 6;
    if (text != NULL && y < bottom) {
        y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 8 * sh}, ui_col_text, text) * sh;
        y += 6;
    }
    if (more != NULL && y < bottom) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim, more);
    }
}

static const char *host_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->hosts[i].name;
}

/* Authentication's details: the line in SETTINGS, or the server highlighted on its page. For
   archive.org, logged in or not, and what happens to the password. */
static void draw_logins(const ui_draw *d, const ui_library *lib)
{
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    if (lib->page != UI_LIB_PAGE_LOGINS) {
        (void)ui_text_fit(d, UI_FONT_BOLD, x, y, w, ui_col_accent, "Authentication");
        y += bh + 6;
        y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 5 * sh}, ui_col_text,
                          "The logins for the servers your map gets games from. A lists them.") *
             sh;
        y += 6;
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim,
                           "archive.org logs in here. Other logins come from the credentials "
                           "file, and can be removed here.");
        return;
    }
    if (lib->host_count == 0) {
        return;
    }
    const ui_lib_host *h = &lib->hosts[ui_library_host(lib)];
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent, h->name) * bh;
    if (h->note != NULL && h->note[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, h->note);
        y += sh;
    }
    y += 6;
    char label[48];
    bool in = h->archive ? lib->logged_in : h->login != NULL;
    if (h->archive) {
        (void)snprintf(label, sizeof label, "%s", in ? "LOGGED IN" : "NOT LOGGED IN");
    } else if (in) {
        (void)snprintf(label, sizeof label, "LOGIN [%s]", h->login);
    } else {
        (void)snprintf(label, sizeof label, "NO LOGIN");
    }
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, in ? ui_col_accent : ui_col_text, label);
    y += lh;
    const char *what = h->archive && in ? "Items that need your login download with it. A logs out."
                       : h->archive     ? "A logs in with your archive.org email and password."
                       : in             ? "Sent only to the servers the login is for. A removes it."
                                        : "Logins for servers other than archive.org are added by "
                                          "hand to the credentials file.";
    y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 4 * sh}, ui_col_text, what) * sh;
    y += 6;
    const char *about = h->about;
    if (h->archive) {
        about = "The password is sent once, over HTTPS, and never stored: only the session is.";
    }
    if (about != NULL && y < bottom) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim, about);
    }
}

/* The list of the section shown. In the sections, the D-pad moves between them, so its
   highlight is drawn dim. */
bool ui_library_tick(ui_library *lib, uint32_t now_ms)
{
    lib->now_ms = now_ms;
    return lib->slide_over > 0 &&
           ui_slide_offset(now_ms - lib->slide_since, lib->slide_over) != lib->slide_px;
}

/* The list in the list panel, its highlighted line sliding if it's too wide and the buttons
   move it. */
static void draw_lines(const ui_draw *d, ui_library *lib, const ui_list *l, ui_list_line *line,
                       void *ctx)
{
    bool still =
        lib->in_sections || lib->asking != UI_LIB_NONE || lib->typing_for != UI_LIB_TYPING_NONE;
    if (l != lib->slide_list || l->cur != lib->slide_cur || still) {
        lib->slide_list = still ? NULL : l;
        lib->slide_cur = l->cur;
        lib->slide_since = lib->now_ms;
    }
    lib->slide_over = still ? 0 : ui_list_overflow(d, l, list_in, line, ctx);
    lib->slide_px = ui_slide_offset(lib->now_ms - lib->slide_since, lib->slide_over);
    ui_list_draw_slid(d, l, list_in, line, ctx, lib->in_sections, lib->slide_px);
}

static void draw_list(const ui_draw *d, ui_library *lib)
{
    lib->slide_over = 0; /* a list drawn below sets it */
    ui_panel(d, list, UI_CHAMFER, ui_col_panel, ui_col_border);
    if (lib->section == UI_LIB_INSTALLED_GAMES) {
        if (lib->installed_count == 0) {
            (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                               "Nothing installed yet. Games go to the install location in "
                               "SETTINGS.");
        } else {
            draw_lines(d, lib, &lib->installed_list, installed_name, lib);
        }
        return;
    }
    if (lib->section == UI_LIB_SETTINGS) {
        if (lib->page == UI_LIB_PAGE_FOLDERS) {
            draw_lines(d, lib, &lib->folder_list, folder_name, lib);
        } else if (lib->page == UI_LIB_PAGE_LOGINS) {
            draw_lines(d, lib, &lib->host_list, host_name, lib);
        } else if (lib->page == UI_LIB_PAGE_SOURCES) {
            draw_lines(d, lib, &lib->source_list, source_name, lib);
        } else if (lib->page == UI_LIB_PAGE_NAS) {
            draw_lines(d, lib, &lib->nas_list, nas_name, lib);
        } else if (lib->page == UI_LIB_PAGE_BROWSE) {
            if (lib->browse_list.count == 0) {
                (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                                   "Nothing here this login can open. B goes back.");
            } else {
                draw_lines(d, lib, &lib->browse_list, browse_name, lib);
            }
        } else {
            draw_lines(d, lib, &lib->settings_list, setting_name, NULL);
        }
        return;
    }
    if (lib->count == 0) {
        (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                           "No games. Put a map in E:\\xcat\\map.txt, or images in "
                           "E:\\xcat\\isos.");
        return;
    }
    if (lib->list.count == 0) {
        char line[UI_LIB_QUERY_MAX + 64];
        (void)snprintf(line, sizeof line,
                       "No game matches \"%s\". Y to search again, B to list them all.",
                       lib->query);
        (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim, line);
        return;
    }
    draw_lines(d, lib, &lib->list, item_name, lib);
}

/* While GAMES is searched: what for, and how many games match. */
static void draw_search(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, search_box, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int x = search_box.x + 16;
    int right = search_box.x + search_box.w - 16;
    int y = search_box.y + (search_box.h - lh) / 2;
    x += ui_text_fit(d, UI_FONT_LABEL, x, y, right - x, ui_col_accent, "SEARCH") + 12;
    char n[32];
    (void)snprintf(n, sizeof n, "%d of %d", lib->list.count, lib->count);
    int nw = d->text_width(d->ctx, UI_FONT_SMALL, n, strlen(n));
    int sy = search_box.y + (search_box.h - sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, right - nw, sy, nw, ui_col_dim, n);
    (void)ui_text_fit(d, UI_FONT_BODY, x,
                      search_box.y + (search_box.h - d->line_height(d->ctx, UI_FONT_BODY)) / 2,
                      right - nw - 12 - x, ui_col_text, lib->query);
}

static const char *hints(const ui_library *lib)
{
    if (lib->in_sections) {
        return "Up Down section  Right or A list  Y search  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_FOLDERS) {
        return "A choose  B back  Left sections  Y search  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_LOGINS && lib->host_count > 0) {
        const ui_lib_host *h = &lib->hosts[ui_library_host(lib)];
        return h->archive && lib->logged_in ? "A log out  B back  Left sections  Back exit"
               : h->archive                 ? "A log in  B back  Left sections  Back exit"
               : h->login != NULL           ? "A remove login  B back  Left sections  Back exit"
                                            : "A how to add one  B back  Left sections  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_NAS) {
        int n = nas_line(lib, lib->nas_list.cur);
        return n == NAS_FIND               ? "A find  B back  Left sections  Back exit"
               : n == NAS_TYPE             ? "A type it  B back  Left sections  Back exit"
               : lib->nas[n].login != NULL ? "A open  X other login  B back  Left sections"
                                           : "A log in  B back  Left sections  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_BROWSE) {
        return ui_library_browse_entry(lib) < 0 ? "A add this folder  B up  Left sections"
               : lib->browse_count > 0          ? "A open  X add it  B up  LT RT page"
                                                : "B up  Left sections  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_SOURCES) {
        return ui_library_source(lib) < 0 ? "A add  B back  Left sections  Back exit"
                                          : "A remove  B back  Left sections  Back exit";
    }
    if (lib->section == UI_LIB_SETTINGS && lib->page == UI_LIB_PAGE_NONE) {
        return lib->settings_list.cur == UI_LIB_SETTING_FOLDER
                   ? "A change  Left sections  Y search  Back exit"
               : lib->settings_list.cur == UI_LIB_SETTING_SOURCES
                   ? "A list, add, remove  Left sections  Y search  Back exit"
               : lib->settings_list.cur == UI_LIB_SETTING_NAS
                   ? "A find a NAS  Left sections  Y search  Back exit"
                   : "A servers and logins  Left sections  Y search  Back exit";
    }
    if (lib->section == UI_LIB_INSTALLED_GAMES) {
        if (lib->installed_count > 0 &&
            lib->installed[ui_library_installed(lib)].state != UI_LIB_INSTALLED) {
            return "A delete  LT RT page  Left sections  Y search  Back exit";
        }
        return "A play or delete  LT RT page  Left sections  Y search  Back exit";
    }
    if (lib->section != UI_LIB_GAMES) {
        return "LT RT page  Left sections  Y search  Back exit";
    }
    int g = ui_library_game(lib);
    if (g >= 0 && unfinished(&lib->items[g])) {
        return "A carry on or delete  Y search  LT RT page  Left sections";
    }
    return lib->query[0] != '\0' ? "A install  B list all  Y search  LT RT page  Left sections"
                                 : "A install  Y search  LT RT page  Left sections  Back exit";
}

static void draw_screen(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    bool searched = lib->query[0] != '\0';
    const ui_rect panels[] = {tab, nav, summary, list, detail, footer, top_box};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]));

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "LIBRARY");

    if (searched) {
        draw_search(d, lib);
    } else {
        draw_top(d, lib);
    }
    draw_sections(d, lib);
    draw_summary(d, lib);
    draw_list(d, lib);
    if (lib->section == UI_LIB_SETTINGS) {
        ui_panel(d, detail, UI_CHAMFER, ui_col_panel, ui_col_border);
        bool on_line = lib->page == UI_LIB_PAGE_NONE;
        int line = lib->settings_list.cur;
        if (lib->page == UI_LIB_PAGE_FOLDERS || (on_line && line == UI_LIB_SETTING_FOLDER)) {
            draw_folder(d, lib);
        } else if (lib->page == UI_LIB_PAGE_SOURCES ||
                   (on_line && line == UI_LIB_SETTING_SOURCES)) {
            draw_sources(d, lib);
        } else if (lib->page == UI_LIB_PAGE_NAS || lib->page == UI_LIB_PAGE_BROWSE ||
                   (on_line && line == UI_LIB_SETTING_NAS)) {
            draw_nas(d, lib);
        } else {
            draw_logins(d, lib);
        }
    } else {
        draw_detail(d, lib, current(lib));
    }

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int by = footer.y + (footer.h - 2 * sh) / 2;
    if (status != NULL) {
        (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_text, status);
    }
    char hint[160];
    (void)snprintf(hint, sizeof hint, "%s%s", hints(lib),
                   lib->section != UI_LIB_SETTINGS && lib->detail_lines > lib->detail_fit
                       ? "  White Black more"
                       : "");
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by + sh, footer.w - 24, ui_col_dim, hint);
}

const char *ui_library_screen(const ui_library *lib)
{
    switch (lib->typing_for) {
    case UI_LIB_TYPING_SEARCH:
        return "keyboard-search";
    case UI_LIB_TYPING_EMAIL:
        return "keyboard-email";
    case UI_LIB_TYPING_PASSWORD:
        return "keyboard-password";
    case UI_LIB_TYPING_FOLDER:
        return "keyboard-folder";
    case UI_LIB_TYPING_SOURCE:
        return "keyboard-source";
    case UI_LIB_TYPING_TITLE_ID:
        return "keyboard-title-id";
    case UI_LIB_TYPING_NAS_ADDRESS:
        return "keyboard-nas-address";
    case UI_LIB_TYPING_NAS_USER:
        return "keyboard-nas-user";
    case UI_LIB_TYPING_NAS_PASSWORD:
        return "keyboard-nas-password";
    case UI_LIB_TYPING_NONE:
        break;
    }
    switch (lib->asking) {
    case UI_LIB_INSTALL:
        return "ask-install";
    case UI_LIB_EXIT:
        return "ask-exit";
    case UI_LIB_LOGOUT:
        return "ask-logout";
    case UI_LIB_FORGET:
        return "ask-forget";
    case UI_LIB_REMOVE_SOURCE:
        return "ask-remove";
    case UI_LIB_SOURCE_KIND:
        return "ask-kind";
    case UI_LIB_PLAY:
        return "ask-play";
    case UI_LIB_DELETE:
        return "ask-delete";
    case UI_LIB_NAS_USE:
        return "ask-nas-use";
    case UI_LIB_TELL:
        return "message";
    default:
        break;
    }
    switch (lib->section) {
    case UI_LIB_INSTALLED_GAMES:
        return "installed";
    case UI_LIB_SETTINGS:
        return lib->page == UI_LIB_PAGE_FOLDERS   ? "folders"
               : lib->page == UI_LIB_PAGE_LOGINS  ? "logins"
               : lib->page == UI_LIB_PAGE_SOURCES ? "sources"
               : lib->page == UI_LIB_PAGE_NAS     ? "nas"
               : lib->page == UI_LIB_PAGE_BROWSE  ? "nas-browse"
                                                  : "settings";
    default:
        return "games";
    }
}

void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    bool typing = lib->typing_for != UI_LIB_TYPING_NONE;
    if (lib->asking == UI_LIB_NONE && !typing) {
        lib->backdrop = false;
        draw_screen(d, lib, pan, status);
        return;
    }
    if (lib->backdrop) {
        ui_backdrop(d);
    } else {
        draw_screen(d, lib, pan, status);
        ui_backdrop_save(d);
        lib->backdrop = true;
    }
    if (typing) {
        ui_kbd_draw(d, &lib->kbd);
    } else {
        ui_dialog_draw(d, &lib->dialog);
    }
}
