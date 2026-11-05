/*
 * xiso-cartographer XBE, milestone M6: install games over the network (HTTP or HTTPS),
 * resumably, from images or from zips holding one (ADR 0009).
 *
 * The map is a catalog: the app lists, and installs only the game picked with A,
 *   1. every entry of the map E:\xcat\map.txt (docs/formats/map.md), including those an
 *      @archiveorg item generates (docs/formats/archiveorg.md), downloaded over HTTP with
 *      Range and installed while it streams in (ADR 0004); an entry ending in .zip
 *      installs the zip's image (docs/formats/zip.md, stream-install.md);
 *   2. every image or zip in E:\xcat\isos\ (the M2 local install);
 *   3. if there is neither, the bundled synthetic test image (D:\samples\*.iso).
 * Each one goes through xc_install (xcat/install.h): FATX check, free space, a journal so a
 * pulled cable or a power cut resumes instead of starting over, a read-back check of every
 * file, and the .xcat-installed marker. Logins come from E:\Apps\xcat\credentials
 * (docs/formats/credentials.md). Everything shown is also appended to E:\xcat\install.log.
 * The game catalog E:\Apps\xcat\catalog.bin (docs/formats/catalog.md, M7), if present,
 * names the listed games and describes them; after an install, the title ID from default.xbe
 * is compared with the catalog's.
 *
 * The screens open first (M8, ADR 0015, docs/ui-style.md): the startup screen shows what
 * startup does, or why it can't go on (nxdk's debug console only if the screens can't open).
 * Then the library lists the games, Y searches them, A asks before installing
 * one, and the install screen shows the progress; B asks before stopping it. The games already in
 * the install folder (a marker, or a journal for an unfinished one: xcat/installed.h) are marked
 * in the list and listed under INSTALLED. SETTINGS chooses the install folder, E:\Games by
 * default, kept in E:\Apps\xcat\settings (docs/formats/settings.md, ADR 0016); it logs in to
 * archive.org with an email and password typed on the console, keeping only the session (ADR
 * 0010, docs/formats/login.md), and logs out. Every message is in the log, never a password or
 * a session. A UI script next to the XBE (D:\ui-script.txt, docs/formats/ui-script.md) presses
 * the buttons for the scripted walkthrough, alongside the controller.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "draw_sdl.h"
#include "input_sdl.h"
#include "install_view.h"
#include "library.h"
#include "script.h"
#include "startup_view.h"
#include "theme.h"
#include "xcat/art.h"
#include "xcat/catalog.h"
#include "xcat/credentials.h"
#include "xcat/http_client.h"
#include "xcat/install.h"
#include "xcat/installed.h"
#include "xcat/login.h"
#include "xcat/map.h"
#include "xcat/map_edit.h"
#include "xcat/nxdk.h"
#include "xcat/picture.h"
#include "xcat/settings.h"
#include "xcat/sevenz.h"
#include "xcat/smb.h"
#include "xcat/smb_client.h"
#include "xcat/smb_scan.h"
#include "xcat/synopsis.h"
#include "xcat/ui_script.h"
#include "xcat/version.h"
#include "xcat/xbe.h"
#include "xcat/xdvdfs.h"
#include "xcat/zip.h"

#define XCAT_DIR "E:\\xcat"
#define MAP_FILE XCAT_DIR "\\map.txt"
#define ISO_DIR XCAT_DIR "\\isos"
#define LOG_FILE XCAT_DIR "\\install.log"
#define APPS_DIR "E:\\Apps\\xcat"
#define APPS_ROOT "E:\\Apps" /* where apps install (ui_library_to_apps) */
#define CRED_FILE APPS_DIR "\\credentials"
#define CATALOG_FILE "E:\\Apps\\xcat\\catalog.bin"
#define SETTINGS_FILE APPS_DIR "\\settings"
#define SAMPLE_DIR "D:\\samples"
#define SCRIPT_FILE "D:\\ui-script.txt" /* next to the XBE (docs/formats/ui-script.md §2) */
#define IO_BUF_SIZE ((size_t)1024 * 1024)
#define RECV_BUF_SIZE ((size_t)256 * 1024)
#define CLUSTER                                                                                    \
    16384U           /* FATX cluster size on a retail E: partition, if a drive's can't be read     \
                      */
#define ATTEMPTS 40U /* consecutive failures before giving up: about half an hour */
#define MAX_LOCAL 16
#define LOG_MAX_BYTES (1024U * 1024U)
#define REPORT_EVERY_MS 5000U

static uint8_t *io_buf;
static uint8_t *recv_buf;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static ui_draw screen;
static bool screens_open;       /* ui_sdl_open worked: the debug console is never drawn again */
static bool starting;           /* the startup screen shows what out() writes */
static ui_startup_view startup; /* the screen before the library (ADR 0015 decision 1) */

static void startup_frame(void);

/* --- log, and the startup screen (or the debug console before the screens open) ---------- */

__attribute__((format(printf, 1, 2))) static void out(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof line) {
        n = (int)sizeof line - 1;
    }
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        (void)WriteFile(log_file, line, (DWORD)n, &put, NULL);
        IO_STATUS_BLOCK io; /* flushed: the run usually ends at the power switch */
        (void)NtFlushBuffersFile(log_file, &io);
    }
    if (starting) {
        ui_startup_add(&startup, line);
        startup_frame();
    } else if (!screens_open) {
        debugPrint("%s", line);
    }
}

static void open_log(void)
{
    log_file =
        CreateFileA(LOG_FILE, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD end = SetFilePointer(log_file, 0, NULL, FILE_END);
    if (end == INVALID_SET_FILE_POINTER || end > LOG_MAX_BYTES) {
        SetFilePointer(log_file, 0, NULL, FILE_BEGIN);
        SetEndOfFile(log_file);
    } else if (end > 0) {
        DWORD put = 0;
        (void)WriteFile(log_file, "\n", 1, &put, NULL);
    }
}

/* --- controllers: LT + RT + Back + Black returns to the dashboard ------------------- */

#define PAD_POLL_MS 50U

static bool pads_ok;

static void pads_init(void)
{
    pads_ok = SDL_Init(SDL_INIT_GAMECONTROLLER) == 0;
}

/* True while LT + RT + Back + Black (the rocky5 shortcut) is held on any controller. The
   screens read the buttons (ui_sdl_button), which keeps the controllers' state current. */
static bool exit_combo(void)
{
    return pads_ok && ui_sdl_exit_combo();
}

static void close_log(void)
{
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
}

/* Back to the dashboard. Everything written so far is on the disk already (journal and
   checkpoints are flushed as they go), so an unfinished install resumes next time. */
static void to_dashboard(void)
{
    starting = false; /* nothing more is drawn */
    out("\nReturning to the dashboard...\n");
    close_log();
    if (screens_open) {
        ui_sdl_close();
    }
    HalReturnToFirmware(HalQuickRebootRoutine);
}

/* One frame of the startup screen. The buttons are read meanwhile, so the dashboard shortcut
   works while the app starts. */
static void startup_frame(void)
{
    while (pads_ok && ui_sdl_button() != UI_BTN_NONE) {
    }
    if (exit_combo()) {
        to_dashboard();
    }
    ui_sdl_begin(ui_col_bg);
    ui_startup_draw(&screen, &startup, ui_pan_x(SDL_GetTicks(), ui_pan_range(&screen)));
    ui_sdl_present();
}

/* The app can't go on: says why, on the startup screen (or on the debug console if the screens
   never opened), and waits for the dashboard shortcut. */
static void halt(const char *why)
{
    starting = false;
    out("%s\n", why);
    close_log();
    if (screens_open) {
        ui_startup_fail(&startup, why);
    }
    int drawn = -1;
    for (;;) {
        while (pads_ok && ui_sdl_button() != UI_BTN_NONE) {
        }
        if (exit_combo()) {
            to_dashboard();
        }
        int pan = screens_open ? ui_pan_x(SDL_GetTicks(), ui_pan_range(&screen)) : 0;
        if (screens_open && pan != drawn) {
            drawn = pan;
            ui_sdl_begin(ui_col_bg);
            ui_startup_draw(&screen, &startup, pan);
            ui_sdl_present();
        }
        Sleep(PAD_POLL_MS);
    }
}

/* --- the UI script: presses for the scripted walkthrough (ADR 0015 decision 6) ------------ */

static xc_ui_script *script_data; /* D:\ui-script.txt, if it's there and valid */
static ui_script player;          /* UI_SCRIPT_OFF without one */
static char script_problem[320];  /* why D:\ui-script.txt isn't played, for the library */
static char last_screen[XC_UI_SCRIPT_SCREEN_MAX + 1];

/* The next press: the controller's, else the script's. `screen` is what the screen shows
   (ui_library_screen, ui_install_screen): an expect waits for it, and each new one is logged
   while the script runs. */
static ui_button next_button(const char *screen)
{
    ui_button b = ui_sdl_button();
    if (b != UI_BTN_NONE || player.state != UI_SCRIPT_RUNNING) {
        return b;
    }
    if (strcmp(screen, last_screen) != 0) {
        snprintf(last_screen, sizeof last_screen, "%s", screen);
        out("Screen: %s\n", screen);
    }
    char log[200];
    b = ui_script_next(&player, SDL_GetTicks(), screen, log, sizeof log);
    if (log[0] != '\0') {
        out("%s\n", log);
    }
    return b;
}

/* "12.3" from tenths: PDCLib's printf has no floating point. */
static void tenths(char *buf, size_t size, uint64_t t)
{
    (void)snprintf(buf, size, "%llu.%llu", (unsigned long long)(t / 10U),
                   (unsigned long long)(t % 10U));
}

/* --- the install screen, on a thread of its own ------------------------------------------ */

/*
 * The install reports progress once a MiB: in the spooled part of a zip that's every half
 * second or so, and never while it waits on the network or a disk flush. nxdk's SDL sees a
 * button only if it's still down when the pads are read, so a press between two reports was
 * lost. The screen's thread reads the pads every PAD_POLL_MS instead, and while it runs it
 * alone calls SDL. The install's callbacks change `view` under view_lock and read the flags.
 */

#define INSTALL_DRAW_MS 250U /* a frame costs the install CPU time: four a second */

static xc_settings settings; /* E:\Apps\xcat\settings, or the defaults */
static ui_install_view view;
static CRITICAL_SECTION view_lock;
static bool view_dirty;           /* a callback changed `view`: draw it at the next look */
static volatile bool stop_asked;  /* the user confirmed STOP */
static volatile bool combo_held;  /* the dashboard shortcut was held */
static volatile bool screen_quit; /* the install is over: the thread ends */
static HANDLE screen_thread;      /* NULL: the callbacks look at the buttons themselves */
static int pan_now;               /* the background's pan, which stands still during an install */
static uint32_t next_redraw;      /* SDL_GetTicks() of the next frame, if nothing asks sooner */

static bool due(uint32_t at)
{
    return (int32_t)(SDL_GetTicks() - at) >= 0;
}

/* One look at the buttons, and a frame if something changed or INSTALL_DRAW_MS passed. */
static void install_look(void)
{
    EnterCriticalSection(&view_lock);
    ui_button b;
    while ((b = next_button(ui_install_screen(&view))) != UI_BTN_NONE) {
        view_dirty = true;
        if (ui_install_input(&view, b) == UI_INST_STOP) {
            stop_asked = true;
            ui_install_status(&view, "Stopping...");
        }
    }
    if (exit_combo()) {
        combo_held = true;
    }
    bool draw = view_dirty || due(next_redraw);
    if (draw) {
        view_dirty = false;
        next_redraw = SDL_GetTicks() + INSTALL_DRAW_MS;
        ui_sdl_begin(ui_col_bg);
        ui_install_draw(&screen, &view, pan_now);
    }
    LeaveCriticalSection(&view_lock); /* the frame is drawn: showing it needs no `view` */
    if (draw) {
        ui_sdl_present();
    }
}

static DWORD WINAPI install_screen(LPVOID arg)
{
    (void)arg;
    while (!screen_quit) {
        install_look();
        Sleep(PAD_POLL_MS);
    }
    return 0;
}

/* Where the next install goes: settings.install_dir, or APPS_ROOT for an app. */
static const char *dest_dir;

/* Puts the install screen up for `name`, drawn and read by its thread until install_done. */
static void install_start(const char *name)
{
    ui_install_open(&view, name);
    view.dir = dest_dir != NULL ? dest_dir : settings.install_dir;
    stop_asked = false;
    combo_held = false;
    screen_quit = false;
    view_dirty = true;
    next_redraw = SDL_GetTicks();
    screen_thread = CreateThread(NULL, 256U * 1024U, install_screen, NULL, 0, NULL);
    if (screen_thread == NULL) {
        out("No thread for the install screen: it updates as the install reports progress\n");
        install_look();
    }
}

static void install_done(void)
{
    screen_quit = true;
    if (screen_thread != NULL) {
        (void)WaitForSingleObject(screen_thread, INFINITE);
        CloseHandle(screen_thread);
        screen_thread = NULL;
    }
}

/* Sets the screen's status line from a callback. */
static void install_say(const char *fmt, ...)
{
    EnterCriticalSection(&view_lock);
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(view.status, sizeof view.status, fmt, ap);
    va_end(ap);
    view_dirty = true;
    LeaveCriticalSection(&view_lock);
}

/* Whether the install should go on: not stopped, nor the dashboard shortcut held. */
static bool carry_on(void)
{
    if (screen_thread == NULL) {
        install_look();
    }
    return !stop_asked && !combo_held;
}

/* --- network, on first use ------------------------------------------------------------ */

static bool net_tried;
static bool net_up;
/* Where the console is on the network, for the NAS pages ("" until the network is tried). */
static char console_line[96];
static char console_ip[32]; /* the top panel's: the console's address, "offline", or "" */
static xc_http_config http_cfg;
static bool https_tried;                      /* TLS was set up, or tried (https_ready) */
static xc_err https_err = XC_ERR_UNSUPPORTED; /* why there is no TLS, if there isn't */
static xc_tls *tls;
static xc_net tls_net;
static xc_net net;
static xc_clock clk;
static xc_credentials creds;
static xc_http_auth auth;
static bool have_creds;
static char settings_problem[256]; /* why it isn't used, for the screen; "" if it is */
static bool has_f, has_g;          /* the drives F: and G: are there */
static xc_map map;                 /* E:\xcat\map.txt and what it includes */
static bool have_map;
static xc_http_client client;
static uint64_t run_start_ms;

static uint64_t now_ms(void)
{
    return clk.now_ms(clk.ctx);
}

static bool on_retry(void *ctx, const xc_http_retry *r)
{
    (void)ctx;
    xc_nx_net_stats st;
    xc_nx_net_last(&st);
    char delay[24];
    tenths(delay, sizeof delay, r->delay_ms / 100U);
    unsigned long long t = (unsigned long long)((now_ms() - run_start_ms) / 1000U);
    if (r->status != 0) {
        out("  %4llu s  HTTP %d, retrying in %s s (failure %u of %u)\n", t, r->status, delay,
            r->failures, r->max_attempts);
    } else {
        out("  %4llu s  %s, retrying in %s s (failure %u of %u)\n", t, xc_err_str(r->err), delay,
            r->failures, r->max_attempts);
    }
    /* Which step was slow (diagnosis for the first-request timeout seen in M3). */
    out("           last connect: lookup %lu ms, handshake %lu ms, %s\n", (unsigned long)st.dns_ms,
        (unsigned long)st.connect_ms, xc_err_str(st.result));
    if (r->status != 0) {
        install_say("The server said HTTP %d. Trying again in %s s (%u of %u).", r->status, delay,
                    r->failures, r->max_attempts);
    } else {
        install_say("%s. Trying again in %s s (%u of %u).", xc_err_str(r->err), delay, r->failures,
                    r->max_attempts);
    }
    return carry_on(); /* stopping also works while waiting out an outage */
}

static void load_credentials(void)
{
    if (!xc_nx_exists(CRED_FILE)) {
        /* credentials.md §2: not an error, but say so; a .txt name is an easy mistake */
        out("No credentials: %s doesn't exist%s\n", CRED_FILE,
            xc_nx_exists(CRED_FILE ".txt") ? " (there is a credentials.txt: rename it to "
                                             "credentials, without .txt)"
                                           : "");
        return;
    }
    xc_nx_file f = {NULL};
    xc_source src;
    char *data = NULL;
    xc_err err = xc_nx_file_open(&src, &f, CRED_FILE);
    if (err == XC_OK && src.size > XC_CRED_MAX_FILE) {
        err = XC_ERR_LIMIT;
    }
    if (err == XC_OK) {
        data = malloc((size_t)src.size + 1);
        err = data != NULL ? xc_source_read(&src, 0, data, (size_t)src.size) : XC_ERR_NOMEM;
    }
    xc_cred_error cerr;
    memset(&cerr, 0, sizeof cerr);
    if (err == XC_OK) {
        err = xc_cred_parse(data, (size_t)src.size, &creds, &cerr);
    }
    free(data);
    xc_nx_file_close(&f);
    if (err == XC_OK) {
        have_creds = true;
        out("Credentials: %u section(s) from %s\n", (unsigned)creds.section_count, CRED_FILE);
    } else if (err == XC_ERR_CREDENTIALS) {
        out("%s line %u: %s; installing without credentials\n", CRED_FILE, (unsigned)cerr.line,
            cerr.message);
    } else {
        out("%s: %s; installing without credentials\n", CRED_FILE, xc_err_str(err));
    }
}

/* xc_nx_https_start reports each step through this. */
static void say(const char *line)
{
    out("%s", line);
    line += strspn(line, " ");
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == ' ')) {
        n--;
    }
    install_say("%.*s", (int)n, line);
}

static bool is_https(const char *url)
{
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "HTTPS://", 8) == 0;
}

static bool network(void)
{
    if (net_tried) {
        return net_up;
    }
    net_tried = true;
    char ip[32] = "";
    say("Starting the network (dashboard settings)...\n");
    if (xc_nx_net_startup(ip, sizeof ip) != XC_OK) {
        out("No network: check the cable and the dashboard's network settings.\n");
        snprintf(console_line, sizeof console_line, "%s",
                 "This Xbox isn't on the network: check the cable.");
        snprintf(console_ip, sizeof console_ip, "%s", "offline");
        return false;
    }
    out("Console IP %s\n", ip);
    snprintf(console_line, sizeof console_line, "This Xbox: %s", ip);
    snprintf(console_ip, sizeof console_ip, "%s", ip);
    xc_nx_net(&net);
    memset(&http_cfg, 0, sizeof http_cfg);
    http_cfg.net = &net;
    http_cfg.clock = &clk;
    http_cfg.buf = recv_buf;
    http_cfg.buf_size = RECV_BUF_SIZE;
    http_cfg.max_attempts = ATTEMPTS;
    http_cfg.seed = (uint32_t)now_ms();
    http_cfg.on_retry = on_retry;
    if (have_creds) {
        xc_cred_http_auth(&auth, &creds);
        http_cfg.auth = &auth;
    }
    net_up = xc_http_client_init(&client, &http_cfg) == XC_OK;
    return net_up;
}

/* The first https URL sets up TLS (ADR 0012: randomness, NTP, trust) and gives it to the
   HTTP client. False, after saying why once, if HTTPS can't be used. */
static bool https_ready(void)
{
    if (https_tried) {
        return https_err == XC_OK;
    }
    https_tried = true;
    char ntp_buf[256];
    xc_nx_https_opts o = {&net, &clk, xc_nx_ntp_setting(ntp_buf, sizeof ntp_buf), say};
    https_err = xc_nx_https_start(&o, &tls_net, &tls);
    if (https_err == XC_OK) {
        http_cfg.tls = &tls_net;
        xc_http_client_close(&client);
        https_err = xc_http_client_init(&client, &http_cfg);
    }
    if (https_err != XC_OK) {
        out("HTTPS isn't available: %s. http:// entries still install.\n", xc_err_str(https_err));
    }
    return https_err == XC_OK;
}

/* --- the map ------------------------------------------------------------------------------ */

static xc_err read_local(const char *path, size_t max_len, char **data, size_t *len)
{
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err == XC_OK && src.size > max_len) {
        err = XC_ERR_LIMIT;
    }
    char *buf = NULL;
    if (err == XC_OK) {
        buf = malloc((size_t)src.size + 1);
        err = buf != NULL ? xc_source_read(&src, 0, buf, (size_t)src.size) : XC_ERR_NOMEM;
    }
    xc_nx_file_close(&f);
    if (err != XC_OK) {
        free(buf);
        return err;
    }
    *data = buf;
    *len = (size_t)src.size;
    return XC_OK;
}

typedef struct mem_ctx {
    char *buf;
    size_t len;
    size_t cap;
} mem_ctx;

/* xc_http_data_fn into a growing buffer. A body that starts over at 0 (xc_http_get after a
 * retry) replaces what came before. */
static xc_err to_mem(void *ctx, uint64_t offset, const uint8_t *data, size_t len)
{
    mem_ctx *m = ctx;
    m->len = (size_t)offset;
    if (m->cap - m->len <= len) {
        size_t cap = m->cap == 0 ? (size_t)64 * 1024 : m->cap;
        while (cap - m->len <= len) {
            cap *= 2;
        }
        char *grown = realloc(m->buf, cap);
        if (grown == NULL) {
            return XC_ERR_NOMEM;
        }
        m->buf = grown;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return XC_OK;
}

static xc_http_resource res; /* large: one at a time */

/* xc_map_fetch_fn: includes from the HDD or over HTTP. */
static xc_err map_fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **data,
                        size_t *len)
{
    (void)ctx;
    if (!is_url) {
        return read_local(location, max_len, data, len);
    }
    if (!network()) {
        return XC_ERR_NET_CONNECT;
    }
    if (is_https(location) && !https_ready()) {
        return https_err;
    }
    xc_err err = xc_http_resource_init(&res, location, strlen(location));
    mem_ctx m = {NULL, 0, 0};
    if (err == XC_OK) {
        err = xc_http_get(&client, &res, max_len, to_mem, &m);
    }
    if (err == XC_OK && m.buf == NULL) {
        m.buf = malloc(1); /* an empty body */
        err = m.buf != NULL ? XC_OK : XC_ERR_NOMEM;
    }
    if (err != XC_OK) {
        free(m.buf);
        return err;
    }
    *data = m.buf;
    *len = m.len;
    return XC_OK;
}

/* xc_map_smb_list_fn: an @smb folder's images, with the login the credentials file gives its
   NAS (smb.md §3, §4). */
/* The NASes the map's @smb folders are on, in the map's order, and whether any of a NAS's
   folders listed: the top panel's. Emptied each time the map loads. */
#define MAX_SMB_SEEN 8
static struct {
    char host[XC_SMB_MAX_HOST + 1];
    bool listed;
} smb_seen[MAX_SMB_SEEN];
static int smb_seen_n;

static void saw_nas(const char *host, bool listed)
{
    for (int i = 0; i < smb_seen_n; i++) {
        if (strcmp(smb_seen[i].host, host) == 0) {
            smb_seen[i].listed = smb_seen[i].listed || listed;
            return;
        }
    }
    if (smb_seen_n < MAX_SMB_SEEN) {
        snprintf(smb_seen[smb_seen_n].host, sizeof smb_seen[smb_seen_n].host, "%s", host);
        smb_seen[smb_seen_n].listed = listed;
        smb_seen_n++;
    }
}

static xc_err map_list_smb(void *ctx, const xc_smb_url *folder, xc_smb_listing *listing)
{
    (void)ctx;
    if (!network()) {
        saw_nas(folder->host, false);
        return XC_ERR_NET_CONNECT;
    }
    say("Listing a folder on the NAS...\n");
    xc_smb_conn *c = NULL;
    char detail[XC_SMB_DETAIL_MAX];
    xc_err err = xc_smb_connect_url(folder, have_creds ? &creds : NULL, &c, detail, sizeof detail);
    if (err == XC_OK) {
        err = xc_smb_list_tree(c, folder->path, listing);
        if (err != XC_OK) {
            snprintf(detail, sizeof detail, "%s", xc_smb_detail(c));
        }
        xc_smb_disconnect(c);
    }
    saw_nas(folder->host, err == XC_OK);
    if (err != XC_OK) {
        out("  NAS %s, share %s: %s (%s)\n", folder->host, folder->share, xc_err_str(err), detail);
    } else {
        out("  NAS %s, share %s: %u image(s)\n", folder->host, folder->share,
            (unsigned)listing->count);
    }
    return err;
}

static bool load_map(xc_map *map)
{
    memset(map, 0, sizeof *map);
    smb_seen_n = 0;
    if (!xc_nx_exists(MAP_FILE)) {
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(MAP_FILE, XC_MAP_MAX_FILE, &data, &len);
    if (err != XC_OK) {
        out("%s: %s\n", MAP_FILE, err == XC_ERR_LIMIT ? "larger than 1 MiB" : xc_err_str(err));
        return false;
    }
    xc_map_loader loader = {map_fetch, NULL, map_list_smb};
    err = xc_map_load(data, len, MAP_FILE, &loader, map);
    free(data);
    for (size_t i = 0; i < map->problem_count; i++) {
        const xc_map_problem *p = &map->problems[i];
        out("%s:%u: %s: %s\n", map->sources[p->source], (unsigned)p->line,
            p->severity == XC_MAP_ERROR ? "error" : "warning", p->message);
    }
    if (map->problems_dropped > 0) {
        out("... and %u more map problems\n", (unsigned)map->problems_dropped);
    }
    out("Map: %u entr%s from %u file(s)\n", (unsigned)map->entry_count,
        map->entry_count == 1 ? "y" : "ies", (unsigned)map->source_count);
    return err == XC_OK;
}

/* --- logging in to archive.org (docs/formats/login.md, ADR 0010) ------------------------- */

/* Reads the credentials file again, after a login or a log out, and gives it to the client. */
static void reload_credentials(void)
{
    if (have_creds) {
        xc_cred_free(&creds);
        have_creds = false;
    }
    load_credentials();
    if (net_up) {
        if (have_creds) {
            xc_cred_http_auth(&auth, &creds);
        }
        http_cfg.auth = have_creds ? &auth : NULL;
        xc_http_client_close(&client);
        net_up = xc_http_client_init(&client, &http_cfg) == XC_OK;
    }
}

static bool logged_in(void)
{
    return have_creds && xc_cred_section_named(&creds, XC_LOGIN_SECTION) != NULL;
}

/* Writes section [name] into the credentials file, or removes it when `section` is NULL
   (login.md §5). On failure, msg says why for the screen. */
static bool write_section(const char *name, const char *section, char *msg, size_t size)
{
    char *old = NULL;
    size_t old_len = 0;
    xc_err err = XC_OK;
    if (xc_nx_exists(CRED_FILE)) {
        err = read_local(CRED_FILE, XC_CRED_MAX_FILE, &old, &old_len);
    }
    char *text = NULL;
    size_t len = 0;
    xc_cred_error cerr;
    memset(&cerr, 0, sizeof cerr);
    if (err == XC_OK) {
        err = xc_cred_set_section(old, old_len, name, section, &text, &len, &cerr);
    }
    if (old != NULL) {
        xc_login_wipe(old, old_len);
        free(old);
    }
    if (err == XC_OK && len > 0) {
        (void)xc_nx_mkdirs(APPS_DIR);
        err = xc_nx_replace_file(CRED_FILE, text, len);
    }
    if (text != NULL) {
        xc_login_wipe(text, len);
        free(text);
    }
    if (err == XC_ERR_CREDENTIALS) {
        out("%s line %u: %s; not changed\n", CRED_FILE, (unsigned)cerr.line, cerr.message);
        (void)snprintf(msg, size,
                       "E:\\Apps\\xcat\\credentials has a problem on line %u: %s. Fix or delete "
                       "it, then try again.",
                       (unsigned)cerr.line, cerr.message);
    } else if (err != XC_OK) {
        out("%s: %s; not changed\n", CRED_FILE, xc_err_str(err));
        (void)snprintf(msg, size, "E:\\Apps\\xcat\\credentials couldn't be written: %s.",
                       xc_err_str(err));
    }
    return err == XC_OK;
}

/* Reads E:\Apps\xcat\settings (settings.md §2): without one, the defaults; with one that can't
   be used, the defaults too, and settings_problem says why. An install folder that can't be
   made (its drive is gone) falls back to E:\Games for this run. */
static void load_settings(void)
{
    xc_settings_default(&settings);
    if (xc_nx_exists(SETTINGS_FILE)) {
        char *data = NULL;
        size_t len = 0;
        xc_settings_error serr;
        memset(&serr, 0, sizeof serr);
        xc_err err = read_local(SETTINGS_FILE, XC_SETTINGS_MAX_FILE, &data, &len);
        if (err == XC_OK) {
            err = xc_settings_parse(data, len, &settings, &serr);
        }
        free(data);
        if (err == XC_ERR_SETTINGS) {
            out("%s line %u: %s; using the defaults\n", SETTINGS_FILE, (unsigned)serr.line,
                serr.message);
            snprintf(settings_problem, sizeof settings_problem,
                     "E:\\Apps\\xcat\\settings has a problem on line %u: %s. Games install to "
                     "E:\\Games until it's fixed or deleted.",
                     (unsigned)serr.line, serr.message);
        } else if (err != XC_OK) {
            const char *what = err == XC_ERR_LIMIT ? "larger than 16 KiB" : xc_err_str(err);
            out("%s: %s; using the defaults\n", SETTINGS_FILE, what);
            snprintf(settings_problem, sizeof settings_problem,
                     "E:\\Apps\\xcat\\settings can't be read (%s). Games install to E:\\Games.",
                     what);
        }
    }
    if (!xc_nx_mkdirs(settings.install_dir)) {
        out("%s can't be made; installing to " XC_SETTINGS_DEFAULT_DIR " instead\n",
            settings.install_dir);
        snprintf(settings_problem, sizeof settings_problem,
                 "%s can't be made (is its drive there?), so games install to E:\\Games for "
                 "now. SETTINGS chooses another folder.",
                 settings.install_dir);
        xc_settings_default(&settings);
    }
    out("Games install to %s\n", settings.install_dir);
}

/* Reads D:\ui-script.txt (ui-script.md §2): without one, nothing is played. One that can't be
   played is shown in the library, and the controller alone works, as without one. */
static void load_script(void)
{
    if (!xc_nx_exists(SCRIPT_FILE)) {
        return;
    }
    char *data = NULL;
    size_t len = 0;
    xc_ui_script_error serr;
    memset(&serr, 0, sizeof serr);
    script_data = malloc(sizeof *script_data);
    xc_err err = script_data != NULL ? read_local(SCRIPT_FILE, XC_UI_SCRIPT_MAX_FILE, &data, &len)
                                     : XC_ERR_NOMEM;
    if (err == XC_OK) {
        err = xc_ui_script_parse(data, len, script_data, &serr);
    }
    free(data);
    char why[160] = "";
    if (err == XC_ERR_UI_SCRIPT) {
        snprintf(why, sizeof why, "line %u: %s", (unsigned)serr.line, serr.message);
    } else if (err != XC_OK) {
        snprintf(why, sizeof why, "%s",
                 err == XC_ERR_LIMIT ? "larger than 64 KiB" : xc_err_str(err));
    } else {
        const char *bad = ui_script_check(script_data, why, sizeof why);
        if (bad == NULL) {
            out("UI script %s: %u commands; the controller works too\n", SCRIPT_FILE,
                (unsigned)script_data->count);
            return;
        }
    }
    free(script_data);
    script_data = NULL;
    out("UI script %s not played: %s\n", SCRIPT_FILE, why);
    snprintf(script_problem, sizeof script_problem, "%s isn't played: %s. %s", SCRIPT_FILE, why,
             xc_err_action(XC_ERR_UI_SCRIPT));
}

/* Makes `dir` the install folder (settings.md §6): checks it, makes it, and writes it into
   E:\Apps\xcat\settings. On failure, msg says why for the screen, and nothing changed. */
static bool save_folder(const char *dir, char *msg, size_t size)
{
    const char *why = xc_settings_check_dir(dir);
    if (why != NULL) {
        snprintf(msg, size, "\"%s\" %s.", dir, why);
        return false;
    }
    char d[XC_SETTINGS_DIR_SIZE];
    snprintf(d, sizeof d, "%s", dir);
    if (d[0] >= 'a') {
        d[0] = (char)(d[0] - 'a' + 'A');
    }
    if ((d[0] == 'F' && !has_f) || (d[0] == 'G' && !has_g)) {
        snprintf(msg, size, "This console has no %c: drive.", d[0]);
        return false;
    }
    if (!xc_nx_mkdirs(d)) {
        out("%s can't be made\n", d);
        snprintf(msg, size, "%s can't be made on the drive.", d);
        return false;
    }
    char *old = NULL;
    size_t old_len = 0;
    xc_err err = XC_OK;
    if (xc_nx_exists(SETTINGS_FILE)) {
        err = read_local(SETTINGS_FILE, XC_SETTINGS_MAX_FILE, &old, &old_len);
    }
    char *text = NULL;
    size_t len = 0;
    xc_settings_error serr;
    memset(&serr, 0, sizeof serr);
    if (err == XC_OK) {
        err = xc_settings_set(old, old_len, "install_dir", d, &text, &len, &serr);
    }
    free(old);
    if (err == XC_OK) {
        (void)xc_nx_mkdirs(APPS_DIR);
        err = xc_nx_replace_file(SETTINGS_FILE, text, len);
    }
    free(text);
    if (err == XC_ERR_SETTINGS) {
        out("%s line %u: %s; not changed\n", SETTINGS_FILE, (unsigned)serr.line, serr.message);
        snprintf(msg, size,
                 "E:\\Apps\\xcat\\settings has a problem on line %u: %s. Fix or delete it, then "
                 "try again.",
                 (unsigned)serr.line, serr.message);
        return false;
    }
    if (err != XC_OK) {
        out("%s: %s; not changed\n", SETTINGS_FILE, xc_err_str(err));
        snprintf(msg, size, "E:\\Apps\\xcat\\settings couldn't be written: %s.", xc_err_str(err));
        return false;
    }
    snprintf(settings.install_dir, sizeof settings.install_dir, "%s", d);
    settings_problem[0] = '\0';
    out("Games now install to %s\n", d);
    return true;
}

/* --- Authentication: the map's servers and their logins (SETTINGS) ----------------------- */

#define MAX_HOSTS 32 /* servers listed besides archive.org; more are left out */

static xc_map_host map_hosts[MAX_HOSTS];
static char host_names[MAX_HOSTS + 1][XC_URL_MAX_HOST + 16];
static char host_logins[MAX_HOSTS + 1][XC_CRED_MAX_NAME + 1];
static char host_notes[MAX_HOSTS + 1][64];
static ui_lib_host hosts[MAX_HOSTS + 1];

static bool archive_host(const char *host)
{
    size_t n = strlen(host);
    return strcmp(host, "archive.org") == 0 ||
           (n > 12 && strcmp(host + n - 12, ".archive.org") == 0);
}

/* What the map gets from a server, for its details. */
static void host_note(char *out, size_t size, size_t entries, size_t maps)
{
    if (entries == 0 && maps == 0) {
        snprintf(out, size, "%s", "Not in the map");
    } else if (maps == 0) {
        snprintf(out, size, "%u game%s", (unsigned)entries, entries == 1 ? "" : "s");
    } else {
        snprintf(out, size, "%u game%s, %u map%s", (unsigned)entries, entries == 1 ? "" : "s",
                 (unsigned)maps, maps == 1 ? "" : "s");
    }
}

/* archive.org (and its subdomains), then the map's other servers in the order the map has
   them, each with the credentials section that would go with a request to it (credentials.md
   §6). The strings are copies: the credentials can be reloaded under them. */
static int list_hosts(void)
{
    size_t n = have_map ? xc_map_hosts(&map, map_hosts, MAX_HOSTS) : 0;
    size_t archive_entries = 0;
    size_t archive_maps = 0;
    int k = 1;
    static xc_url u;
    for (size_t i = 0; i < n; i++) {
        const xc_map_host *h = &map_hosts[i];
        if (archive_host(h->host)) {
            archive_entries += h->entries;
            archive_maps += h->maps;
            continue;
        }
        if (h->scheme == XC_URL_SMB) {
            char port[8] = "";
            if (h->port != XC_SMB_PORT) {
                snprintf(port, sizeof port, ":%u", (unsigned)h->port);
            }
            snprintf(host_names[k], sizeof host_names[k], "NAS %s%s", h->host, port);
            const xc_cred_section *nas_sec =
                have_creds ? xc_cred_for_smb(&creds, h->host, h->port) : NULL;
            if (nas_sec != NULL) {
                snprintf(host_logins[k], sizeof host_logins[k], "%s", nas_sec->name);
            }
            snprintf(host_notes[k], sizeof host_notes[k], "%u game%s, %u folder%s",
                     (unsigned)h->entries, h->entries == 1 ? "" : "s", (unsigned)h->folders,
                     h->folders == 1 ? "" : "s");
            hosts[k] = (ui_lib_host){
                host_names[k], false, nas_sec != NULL ? host_logins[k] : NULL, host_notes[k],
                nas_sec != NULL ? "Its NT hash is kept, never the password. SETTINGS > "
                                  "NAS logs in again."
                                : "No login: it's asked as a guest. SETTINGS > NAS logs "
                                  "in with a user name."};
            k++;
            continue;
        }
        bool https = h->scheme == XC_URL_HTTPS;
        bool plain_port = h->port == (https ? 443 : 80);
        char port[8] = "";
        if (!plain_port) {
            snprintf(port, sizeof port, ":%u", (unsigned)h->port);
        }
        snprintf(host_names[k], sizeof host_names[k], "%s%s%s", https ? "" : "http://", h->host,
                 port);
        memset(&u, 0, sizeof u);
        u.scheme = h->scheme;
        u.port = h->port;
        u.is_ipv4 = h->is_ipv4;
        memcpy(u.host, h->host, sizeof u.host);
        u.target[0] = '/';
        const xc_cred_section *sec = have_creds ? xc_cred_for_url(&creds, &u) : NULL;
        if (sec != NULL) {
            snprintf(host_logins[k], sizeof host_logins[k], "%s", sec->name);
        }
        host_note(host_notes[k], sizeof host_notes[k], h->entries, h->maps);
        hosts[k] = (ui_lib_host){
            host_names[k], false, sec != NULL ? host_logins[k] : NULL, host_notes[k],
            https || sec != NULL
                ? NULL
                : "Plain http: a login goes only to a LAN address (10.x, 172.16-31.x, 192.168.x) "
                  "whose section says allow_http = yes."};
        k++;
    }
    host_note(host_notes[0], sizeof host_notes[0], archive_entries, archive_maps);
    hosts[0] = (ui_lib_host){"archive.org", true, NULL, host_notes[0], NULL};
    return k;
}

/* After the credentials changed: what Authentication shows. */
static void refresh_logins(ui_library *lib)
{
    ui_library_set_account(lib, logged_in());
    ui_library_set_hosts(lib, hosts, list_hosts());
}

/* UI_LIB_FORGET: removes the section that logs in to the highlighted server. */
static void forget(ui_library *lib)
{
    const ui_lib_host *h = &hosts[ui_library_host(lib)];
    char name[XC_CRED_MAX_NAME + 1];
    snprintf(name, sizeof name, "%s", h->login != NULL ? h->login : "");
    char msg[256] = "No login is listed for this server.";
    if (name[0] == '\0' || !write_section(name, NULL, msg, sizeof msg)) {
        ui_library_tell(lib, "NOT REMOVED", msg);
        return;
    }
    out("Login [%s] removed from %s\n", name, CRED_FILE);
    reload_credentials();
    refresh_logins(lib);
    snprintf(msg, sizeof msg,
             "The login [%s] is removed from this console. Every other section of the "
             "credentials file is as it was.",
             name);
    ui_library_tell(lib, "LOGIN REMOVED", msg);
}

static char login_body[XC_LOGIN_MAX_BODY];
static xc_login_result login_result;
static char login_section[2 * XC_LOGIN_MAX_COOKIE + 256];

/* The email and password typed in SETTINGS: one POST over HTTPS, then the session into the
   credentials file. The password is wiped as soon as the body is made, and the body once
   it's sent; neither is logged. True if logged in; with `resuming` (an install waits for the
   login) there's no LOGGED IN message. */
static bool login(ui_library *lib, bool resuming)
{
    size_t body_len = 0;
    xc_err err = xc_login_body(lib->email, lib->password, login_body, sizeof login_body, &body_len);
    ui_library_forget_password(lib);
    if (err != XC_OK) {
        xc_login_wipe(login_body, sizeof login_body);
        ui_library_tell(lib, "LOG IN",
                        "The email or the password is too long, or has a character it can't "
                        "have.");
        return false;
    }
    out("Logging in to archive.org\n");
    if (!network() || !https_ready()) {
        xc_login_wipe(login_body, sizeof login_body);
        ui_library_tell(lib, "LOG IN",
                        "No network, or HTTPS couldn't start. The reason is in "
                        "E:\\xcat\\install.log.");
        return false;
    }
    mem_ctx m = {NULL, 0, 0};
    err = xc_http_post(&client, XC_LOGIN_URL, XC_LOGIN_CONTENT_TYPE, login_body, body_len,
                       XC_LOGIN_MAX_RESPONSE, to_mem, &m);
    xc_login_wipe(login_body, sizeof login_body);
    xc_login_wipe(recv_buf, RECV_BUF_SIZE); /* the answer, with the session, came through it */
    bool answered = err == XC_OK;
    if (answered) {
        err = xc_login_parse(m.buf, m.len, &login_result);
    }
    if (m.buf != NULL) {
        xc_login_wipe(m.buf, m.cap);
        free(m.buf);
    }
    char msg[256];
    if (err != XC_OK && !answered) {
        out("Login failed: %s (HTTP %d)\n", xc_err_str(err), client.last_status);
        (void)snprintf(msg, sizeof msg,
                       "The login didn't get through: %s. Check the network and try again.",
                       xc_err_str(err));
        ui_library_tell(lib, "LOG IN", msg);
        return false;
    }
    if (err != XC_OK) {
        /* login.md §3: the problem names no value, so it can be logged and shown */
        out("Login failed: archive.org's answer can't be read: %s\n", login_result.problem);
        (void)snprintf(msg, sizeof msg,
                       "archive.org answered, but the answer can't be read (%s). Nothing was "
                       "saved.",
                       login_result.problem);
        ui_library_tell(lib, "LOG IN", msg);
        return false;
    }
    if (login_result.outcome != XC_LOGIN_OK) {
        out("Login refused: %s\n",
            login_result.reason[0] != '\0' ? login_result.reason : "no reason given");
        if (login_result.outcome == XC_LOGIN_NO_ACCOUNT) {
            (void)snprintf(msg, sizeof msg, "archive.org has no account with that email address.");
        } else if (login_result.outcome == XC_LOGIN_BAD_PASSWORD) {
            (void)snprintf(msg, sizeof msg, "The password is wrong. Try again.");
        } else {
            (void)snprintf(msg, sizeof msg, "archive.org refused the login: %s.",
                           login_result.reason[0] != '\0' ? login_result.reason
                                                          : "no reason given");
        }
        ui_library_tell(lib, "LOG IN", msg);
        return false;
    }
    err = xc_login_section(&login_result, login_section, sizeof login_section, NULL);
    xc_login_wipe(&login_result, sizeof login_result);
    bool ok = err == XC_OK && write_section(XC_LOGIN_SECTION, login_section, msg, sizeof msg);
    xc_login_wipe(login_section, sizeof login_section);
    if (!ok) {
        ui_library_tell(lib, "LOG IN", err == XC_OK ? msg : "The session can't be saved.");
        return false;
    }
    out("Logged in to archive.org: [%s] written to %s\n", XC_LOGIN_SECTION, CRED_FILE);
    reload_credentials();
    refresh_logins(lib);
    if (!resuming) { /* else the install it was for carries on at once */
        ui_library_tell(lib, "LOGGED IN",
                        "You're logged in to archive.org. The session is saved in "
                        "E:\\Apps\\xcat\\credentials; the password isn't.");
    }
    return true;
}

static void logout(ui_library *lib)
{
    char msg[256];
    if (!write_section(XC_LOGIN_SECTION, NULL, msg, sizeof msg)) {
        ui_library_tell(lib, "LOG OUT", msg);
        return;
    }
    out("Logged out of archive.org: [%s] removed from %s\n", XC_LOGIN_SECTION, CRED_FILE);
    reload_credentials();
    refresh_logins(lib);
    ui_library_tell(lib, "LOGGED OUT",
                    "This console is logged out of archive.org. The session itself ends when "
                    "you log out on archive.org.");
}

/* --- the catalog (docs/formats/catalog.md) ------------------------------------------------ */

static xc_catalog cat; /* zeroed, so every lookup finds nothing, if there is none */

/* Loads E:\Apps\xcat\catalog.bin. Without one, or with a bad one, games show their file
   names and every map entry is "not in catalog" (catalog.md §7). Its bytes stay loaded. */
static void load_catalog(void)
{
    if (!xc_nx_exists(CATALOG_FILE)) {
        out("No catalog (" CATALOG_FILE "): games show their file names\n");
        return;
    }
    char *data = NULL;
    size_t len = 0;
    xc_err err = read_local(CATALOG_FILE, XC_CATALOG_MAX_SIZE, &data, &len);
    if (err == XC_OK) {
        err = xc_catalog_open(&cat, data, len);
    }
    if (err != XC_OK) {
        free(data);
        out(CATALOG_FILE ": %s; games show their file names\n",
            err == XC_ERR_LIMIT ? "larger than 8 MiB" : xc_err_str(err));
        return;
    }
    out("Catalog: %u games, %u releases (%s)\n", (unsigned)cat.game_count,
        (unsigned)cat.release_count, cat.snapshot);
}

/* --- installing --------------------------------------------------------------------------- */

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = s[len - n + i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != suffix[i]) {
            return false;
        }
    }
    return true;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* "dir/Halo%20(USA).xiso.iso?x" -> "Halo (USA)"; likewise "Halo (USA).zip" */
/* True if a source's path has a folder named like where apps are kept ("Apps",
   "Applications", "Homebrew", "Emulators", "Dashboards", any case): installing it offers
   E:\\Apps first. */
static bool looks_like_app(const char *source)
{
    static const char *const names[] = {"apps", "applications", "homebrew", "emulators",
                                        "dashboards"};
    for (const char *p = source; *p != '\0'; p++) {
        if (*p != '/' && *p != '\\') {
            continue;
        }
        const char *name = p + 1;
        size_t n = strcspn(name, "/\\");
        if (name[n] == '\0') {
            break; /* the file itself */
        }
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
            size_t m = strlen(names[k]);
            bool same = n == m;
            for (size_t c = 0; same && c < m; c++) {
                same = (name[c] | 0x20) == names[k][c];
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

static void title_from(const char *name, char *out_title, size_t size)
{
    const char *end = name + strcspn(name, "?#");
    while (end > name && end[-1] == '/') {
        end--; /* a game folder's address (smb.md §3.1): named after the folder */
    }
    const char *p = end;
    while (p > name && p[-1] != '/' && p[-1] != '\\') {
        p--;
    }
    size_t n = 0;
    while (p < end && n + 1 < size) {
        int hi = p[0] == '%' && end - p >= 3 ? hexval(p[1]) : -1;
        int lo = hi >= 0 ? hexval(p[2]) : -1;
        out_title[n++] = lo >= 0 ? (char)(hi * 16 + lo) : *p;
        p += lo >= 0 ? 3 : 1;
    }
    if (ends_with_ci(out_title, n, ".zip")) {
        n -= 4;
    } else if (ends_with_ci(out_title, n, ".7z")) {
        n -= 3;
    }
    if (ends_with_ci(out_title, n, ".iso")) {
        n -= 4;
    }
    if (ends_with_ci(out_title, n, ".xiso")) {
        n -= 5;
    }
    out_title[n] = '\0';
}

typedef struct ui {
    int phase;
    uint64_t next_ms;
    uint64_t phase_ms;    /* when the current phase began */
    uint64_t phase_bytes; /* bytes_done when it began: a resumed install starts above 0 */
    uint64_t write_ms;    /* how long writing took, once it's over */
    uint64_t write_bytes; /* bytes written in this run */
    uint64_t verify_ms;
    uint64_t verify_bytes;
} ui;

/* Decimal MB/s in tenths. */
static uint64_t mbps_tenths(uint64_t bytes, uint64_t ms)
{
    return ms > 0 ? bytes / ms / 100U : 0;
}

static void end_phase(ui *u, uint64_t now, uint64_t bytes_done)
{
    if (u->phase == XC_INSTALL_WRITING) {
        u->write_ms = now - u->phase_ms;
        u->write_bytes = bytes_done - u->phase_bytes;
    } else if (u->phase == XC_INSTALL_VERIFYING) {
        u->verify_ms = now - u->phase_ms;
        u->verify_bytes = bytes_done - u->phase_bytes;
    }
}

static uint64_t last_bytes_done;

static bool on_progress(void *ctx, const xc_install_progress *p)
{
    ui *u = ctx;
    uint64_t now = now_ms();
    bool moved = p->bytes_done != last_bytes_done;
    if ((int)p->phase != u->phase) {
        end_phase(u, now, last_bytes_done);
        u->phase = (int)p->phase;
        u->next_ms = 0;
        u->phase_ms = now;
        u->phase_bytes = p->bytes_done;
        out("  %s\n", p->phase == XC_INSTALL_WRITING     ? "writing files"
                      : p->phase == XC_INSTALL_VERIFYING ? "verifying (reading every file back)"
                                                         : "reading the directory tables");
    }
    last_bytes_done = p->bytes_done;
    if (now >= u->next_ms && p->bytes_total > 0) {
        u->next_ms = now + REPORT_EVERY_MS;
        char speed[24] = "-";
        if (now > u->phase_ms + 1000U) {
            /* this run's bytes only: what a resume found on the disk isn't speed */
            tenths(speed, sizeof speed,
                   mbps_tenths(p->bytes_done - u->phase_bytes, now - u->phase_ms));
        }
        /* The file being worked on (1-based), not the number finished: one big file would
           otherwise read "0 of N" for most of an install. */
        size_t current = p->files_done < p->file_count ? p->files_done + 1 : p->file_count;
        out("  %4llu s  %llu of %llu MiB, file %u of %u, %s MB/s\n",
            (unsigned long long)((now - run_start_ms) / 1000U),
            (unsigned long long)(p->bytes_done >> 20U), (unsigned long long)(p->bytes_total >> 20U),
            (unsigned)current, (unsigned)p->file_count, speed);
    }
    /* This phase's speed, once it has run a few seconds. */
    uint64_t ran = now - u->phase_ms;
    EnterCriticalSection(&view_lock);
    view.phase = p->phase == XC_INSTALL_WRITING     ? UI_INST_WRITING
                 : p->phase == XC_INSTALL_VERIFYING ? UI_INST_VERIFYING
                                                    : UI_INST_CHECKING;
    view.done = p->bytes_done;
    view.total = p->bytes_total;
    view.files = (uint32_t)p->file_count;
    view.file = (uint32_t)(p->files_done < p->file_count ? p->files_done + 1 : p->file_count);
    view.speed = ran > 3000U ? (uint32_t)mbps_tenths(p->bytes_done - u->phase_bytes, ran) : 0;
    if (!stop_asked && moved) {
        view.status[0] = '\0'; /* data flows: the retries and steps are over */
    }
    LeaveCriticalSection(&view_lock);
    return carry_on();
}

static const char *problem_text(const xc_fatx_problem *p)
{
    switch (p->kind) {
    case XC_FATX_PROBLEM_NAME:
        return p->name == XC_FATX_NAME_TOO_LONG ? "name over 42 characters" : "invalid name";
    case XC_FATX_PROBLEM_PATH_TOO_LONG:
        return "path over 250 characters";
    case XC_FATX_PROBLEM_TOO_MANY_ENTRIES:
        return "over 4096 entries in one folder";
    case XC_FATX_PROBLEM_FILE_TOO_BIG:
        return "file over 4 GiB - 16 KiB (FATX can't grow a file further)";
    }
    return "problem";
}

static xc_install_result result;

/* journal.md §7: the whole-image hashes (compared from M6, when archive.org gives them). */
static void log_image_hash(const xc_install_result *r)
{
    if (r->hash == XC_INSTALL_HASH_NONE) {
        out("  image not hashed: %s\n", r->hash_note != NULL ? r->hash_note : "?");
        return;
    }
    char md5[2 * XC_MD5_BYTES + 1];
    char sha1[2 * XC_SHA1_BYTES + 1];
    xc_hex_lower(r->md5, XC_MD5_BYTES, md5);
    xc_hex_lower(r->sha1, XC_SHA1_BYTES, sha1);
    out("  image MD5 %s\n  image SHA-1 %s%s\n", md5, sha1,
        r->hash_restarted ? " (hashed from the start again)" : "");
}

/* ADR 0009 amendment 1: how a zip install went, and what a Deflate one cost. */
static void log_zip(const xc_install_result *r, bool sevenz)
{
    if (r->archive_folder) {
        out("  from the archive's game folder: %s\n",
            strcmp(r->zip_member, "/") == 0 ? "its top" : r->zip_member);
        return;
    }
    if (sevenz) {
        out("  from the 7z: %s%s\n", r->zip_member, r->passes == 0 ? ", stored" : "");
    } else {
        out("  from the zip: %s, %s\n", r->zip_member, r->zip_method == 0 ? "stored" : "Deflate");
    }
    if (r->passes == 0) {
        return;
    }
    out("  %u pass%s, %llu MiB read again, %u file(s) from a %llu MiB spool\n", r->passes,
        r->passes == 1 ? "" : "es", (unsigned long long)(r->reread_bytes >> 20U),
        (unsigned)r->files_from_spool, (unsigned long long)(r->spool_peak >> 20U));
    if (r->zip_resumed_at > 0) {
        out("  continued %llu MiB into the image\n",
            (unsigned long long)(r->zip_resumed_at >> 20U));
    }
}

/* After an install: does default.xbe's title ID agree with the catalog's release? A
   disagreement means the catalog's serial-derived title ID is wrong for this disc (fixed in
   tools/catalog/overrides.toml [title_id]), or the image isn't that release. */
static void check_catalog_title(size_t release, uint32_t title_id)
{
    xc_catalog_release r;
    if (!xc_catalog_release_at(&cat, release, &r)) {
        return;
    }
    if (r.title_id == title_id) {
        out("  matches the catalog: %08lX@%s, %s\n", (unsigned long)r.title_id, r.variant, r.name);
    } else {
        out("  the catalog expected %08lX for %s: please report it\n", (unsigned long)r.title_id,
            r.name);
    }
}

/* What the last install attempt came to, for the install screen's closing dialog. */
static char why[400];
static xc_err why_err; /* XC_OK if it installed */
static bool why_login; /* it was refused a login that logging in again can fix (refused()) */

/* Adds a sentence to `why`. */
__attribute__((format(printf, 1, 2))) static void explain(const char *fmt, ...)
{
    size_t n = strlen(why);
    if (n > 0 && n + 1 < sizeof why) {
        why[n++] = ' ';
        why[n] = '\0';
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(why + n, sizeof why - n, fmt, ap);
    va_end(ap);
}

/* The install failed with err: logged, and the start of `why`. */
static void fail(xc_err err)
{
    out("  NOT INSTALLED: %s\n", xc_err_str(err));
    why_err = err;
    why_login = false;
    why[0] = '\0';
    if (err == XC_ERR_CANCELLED) {
        explain("Stopped. What's written is kept: install the game again to carry on from "
                "there.");
        return;
    }
    explain("%s.", xc_err_str(err));
    if (err != XC_ERR_HTTP_DENIED) { /* refused() says whose login, and what to do */
        out("  What to do: %s\n", xc_err_action(err));
        explain("%s", xc_err_action(err));
    }
}

/* A detail of a failure: logged, and added to `why`. */
__attribute__((format(printf, 1, 2))) static void detail(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out("   %s\n", line);
    explain("%s.", line);
}

/* After a 401/403 (ADR 0010 decision 4): whose login was refused, in `why`, and whether
   logging in to archive.org again can help, which sets why_login. Nothing is retried. */
static void refused(void)
{
    const xc_cred_section *ia = have_creds ? xc_cred_section_named(&creds, XC_LOGIN_SECTION) : NULL;
    if (client.last_section != NULL) {
        detail("The login for [%s] is missing or expired", client.last_section);
        why_login = ia != NULL && client.last_section == ia->name;
    } else if (ia == NULL && archive_host(res.effective.host)) {
        detail("You aren't logged in to archive.org");
        why_login = true;
    } else if (!have_creds) {
        detail("No login was sent: no credentials were loaded from %s", CRED_FILE);
    } else {
        detail("HTTP %d", client.last_status);
    }
    if (why_login) {
        explain("Log in again, and the install carries on where it stopped.");
    } else {
        out("  What to do: %s\n", xc_err_action(XC_ERR_HTTP_DENIED));
        explain("%s", xc_err_action(XC_ERR_HTTP_DENIED));
    }
}

/* Installs one image; true if it ended installed and verified. `listing` is the map entry
   when an archive.org listing describes the file (archiveorg.md §8), else NULL. `release`
   is the catalog's release for it, or XC_CATALOG_NONE. */
/* sevenz.md §4: the largest LZMA dictionary a 7z may need on this console now: the largest
   block the heap gives, found by halving, less 4 MiB for the install's buffers and the network.
   Never 0, which would mean no bound. */
static size_t free_for_dictionary(void)
{
    size_t lo = 0;
    size_t hi = (size_t)96 << 20;
    while (hi - lo > ((size_t)1 << 20)) {
        size_t mid = lo + (hi - lo) / 2U;
        void *p = malloc(mid);
        if (p != NULL) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    size_t margin = (size_t)4 << 20;
    return lo > margin ? lo - margin : 1U;
}

/* game_folder: src is a game folder's image (folder-image.md), with no image hashes. */
static bool install(const xc_source *src, const char *name, const char *etag,
                    const char *last_modified, const xc_map_entry *listing, size_t release,
                    bool game_folder)
{
    char title[128];
    title_from(name, title, sizeof title);
    xc_install_io io;
    xc_nx_install_io(&io);
    ui u;
    memset(&u, 0, sizeof u);
    u.phase = -1;
    last_bytes_done = 0;
    xc_install_req req;
    memset(&req, 0, sizeof req);
    req.src = src;
    req.etag = etag;
    req.last_modified = last_modified;
    req.source_name = name;
    const char *games_dir = dest_dir != NULL ? dest_dir : settings.install_dir;
    req.games_dir = games_dir;
    req.games_dir_len = strlen(games_dir) - 2U; /* without "E:" */
    req.title = title;
    req.io = &io;
    req.buf = io_buf;
    req.buf_size = IO_BUF_SIZE;
    char root[4] = {games_dir[0], ':', '\\', '\0'};
    req.cluster_size = xc_nx_cluster_size(root);
    if (req.cluster_size == 0) {
        req.cluster_size = CLUSTER;
    }
    req.progress = on_progress;
    req.progress_ctx = &u;
    req.zip = xc_zip_is_zip_name(name);
    req.sevenz = xc_7z_is_7z_name(name);
    if (req.sevenz) {
        req.max_dict = free_for_dictionary();
        out("  7z: room for a %llu MiB dictionary\n", (unsigned long long)(req.max_dict >> 20U));
    }
    req.no_hash = game_folder;
    if (listing != NULL && listing->name != NULL && !game_folder) {
        req.expect_size = listing->size;
        /* A zip's hashes are the zip's, not the image's: its member's CRC-32 covers that. */
        req.expect_md5 = listing->has_md5 && !req.zip ? listing->md5 : NULL;
        req.expect_sha1 = listing->has_sha1 && !req.zip ? listing->sha1 : NULL;
        out("  %s says: %llu bytes%s\n", xc_smb_is_url(name) ? "the NAS's folder" : "archive.org",
            (unsigned long long)listing->size,
            req.expect_md5 != NULL || req.expect_sha1 != NULL ? ", MD5/SHA-1 compared" : "");
    }
    xc_err err = xc_install(&req, &result);
    xc_nx_spool_close();
    end_phase(&u, now_ms(), last_bytes_done);
    if (result.other_folder_reason != NULL) {
        out("  (not the existing folder of that name: %s)\n", result.other_folder_reason);
    }
    if (err == XC_OK) {
        char wspeed[24];
        char vspeed[24];
        tenths(wspeed, sizeof wspeed, mbps_tenths(u.write_bytes, u.write_ms));
        tenths(vspeed, sizeof vspeed, mbps_tenths(u.verify_bytes, u.verify_ms));
        out("  OK: %u files in %s, verified%s\n", (unsigned)result.file_count, result.folder,
            result.resumed ? " (resumed)" : "");
        out("  %u written, %u kept from an earlier attempt\n", (unsigned)result.files_written,
            (unsigned)result.files_skipped);
        out("  wrote %llu MiB in %llu s (%s MB/s); read back %llu MiB in %llu s (%s MB/s)\n",
            (unsigned long long)(u.write_bytes >> 20U), (unsigned long long)(u.write_ms / 1000U),
            wspeed, (unsigned long long)(u.verify_bytes >> 20U),
            (unsigned long long)(u.verify_ms / 1000U), vspeed);
        if (result.files_continued > 0) {
            out("  %u file(s) continued from a checkpoint (%llu MiB were already there)\n",
                (unsigned)result.files_continued,
                (unsigned long long)(result.bytes_continued >> 20U));
        }
        if (req.zip || req.sevenz) {
            log_zip(&result, req.sevenz);
        }
        log_image_hash(&result);
        if (result.has_title_id) {
            out("  title ID %08lX, \"%s\" (from default.xbe)\n", (unsigned long)result.title_id,
                result.title_name);
            check_catalog_title(release, result.title_id);
        } else {
            out("  no title ID: %s\n", result.title_note != NULL ? result.title_note : "?");
        }
        why_err = XC_OK;
        why[0] = '\0';
        explain("In %s, every file read back and checked.", result.folder);
        if (result.resumed) {
            explain("It carried on from where an earlier attempt stopped.");
        }
        return true;
    }
    fail(err);
    if (err == XC_ERR_SOURCE_SIZE) {
        detail("archive.org's listing says %llu bytes; the server's file has %llu",
               (unsigned long long)req.expect_size, (unsigned long long)src->size);
    }
    if (result.folder_why[0] != '\0') {
        detail("In the archive: %s", result.folder_why);
    } else if (err == XC_ERR_ZIP_UNSUPPORTED && result.zip_method != 0 && result.zip_method != 8) {
        const char *method = xc_zip_method_name(result.zip_method);
        detail("%s in the zip uses %s (method %u); only stored and Deflate can be read",
               result.zip_member[0] != '\0' ? result.zip_member : "the image",
               method != NULL ? method : "an unknown method", (unsigned)result.zip_method);
    }
    if (err == XC_ERR_IMAGE_HASH) {
        log_image_hash(&result);
    }
    if (result.current_file[0] != '\0') {
        detail("At %s", result.current_file);
    }
    const char *op = NULL;
    unsigned long code = 0;
    xc_nx_last_io_error(&op, &code);
    if (err == XC_ERR_IO && op != NULL) {
        detail("The disk %s failed (error 0x%08lX)", op, code);
    }
    if (err == XC_ERR_NOT_INSTALLABLE) {
        for (size_t i = 0; i < result.problem_count && i < XC_INSTALL_MAX_PROBLEMS; i++) {
            detail("%.48s: %s", result.problems[i].path, problem_text(&result.problems[i]));
        }
    } else if (err == XC_ERR_NO_SPACE) {
        char need[24];
        char free_now[24];
        ui_format_size(need, sizeof need, result.space_needed);
        ui_format_size(free_now, sizeof free_now, result.space_free);
        detail("It needs %s; %s is free", need, free_now);
    } else if (err == XC_ERR_VERIFY) {
        detail("%s was removed; install again to rewrite it", result.bad_file);
    } else if (err == XC_ERR_HTTP_DENIED) {
        refused();
    } else if (client.last_status != 0 &&
               (err == XC_ERR_HTTP_STATUS || err == XC_ERR_HTTP_NOT_FOUND)) {
        detail("HTTP %d", client.last_status);
    }
    if (result.files_written + result.files_skipped > 0) {
        detail("%u files are done in %s; install again to carry on",
               (unsigned)(result.files_written + result.files_skipped), result.folder);
    }
    return false;
}

/* An image on a NAS's shared folder (smb.md, ADR 0018), with the login the credentials file
   gives the NAS. */
static bool install_smb(const xc_map_entry *e, size_t release)
{
    const char *url = e->url;
    if (!network()) {
        fail(XC_ERR_NET_CONNECT);
        why[0] = '\0';
        explain("No network: check the cable and the dashboard's network settings.");
        return false;
    }
    static xc_smb_url u;
    xc_err err = xc_smb_url_parse(url, strlen(url), &u);
    if (err != XC_OK) {
        fail(err);
        return false;
    }
    size_t url_len = strlen(url);
    bool folder = url_len > 0 && url[url_len - 1U] == '/'; /* a game folder (smb.md §3.1) */
    say("Connecting to the NAS...\n");
    xc_smb_conn *c = NULL;
    char why_smb[XC_SMB_DETAIL_MAX];
    err = xc_smb_connect_url(&u, have_creds ? &creds : NULL, &c, why_smb, sizeof why_smb);
    xc_smb_file *f = NULL;
    xc_smb_game *g = NULL;
    xc_source src;
    if (err == XC_OK && folder) {
        say("Listing the game folder on the NAS...\n");
        err = xc_smb_open_game(c, u.path, &g, &src, why_smb, sizeof why_smb);
    } else if (err == XC_OK) {
        say("Opening the image on the NAS...\n");
        err = xc_smb_open(c, u.path, &f, &src);
        if (err != XC_OK) {
            snprintf(why_smb, sizeof why_smb, "%s", xc_smb_detail(c));
        }
    }
    if (err != XC_OK) {
        fail(err);
        if (why_smb[0] != '\0') {
            detail(err == XC_ERR_GAME_FOLDER ? "In the game folder: %s" : "The NAS said: %s",
                   why_smb);
        }
        xc_smb_disconnect(c);
        return false;
    }
    if (g != NULL) {
        out("  game folder: %u files, %llu bytes\n", (unsigned)xc_smb_game_files(g),
            (unsigned long long)xc_smb_game_size(g));
    }
    bool ok = install(&src, url, g != NULL ? xc_smb_game_validator(g) : NULL, NULL, e, release,
                      g != NULL);
    out("  SMB: %llu bytes read, %u reconnection(s)\n",
        (unsigned long long)(g != NULL ? xc_smb_game_bytes(g) : xc_smb_file_bytes(f)),
        g != NULL ? xc_smb_game_reconnects(g) : xc_smb_file_reconnects(f));
    if (!ok && g != NULL && xc_smb_game_detail(g)[0] != '\0') {
        out("  the NAS, reading %s\n", xc_smb_game_detail(g));
    } else if (!ok && xc_smb_detail(c)[0] != '\0') {
        out("  the NAS said: %s\n", xc_smb_detail(c));
    }
    xc_smb_close_game(g);
    xc_smb_close(f);
    xc_smb_disconnect(c);
    return ok;
}

static bool install_url(const xc_map_entry *e, size_t release)
{
    const char *url = e->url;
    if (xc_smb_is_url(url)) {
        out("\n%s\n", url);
        return install_smb(e, release);
    }
    out("\n%s\n", url);
    if (!network()) {
        fail(XC_ERR_NET_CONNECT);
        why[0] = '\0';
        explain("No network: check the cable and the dashboard's network settings.");
        return false;
    }
    if (is_https(url) && !https_ready()) {
        fail(https_err);
        explain("HTTPS can't be used, so this https:// entry can't be installed.");
        return false;
    }
    say("Opening the file on the server...\n");
    static xc_http_source hs;
    xc_source src;
    xc_err err = xc_http_resource_init(&res, url, strlen(url));
    if (err == XC_OK) {
        err = xc_http_source_open(&src, &hs, &client, &res);
    }
    if (err == XC_ERR_UNSUPPORTED && !https_tried) {
        /* An http URL that redirected to https: set up TLS and try again. */
        err = https_ready() ? xc_http_source_open(&src, &hs, &client, &res) : https_err;
    }
    if (err != XC_OK) {
        fail(err);
        if (err == XC_ERR_HTTP_DENIED) {
            refused();
        } else if (client.last_status != 0) {
            detail("HTTP %d", client.last_status);
        }
        return false;
    }
    bool ok = install(&src, url, res.etag[0] != '\0' ? res.etag : NULL,
                      res.last_modified[0] != '\0' ? res.last_modified : NULL, e, release, false);
    if (is_https(url) && tls != NULL) {
        /* After redirects: the server the data came from (ADR 0005 asks to watch the suite). */
        xc_tls_info info;
        xc_tls_last_info(tls, &info);
        if (info.suite_name != NULL) {
            out("  TLS %u.%u, %s\n", (info.version >> 8) - 2U, (info.version & 0xFFU) - 1U,
                info.suite_name);
        }
    }
    xc_http_client_close(&client);
    return ok;
}

static bool install_file(const char *path, size_t release)
{
    xc_nx_file f = {NULL};
    xc_source src;
    out("\n%s\n", path);
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err != XC_OK) {
        fail(err);
        detail("Can't read %s", path);
        return false;
    }
    bool ok = install(&src, path, NULL, NULL, NULL, release, false);
    xc_nx_file_close(&f);
    return ok;
}

/* If there is nothing to install, copies the bundled samples to E:\xcat\isos (M2). */
static void seed_samples(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(SAMPLE_DIR "\\*.iso", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        out("Nothing to install: put a map in " MAP_FILE " or images in " ISO_DIR "\n");
        return;
    }
    do {
        char from[XC_NX_PATH_MAX];
        char to[XC_NX_PATH_MAX];
        snprintf(from, sizeof from, SAMPLE_DIR "\\%s", fd.cFileName);
        snprintf(to, sizeof to, ISO_DIR "\\%s", fd.cFileName);
        out("Copying the bundled test image %s to the HDD\n", fd.cFileName);
        if (!CopyFileA(from, to, TRUE)) {
            out("  copy failed (error %lu)\n", (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* The images and zips in E:\xcat\isos. */
static int list_local(char names[MAX_LOCAL][XC_NX_PATH_MAX])
{
    static const char *const patterns[] = {ISO_DIR "\\*.iso", ISO_DIR "\\*.zip", ISO_DIR "\\*.7z"};
    int count = 0;
    for (size_t k = 0; k < sizeof patterns / sizeof patterns[0]; k++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(patterns[k], &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < MAX_LOCAL) {
                snprintf(names[count++], XC_NX_PATH_MAX, ISO_DIR "\\%s", fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return count;
}

/* --- the library: the map is a catalog, nothing installs unasked ------------------------ */

#define LABEL_MAX 128

typedef struct game {
    char name[LABEL_MAX];                /* the catalog's title, else the file or listing name */
    char key[40];                        /* the title ID and variant, from the map or the catalog */
    char facts[96];                      /* year, region, size */
    char about[512];                     /* who made it, the release, where it comes from */
    char note[400];                      /* what the last install attempt in this run came to */
    char installed[XC_INSTALL_MAX_PATH]; /* its folder, if it's installed; "" if not */
    char partial[XC_INSTALL_MAX_PATH];   /* the folder of an install that isn't finished, or "" */
    char nas[XC_SMB_MAX_HOST + 1];       /* the NAS it's on, or "" */
    uint64_t size;                       /* from the listing; 0 if unknown */
    const xc_map_entry *entry;           /* a map entry, or */
    const char *path;                    /* a file in E:\xcat\isos */
    size_t release;                      /* the catalog's release, or XC_CATALOG_NONE */
} game;

/* The catalog's release for a map entry or local file (map.md §4.1, §5.2): by title ID and
   variant (the first release of the title ID for its default entry), else by name: an
   archive.org file's, or a local file's without its extension. */
static size_t find_release(const xc_map_entry *e, const char *name)
{
    if (e != NULL && e->name == NULL) {
        size_t count = 0;
        return e->variant[0] != '\0' ? xc_catalog_find_release(&cat, e->title_id, e->variant)
                                     : xc_catalog_find_title(&cat, e->title_id, &count);
    }
    return xc_catalog_find_name(&cat, name);
}

/* Appends `part` to s (of `size`), after " · " if s isn't empty; nothing if part is empty.
   (A middle dot, not a bar: "2005 · PAL · 4.03 GB" fits the details' width.) */
static void join(char *s, size_t size, const char *part)
{
    size_t n = strlen(s);
    if (part[0] != '\0' && n < size) {
        (void)snprintf(s + n, size - n, "%s%s", n > 0 ? " \xC2\xB7 " : "", part);
    }
}

/* Where a game comes from, for its details: the server's name, or the folder. */
static void source_text(const game *g, char *out_text, size_t size)
{
    if (g->entry == NULL) {
        (void)snprintf(out_text, size, "From " ISO_DIR " on this console.");
        return;
    }
    const char *host = strstr(g->entry->url, "://");
    host = host != NULL ? host + 3 : g->entry->url;
    if (xc_smb_is_url(g->entry->url)) {
        /* "From the NAS 192.168.1.50: Games > Xbox > Halo (USA)": the folder it's in, or the
           game folder itself, decoded, with > between the names. */
        size_t n = strlen(host);
        bool folder = n > 0 && host[n - 1U] == '/';
        const char *path = host + strcspn(host, "/");
        const char *end = host + n - (folder ? 1U : 0U);
        if (!folder) {
            while (end > path && end[-1] != '/') {
                end--; /* the image's folder: without its file name */
            }
            end -= end > path ? 1 : 0;
        }
        char where[160];
        size_t w = 0;
        for (const char *p = path + (*path == '/' ? 1 : 0); p < end && w + 4 < sizeof where;) {
            int hi = p[0] == '%' && end - p >= 3 ? hexval(p[1]) : -1;
            int lo = hi >= 0 ? hexval(p[2]) : -1;
            if (lo >= 0) {
                where[w++] = (char)(hi * 16 + lo);
                p += 3;
            } else if (*p == '/') {
                memcpy(where + w, " > ", 3);
                w += 3;
                p++;
            } else {
                where[w++] = *p++;
            }
        }
        where[w] = '\0';
        (void)snprintf(out_text, size, "From the NAS %.*s: %s%s.", (int)strcspn(host, "/:"), host,
                       where, folder ? ", an extracted game folder" : "");
        return;
    }
    (void)snprintf(out_text, size, "From %.*s%s.", (int)strcspn(host, "/:?#"), host,
                   g->entry->name != NULL ? ", an archive.org listing" : "");
}

/* Names and describes a game from the catalog, if it knows it. */
static void describe(game *g)
{
    g->release = find_release(g->entry, g->name);
    char size[24] = "";
    char year[8] = "";
    xc_catalog_release r;
    xc_catalog_game cg;
    char from[160];
    source_text(g, from, sizeof from);
    if (xc_catalog_release_at(&cat, g->release, &r) && xc_catalog_game_at(&cat, r.game, &cg)) {
        snprintf(g->name, sizeof g->name, "%s", cg.title);
        snprintf(g->key, sizeof g->key, "%08lX@%s", (unsigned long)r.title_id, r.variant);
        if (cg.year != 0) {
            snprintf(year, sizeof year, "%u", (unsigned)cg.year);
        }
        ui_format_size(size, sizeof size, g->size > 0 ? g->size : r.size);
        join(g->facts, sizeof g->facts, year);
        join(g->facts, sizeof g->facts, r.region);
        join(g->facts, sizeof g->facts, size);
        const char *by =
            cg.developer != NULL && cg.developer[0] != '\0' ? cg.developer : cg.publisher;
        snprintf(g->about, sizeof g->about, "%s%s%s%s%sRelease: %s (%s). %s",
                 by != NULL && by[0] != '\0' ? "By " : "", by != NULL ? by : "",
                 by != NULL && by[0] != '\0' ? ". " : "", cg.genres != NULL ? cg.genres : "",
                 cg.genres != NULL && cg.genres[0] != '\0' ? ". " : "", r.name, g->key, from);
        return;
    }
    if (g->size > 0) {
        ui_format_size(size, sizeof size, g->size);
        join(g->facts, sizeof g->facts, size);
    }
    snprintf(g->about, sizeof g->about, "%s%s%s%s",
             cat.release_count > 0 ? "Not in the catalog. " : "No catalog loaded. ",
             g->key[0] != '\0' ? "Title " : "", g->key, g->key[0] != '\0' ? ". " : "");
    size_t n = strlen(g->about);
    snprintf(g->about + n, sizeof g->about - n, "%s", from);
}

static int by_name(const void *a, const void *b)
{
    const game *x = a;
    const game *y = b;
    int c = ui_library_name_cmp(x->name, y->name);
    if (c != 0) {
        return c;
    }
    /* The same name twice (on two NASes, say): map entries in map order, then local files. */
    const char *sx = x->entry != NULL ? x->entry->url : x->path;
    const char *sy = y->entry != NULL ? y->entry->url : y->path;
    if ((x->entry == NULL) != (y->entry == NULL)) {
        return x->entry == NULL ? 1 : -1;
    }
    if (x->entry != NULL) {
        return x->entry < y->entry ? -1 : x->entry > y->entry ? 1 : 0;
    }
    return strcmp(sx, sy);
}

/* One game per map entry and local file, by name (ui_library_name_cmp). */
static game *list_games(const xc_map *map, char local[][XC_NX_PATH_MAX], int nlocal, int *count)
{
    size_t n = map->entry_count + (size_t)nlocal;
    game *games = calloc(n > 0 ? n : 1, sizeof *games);
    if (games == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < map->entry_count; i++) {
        const xc_map_entry *e = &map->entries[i];
        game *g = &games[i];
        g->entry = e;
        if (e->name != NULL) {
            /* From an archive.org listing: its name and size. */
            snprintf(g->name, sizeof g->name, "%s", e->name);
            g->size = e->size;
        } else {
            title_from(e->url, g->name, sizeof g->name);
            snprintf(g->key, sizeof g->key, "%08lX%s%s", (unsigned long)e->title_id,
                     e->variant[0] != '\0' ? "@" : "", e->variant);
        }
        xc_smb_url u;
        if (xc_smb_is_url(e->url) && xc_smb_url_parse(e->url, strlen(e->url), &u) == XC_OK) {
            snprintf(g->nas, sizeof g->nas, "%s", u.host);
        }
        describe(g);
    }
    for (int i = 0; i < nlocal; i++) {
        game *g = &games[map->entry_count + (size_t)i];
        title_from(local[i], g->name, sizeof g->name);
        g->path = local[i];
        describe(g);
    }
    qsort(games, n, sizeof *games, by_name);
    *count = (int)n;
    return games;
}

/* --- what's already in the install folder (xcat/installed.h) --------------------------- */

#define MAX_FOUND 256

typedef struct found {
    const char *root; /* settings.install_dir, or APPS_ROOT */
    char folder[XC_FATX_MAX_NAME + 1];
    xc_installed in;
    char facts[64];                                         /* size, files */
    char note[XC_SETTINGS_DIR_SIZE + XC_FATX_MAX_NAME + 2]; /* where */
    char about[400]; /* the title, the checks, where it came from */
} found;

static found *founds; /* MAX_FOUND of them, sorted by folder */
static int found_count;
static ui_lib_item found_items[MAX_FOUND];

static int by_folder(const void *a, const void *b)
{
    const found *x = a;
    const found *y = b;
    int c = ui_library_name_cmp(x->folder, y->folder);
    return c != 0 ? c : strcmp(x->root, y->root);
}

/* Where an image came from, for the details: a server's name, or the file's path. */
static void origin_text(const char *image, char *out_text, size_t size)
{
    const char *host = strstr(image, "://");
    if (host != NULL) {
        host += 3;
        (void)snprintf(out_text, size, "From %.*s.", (int)strcspn(host, "/:?#@"), host);
    } else if (image[0] != '\0') {
        (void)snprintf(out_text, size, "From %s.", image);
    } else {
        out_text[0] = '\0';
    }
}

static void describe_found(found *f, ui_lib_item *item)
{
    const xc_marker *m = &f->in.marker;
    snprintf(f->note, sizeof f->note, "%s\\%s", f->root, f->folder);
    f->facts[0] = '\0';
    if (f->in.state == XC_FOLDER_UNFINISHED) {
        snprintf(f->about, sizeof f->about, "%s",
                 "Not finished: install the game again from GAMES to carry on where it "
                 "stopped.");
    } else if (!f->in.marker_ok) {
        snprintf(f->about, sizeof f->about, "%s",
                 "Its " XC_INSTALL_MARKER " file can't be read, so there are no details.");
    } else {
        char size[24] = "";
        char files[24] = "";
        if (m->has_bytes) {
            ui_format_size(size, sizeof size, m->bytes);
        }
        if (m->has_files) {
            snprintf(files, sizeof files, "%llu file%s", (unsigned long long)m->files,
                     m->files == 1 ? "" : "s");
        }
        join(f->facts, sizeof f->facts, size);
        join(f->facts, sizeof f->facts, files);
        char title[sizeof m->title + 32] = "";
        if (m->has_title_id) {
            snprintf(title, sizeof title, "%s%stitle ID %08lX. ", m->title,
                     m->title[0] != '\0' ? ", " : "", (unsigned long)m->title_id);
        }
        char from[160];
        origin_text(m->image, from, sizeof from);
        snprintf(f->about, sizeof f->about, "%sChecked: every file's CRC-32%s. %s", title,
                 strstr(m->verified, "image-hash") != NULL ? ", and the image's published hash"
                                                           : "",
                 from);
    }
    *item = (ui_lib_item){
        f->folder, f->facts,
        f->about,  f->in.state == XC_FOLDER_INSTALLED ? UI_LIB_INSTALLED : UI_LIB_UNFINISHED,
        f->note,   strcmp(f->root, APPS_ROOT) == 0};
}

/* Adds the finished and unfinished installs in `root` to founds; other folders are left out. */
static void scan_root(const xc_install_io *io, const char *root)
{
    WIN32_FIND_DATAA fd;
    char pattern[XC_SETTINGS_DIR_SIZE + 2];
    snprintf(pattern, sizeof pattern, "%s\\*", root);
    HANDLE h = founds != NULL ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (found_count == MAX_FOUND) {
            out("More than %d installs: the rest of %s isn't listed\n", MAX_FOUND, root);
            break;
        }
        found *f = &founds[found_count];
        f->root = root;
        snprintf(f->folder, sizeof f->folder, "%s", fd.cFileName);
        if (xc_installed_check(io, root, f->folder, &f->in) == XC_OK &&
            f->in.state != XC_FOLDER_OTHER) {
            found_count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Lists the finished and unfinished installs in the install folder and in E:\\Apps (apps
   this app installed; the others there, this app included, have no marker or journal). */
static void scan_games(void)
{
    xc_install_io io;
    xc_nx_install_io(&io);
    found_count = 0;
    scan_root(&io, settings.install_dir);
    bool same = true;
    for (const char *a = settings.install_dir, *b = APPS_ROOT; same && (*a != '\0' || *b != '\0');
         a++, b++) {
        same = (*a | 0x20) == (*b | 0x20);
    }
    if (!same) {
        scan_root(&io, APPS_ROOT);
    }
    qsort(founds, (size_t)found_count, sizeof *founds, by_folder);
    for (int i = 0; i < found_count; i++) {
        describe_found(&founds[i], &found_items[i]);
    }
}

/* Marks the games already installed (the marker names the same image) or started (a journal
   in a folder named after the image, as xc_install would choose it, ADR 0008). */
static void mark_games(game *games, ui_lib_item *items, int count)
{
    int installed = 0;
    int unfinished = 0;
    for (int i = 0; i < count; i++) {
        const char *source = games[i].entry != NULL ? games[i].entry->url : games[i].path;
        char title[128];
        title_from(source, title, sizeof title);
        const found *half = NULL;
        const found *done = NULL;
        for (int k = 0; k < found_count && done == NULL; k++) {
            const found *f = &founds[k];
            if (f->in.state == XC_FOLDER_INSTALLED && f->in.marker_ok &&
                strcmp(f->in.marker.image, source) == 0) {
                done = f;
            } else if (f->in.state == XC_FOLDER_UNFINISHED && half == NULL &&
                       xc_installed_folder_for(f->folder, title)) {
                half = f;
            }
        }
        const found *f = done != NULL ? done : half;
        if (f == NULL) {
            continue;
        }
        snprintf(games[i].note, sizeof games[i].note, "%s%s\\%s",
                 done != NULL ? "In " : "Started in an earlier visit, in ", f->root, f->folder);
        if (done != NULL) {
            snprintf(games[i].installed, sizeof games[i].installed, "%s\\%s", f->root, f->folder);
        } else {
            snprintf(games[i].partial, sizeof games[i].partial, "%s\\%s", f->root, f->folder);
        }
        items[i].app = items[i].app || strcmp(f->root, APPS_ROOT) == 0;
        items[i].state = done != NULL ? UI_LIB_INSTALLED : UI_LIB_UNFINISHED;
        items[i].note = games[i].note;
        installed += done != NULL;
        unfinished += done == NULL;
    }
    out("%s and %s: %d folder(s) from this app; of the games listed, %d installed and %d "
        "unfinished\n",
        settings.install_dir, APPS_ROOT, found_count, installed, unfinished);
}

/* Installs g on the install screen until it's installed, or the user goes back: RETRY tries
   again. Leaves what the last attempt came to in `item`. True if the server refused the
   login and the user chose LOG IN (ADR 0010 decision 4): the caller logs in, then installs g
   again, which carries on where it stopped. */
static bool run_install(game *g, ui_lib_item *item)
{
    for (;;) {
        out("\nPicked: %s%s%s\n", g->name, g->key[0] != '\0' ? " " : "", g->key);
        install_start(g->name);
        bool ok = g->entry != NULL ? install_url(g->entry, g->release)
                                   : install_file(g->path, g->release);
        install_done();
        out("%s\n", ok ? "Installed and verified." : "Not installed.");
        if (ok) {
            snprintf(g->installed, sizeof g->installed, "%s", result.folder);
            g->partial[0] = '\0';
        } else if (result.folder[0] != '\0' && xc_nx_exists(result.folder)) {
            snprintf(g->partial, sizeof g->partial, "%s", result.folder);
        }
        snprintf(g->note, sizeof g->note, "%s", why);
        item->state = ok                            ? UI_LIB_INSTALLED
                      : why_err == XC_ERR_CANCELLED ? UI_LIB_STOPPED
                                                    : UI_LIB_FAILED;
        item->note = g->note;
        ui_install_status(&view, "%s", "");
        if (!ok && why_login) {
            ui_install_end_login(&view, why);
        } else if (!ok && !xc_err_retry(why_err)) {
            ui_install_end_back(&view, why); /* ADR 0015 decision 5: RETRY only where it helps */
        } else {
            ui_install_end(&view, ok, why);
        }
        ui_inst_action a = UI_INST_NONE;
        bool dirty = true;
        while (a == UI_INST_NONE) {
            if (combo_held || exit_combo()) { /* held during the install: it stopped for it */
                to_dashboard();
            }
            ui_button b;
            while (a == UI_INST_NONE &&
                   (b = next_button(ui_install_screen(&view))) != UI_BTN_NONE) {
                a = ui_install_input(&view, b);
                dirty = true;
            }
            if (dirty && a == UI_INST_NONE) {
                ui_sdl_begin(ui_col_bg);
                ui_install_draw(&screen, &view, pan_now);
                ui_sdl_present();
                dirty = false;
            } else {
                Sleep(5);
            }
        }
        if (a != UI_INST_RETRY) {
            return a == UI_INST_LOGIN;
        }
    }
}

/* Installs games[i] from the library, then marks the library with how it went. True if it
   waits for a login (run_install). */
static bool same_ci(const char *a, const char *b);

static void show_installed(ui_library *lib);

/* UI_LIB_DELETE: deletes the highlighted install's folder (INSTALLED), or what an unfinished
   install of the highlighted game left (GAMES). Only a folder this app made (a marker or a
   journal in it) in the install folder or E:\\Apps is ever deleted. */
static void delete_game(ui_library *lib, game *games, ui_lib_item *items, int count)
{
    char path[XC_INSTALL_MAX_PATH] = "";
    const char *name = "";
    if (lib->section == UI_LIB_INSTALLED_GAMES && found_count > 0) {
        const found *f = &founds[ui_library_installed(lib)];
        snprintf(path, sizeof path, "%s\\%s", f->root, f->folder);
        name = f->folder;
    } else if (lib->section == UI_LIB_GAMES && ui_library_game(lib) >= 0) {
        const game *g = &games[ui_library_game(lib)];
        snprintf(path, sizeof path, "%s", g->partial[0] != '\0' ? g->partial : g->installed);
        name = g->name;
    }
    /* Ours, and in one of the two folders installs go to. */
    const char *slash = strrchr(path, '\\');
    size_t root_len = slash != NULL ? (size_t)(slash - path) : 0;
    char root[XC_SETTINGS_DIR_SIZE + 2] = "";
    if (root_len > 0 && root_len < sizeof root) {
        memcpy(root, path, root_len);
        root[root_len] = '\0';
    }
    bool ours = false;
    if (slash != NULL && slash[1] != '\0' &&
        (same_ci(root, settings.install_dir) || same_ci(root, APPS_ROOT))) {
        xc_install_io io;
        xc_nx_install_io(&io);
        xc_installed in;
        ours =
            xc_installed_check(&io, root, slash + 1, &in) == XC_OK && in.state != XC_FOLDER_OTHER;
    }
    if (!ours) {
        char msg[sizeof path + 96];
        snprintf(msg, sizeof msg, "%s",
                 path[0] == '\0' ? "There's nothing on the hard disk to delete for it."
                                 : "That folder isn't one this app installed, so it's left alone.");
        out("Not deleted: \"%s\" (%s)\n", path, path[0] == '\0' ? "nothing there" : "not ours");
        ui_library_tell(lib, "NOT DELETED", msg);
        return;
    }
    char line[XC_INSTALL_MAX_PATH + 16];
    snprintf(line, sizeof line, "Deleting %s...", path);
    ui_sdl_begin(ui_col_bg);
    ui_library_draw(&screen, lib, pan_now, line);
    ui_sdl_present();
    xc_err err = xc_nx_remove_tree(path);
    out("Delete %s: %s\n", path, err == XC_OK ? "done" : xc_err_str(err));
    /* What's on the disk now, for every game. */
    for (int i = 0; i < count; i++) {
        if (items[i].state == UI_LIB_INSTALLED || items[i].state == UI_LIB_UNFINISHED ||
            ((items[i].state == UI_LIB_FAILED || items[i].state == UI_LIB_STOPPED) &&
             same_ci(games[i].partial, path))) {
            items[i].state = UI_LIB_NOT_TRIED;
            items[i].note = NULL;
        }
        if (same_ci(games[i].partial, path)) {
            games[i].partial[0] = '\0';
        }
        if (same_ci(games[i].installed, path)) {
            games[i].installed[0] = '\0';
        }
        if (items[i].state == UI_LIB_NOT_TRIED) {
            games[i].partial[0] = '\0';
            games[i].installed[0] = '\0';
        }
    }
    scan_games();
    mark_games(games, items, count);
    show_installed(lib);
    char msg[sizeof path + 160];
    if (err == XC_OK) {
        snprintf(msg, sizeof msg, "%s is deleted: %s is gone.", name, path);
    } else {
        snprintf(msg, sizeof msg,
                 "%s couldn't all be deleted (%s). What could be is gone; try again, or delete "
                 "the rest over FTP.",
                 path, xc_err_str(err));
    }
    ui_library_tell(lib, err == XC_OK ? "DELETED" : "NOT ALL DELETED", msg);
}

/* The folder an install of g goes to: where an unfinished one carries on, else E:\\Apps for an
   app, else the install folder. */
static const char *dest_for(const game *g, bool to_apps)
{
    const char *where = g->partial[0] != '\0' ? g->partial : g->installed;
    size_t n = strlen(APPS_ROOT);
    if (where[0] != '\0') {
        bool in_apps = strncmp(where, APPS_ROOT "\\", n + 1U) == 0;
        return in_apps ? APPS_ROOT : settings.install_dir;
    }
    return to_apps ? APPS_ROOT : settings.install_dir;
}

static bool install_game(ui_library *lib, game *games, ui_lib_item *items, int i, bool to_apps,
                         char *status, size_t size)
{
    game *g = &games[i];
    dest_dir = dest_for(g, to_apps);
    if (dest_dir != settings.install_dir && !xc_nx_mkdirs(dest_dir)) {
        out("Could not create %s\n", dest_dir);
    }
    out("Installing to %s\n", dest_dir);
    bool login_wanted = run_install(g, &items[i]);
    dest_dir = NULL;
    scan_games();
    show_installed(lib);
    ui_lib_state st = items[i].state;
    snprintf(status, size, "%s: %s", g->name,
             login_wanted             ? "log in, and it carries on"
             : st == UI_LIB_INSTALLED ? "installed"
             : st == UI_LIB_STOPPED   ? "stopped; install it again to carry on"
                                      : "not installed");
    return login_wanted;
}

/* --- Map sources: the map's own lines (SETTINGS, map.md §9) ------------------------------ */

#define MAX_SOURCES 64 /* lines listed; more are left out */

static char local_files[MAX_LOCAL][XC_NX_PATH_MAX]; /* E:\xcat\isos */
static int local_count;
static size_t source_line_no[MAX_SOURCES];
static char source_names[MAX_SOURCES][96];
static char source_notes[MAX_SOURCES][40];
static char source_abouts[MAX_SOURCES][400];
static ui_lib_source sources[MAX_SOURCES];

/* What the map reader said about line `line` of E:\xcat\map.txt: its first error, or else its
   first warning. "" if nothing. */
static void line_problem(size_t line, char *out_text, size_t size)
{
    out_text[0] = '\0';
    for (int pass = 0; pass < 2 && out_text[0] == '\0'; pass++) {
        for (size_t i = 0; have_map && i < map.problem_count; i++) {
            const xc_map_problem *p = &map.problems[i];
            bool want = pass == 0 ? p->severity == XC_MAP_ERROR : p->severity == XC_MAP_WARNING;
            if (want && p->source == 0 && p->line == line) {
                snprintf(out_text, size, "%s: %s", pass == 0 ? "Problem" : "Note", p->message);
                break;
            }
        }
    }
}

static void describe_source(const xc_map_line *l, int i)
{
    int tl = (int)l->text_len;
    int al = (int)l->arg_len;
    unsigned line = (unsigned)l->line;
    source_line_no[i] = l->line;
    const char *kind = "Directive";
    if (l->kind == XC_MAP_LINE_SMB) {
        kind = "NAS folder";
        snprintf(source_names[i], sizeof source_names[i], "NAS: %.*s", al > 6 ? al - 6 : al,
                 al > 6 ? l->arg + 6 : l->arg);
    } else if (l->kind == XC_MAP_LINE_ARCHIVEORG) {
        kind = "archive.org item";
        snprintf(source_names[i], sizeof source_names[i], "archive.org: %.*s", al, l->arg);
    } else if (l->kind == XC_MAP_LINE_INCLUDE) {
        kind = "Included map";
        snprintf(source_names[i], sizeof source_names[i], "Map: %.*s", al, l->arg);
    } else if (l->kind == XC_MAP_LINE_ENTRY) {
        kind = "Image";
        const char *file = l->arg;
        for (const char *p = l->arg; p < l->arg + l->arg_len; p++) {
            file = *p == '/' ? p + 1 : file;
        }
        int fl = (int)(l->arg + l->arg_len - file);
        snprintf(source_names[i], sizeof source_names[i], "%.*s: %.*s", (int)l->key_len, l->text,
                 fl, file);
    } else {
        snprintf(source_names[i], sizeof source_names[i], "%.*s", tl, l->text);
    }
    snprintf(source_notes[i], sizeof source_notes[i], "%s, line %u", kind, line);
    char problem[sizeof map.problems[0].message + 16];
    line_problem(l->line, problem, sizeof problem);
    snprintf(source_abouts[i], sizeof source_abouts[i], "%.*s%s%s", tl > 250 ? 250 : tl, l->text,
             problem[0] != '\0' ? ". " : "", problem);
    sources[i] = (ui_lib_source){source_names[i], source_notes[i], source_abouts[i]};
}

/* The map file's bytes, or none if there is no map. False, with msg saying why, if it can't be
   read. */
static bool read_map(char **data, size_t *len, char *msg, size_t size)
{
    *data = NULL;
    *len = 0;
    if (!xc_nx_exists(MAP_FILE)) {
        return true;
    }
    xc_err err = read_local(MAP_FILE, XC_MAP_MAX_FILE, data, len);
    if (err != XC_OK) {
        snprintf(msg, size, "E:\\xcat\\map.txt can't be read: %s.",
                 err == XC_ERR_LIMIT ? "it's larger than 1 MiB" : xc_err_str(err));
        return false;
    }
    return true;
}

/* The lines of E:\xcat\map.txt that Map sources lists. */
static int list_sources(void)
{
    char *data = NULL;
    size_t len = 0;
    char msg[160];
    static xc_map_line lines[MAX_SOURCES];
    size_t count = 0;
    if (!read_map(&data, &len, msg, sizeof msg) ||
        xc_map_lines(data, len, lines, MAX_SOURCES, &count) != XC_OK) {
        free(data);
        return 0;
    }
    int n = count < MAX_SOURCES ? (int)count : MAX_SOURCES;
    for (int i = 0; i < n; i++) {
        describe_source(&lines[i], i);
    }
    free(data);
    return n;
}

/* Says why the map wasn't changed: xc_map_add_line's or xc_map_remove_line's error. */
static void map_error(xc_err err, char *msg, size_t size)
{
    if (err == XC_ERR_MAP) {
        snprintf(msg, size, "%s",
                 "E:\\xcat\\map.txt doesn't start with \"# xcat-map v1\", so it isn't changed. "
                 "Fix it, or delete it to start a new one.");
    } else if (err == XC_ERR_LIMIT) {
        snprintf(msg, size, "%s", "The map would be larger than 1 MiB.");
    } else {
        snprintf(msg, size, "The map can't be changed: %s.", xc_err_str(err));
    }
}

/* Replaces E:\xcat\map.txt with text[0, len), atomically. */
static bool write_map(const char *text, size_t len, char *msg, size_t size)
{
    (void)xc_nx_mkdirs(XCAT_DIR);
    xc_err err = xc_nx_replace_file(MAP_FILE, text, len);
    if (err != XC_OK) {
        out("%s: %s; not changed\n", MAP_FILE, xc_err_str(err));
        snprintf(msg, size, "E:\\xcat\\map.txt couldn't be written: %s.", xc_err_str(err));
        return false;
    }
    return true;
}

/* UI_LIB_ADD_SOURCE: builds the line from what was typed and adds it to the map. On success,
   done says what was added; else msg says why not, and nothing changed. */
static bool add_source(ui_library *lib, char *done, size_t done_size, char *msg, size_t size)
{
    static const xc_map_line_kind kinds[] = {XC_MAP_LINE_ARCHIVEORG, XC_MAP_LINE_INCLUDE,
                                             XC_MAP_LINE_ENTRY};
    static char line[XC_MAP_MAX_LINE + 1];
    char why[200];
    xc_err err = xc_map_new_line(kinds[lib->add_kind], lib->add_key, lib->add_url, line,
                                 sizeof line, why, sizeof why);
    if (err != XC_OK) {
        snprintf(msg, size, "Not added: %s.", why);
        return false;
    }
    char *old = NULL;
    size_t old_len = 0;
    if (!read_map(&old, &old_len, msg, size)) {
        return false;
    }
    char *text = NULL;
    size_t len = 0;
    err = xc_map_add_line(old, old_len, line, &text, &len);
    free(old);
    if (err != XC_OK) {
        map_error(err, msg, size);
        return false;
    }
    bool ok = write_map(text, len, msg, size);
    free(text);
    if (ok) {
        out("Map: added \"%s\"\n", line);
        snprintf(done, done_size, "Added to the map: %.150s.", line);
        lib->add_url[0] = '\0';
        lib->add_key[0] = '\0';
    }
    return ok;
}

/* UI_LIB_REMOVE_SOURCE: removes the highlighted line from the map. */
static bool remove_source(ui_library *lib, char *done, size_t done_size, char *msg, size_t size)
{
    int i = ui_library_source(lib);
    if (i < 0 || i >= MAX_SOURCES) {
        snprintf(msg, size, "%s", "No line is highlighted.");
        return false;
    }
    char *old = NULL;
    size_t old_len = 0;
    if (!read_map(&old, &old_len, msg, size)) {
        return false;
    }
    char *text = NULL;
    size_t len = 0;
    xc_err err = xc_map_remove_line(old, old_len, source_line_no[i], &text, &len);
    free(old);
    if (err != XC_OK) {
        map_error(err, msg, size);
        return false;
    }
    bool ok = write_map(text, len, msg, size);
    free(text);
    if (ok) {
        out("Map: removed line %u\n", (unsigned)source_line_no[i]);
        snprintf(done, done_size, "Removed from the map: %.150s.", source_names[i]);
    }
    return ok;
}

/* The library's games: the map's and E:\xcat\isos', marked with what's installed. */
static bool build_list(game **out_games, ui_lib_item **out_items, int *out_count)
{
    int count = 0;
    game *games = list_games(&map, local_files, local_count, &count);
    ui_lib_item *items = calloc(count > 0 ? (size_t)count : 1, sizeof *items);
    if (games == NULL || items == NULL) {
        free(games);
        free(items);
        return false;
    }
    for (int i = 0; i < count; i++) {
        const char *src = games[i].entry != NULL ? games[i].entry->url : games[i].path;
        items[i] = (ui_lib_item){games[i].name,    games[i].facts, games[i].about,
                                 UI_LIB_NOT_TRIED, NULL,           looks_like_app(src)};
    }
    mark_games(games, items, count);
    *out_games = games;
    *out_items = items;
    *out_count = count;
    return true;
}

/* After the map changed: loads it again, with its includes and archive.org items, and gives
   the library the new list. The old one is freed once the library no longer shows it. */
static void reload_map(ui_library *lib, game **games, ui_lib_item **items, int *count, int **shown)
{
    ui_sdl_begin(ui_col_bg);
    ui_library_draw(&screen, lib, pan_now, "Loading the map again...");
    ui_sdl_present();
    xc_map_free(&map);
    have_map = load_map(&map);
    game *g = NULL;
    ui_lib_item *it = NULL;
    int n = 0;
    if (!build_list(&g, &it, &n)) {
        halt("Out of memory for the list of games");
    }
    int *s = calloc(n > 0 ? (size_t)n : 1, sizeof *s);
    if (s == NULL) {
        halt("Out of memory for the list of games");
    }
    ui_library_set_items(lib, it, n, s);
    free(*games);
    free(*items);
    free(*shown);
    *games = g;
    *items = it;
    *count = n;
    *shown = s;
    refresh_logins(lib);
    ui_library_set_sources(lib, sources, list_sources());
}

/* --- the install folder (SETTINGS) ------------------------------------------------------ */

static char folder_paths[3][12];
static char folder_notes[3][40];
static ui_lib_folder folder_choices[3];

/* Games on each drive there is, with how much is free on it. */
static int list_folders(void)
{
    int n = 0;
    for (char letter = 'E'; letter <= 'G'; letter++) {
        if ((letter == 'F' && !has_f) || (letter == 'G' && !has_g)) {
            continue;
        }
        snprintf(folder_paths[n], sizeof folder_paths[n], "%c:\\Games", letter);
        char root[4] = {letter, ':', '\\', '\0'};
        uint64_t free_bytes = 0;
        folder_notes[n][0] = '\0';
        if (xc_nx_free_space(root, &free_bytes) == XC_OK) {
            char size[24];
            ui_format_size(size, sizeof size, free_bytes);
            snprintf(folder_notes[n], sizeof folder_notes[n], "%s free", size);
        }
        folder_choices[n] = (ui_lib_folder){folder_paths[n], folder_notes[n]};
        n++;
    }
    return n;
}

/* INSTALLED's summary: the install folder's drive, then E: (apps) if that's another. */
static char disk_names[2][4];
static ui_lib_disk disks[2];

/* Gives INSTALLED its list and the drives' space, and SETTINGS the free space again: after
   anything that writes to or deletes from a drive. */
static void show_installed(ui_library *lib)
{
    ui_library_set_installed(lib, found_items, found_count);
    char letters[2] = {settings.install_dir[0], 'E'};
    if (letters[0] >= 'a' && letters[0] <= 'z') {
        letters[0] = (char)(letters[0] - 'a' + 'A');
    }
    int n = 0;
    for (int i = 0; i < 2; i++) {
        if (i == 1 && letters[1] == letters[0]) {
            break;
        }
        char root[4] = {letters[i], ':', '\\', '\0'};
        uint64_t free_bytes = 0;
        uint64_t total_bytes = 0;
        if (xc_nx_disk_space(root, &free_bytes, &total_bytes) != XC_OK) {
            continue;
        }
        snprintf(disk_names[n], sizeof disk_names[n], "%c:", letters[i]);
        disks[n] = (ui_lib_disk){disk_names[n], free_bytes, total_bytes};
        n++;
    }
    ui_library_set_disks(lib, disks, n);
    (void)list_folders(); /* the same drives, so the same count: just the notes change */
}

static bool same_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y || x == '\0') {
            return x == y;
        }
    }
}

/* UI_LIB_FOLDER: installs go to lib->folder from now on, if it can be. The library then marks
   and lists what's in it; games installed elsewhere stay where they are. */
static void change_folder(ui_library *lib, game *games, ui_lib_item *items, int count)
{
    char msg[256];
    if (same_ci(lib->folder, settings.install_dir)) {
        snprintf(msg, sizeof msg, "Games already install to %s.", settings.install_dir);
        ui_library_tell(lib, "INSTALL LOCATION", msg);
        return;
    }
    if (!save_folder(lib->folder, msg, sizeof msg)) {
        ui_library_tell(lib, "NOT CHANGED", msg);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (items[i].state == UI_LIB_INSTALLED || items[i].state == UI_LIB_UNFINISHED) {
            items[i].state = UI_LIB_NOT_TRIED;
            items[i].note = NULL;
        }
        games[i].installed[0] = '\0';
    }
    scan_games();
    mark_games(games, items, count);
    show_installed(lib);
    ui_library_set_folders(lib, settings.install_dir, folder_choices, list_folders());
    snprintf(msg, sizeof msg,
             "Games install to %s from now on. Games installed in another folder stay there.",
             settings.install_dir);
    ui_library_tell(lib, "INSTALL LOCATION", msg);
}

/* --- playing an installed game ---------------------------------------------------------- */

/* UI_LIB_PLAY: starts default.xbe in the game's folder. Returns only if it can't. */
static void play(ui_library *lib, const game *games)
{
    char folder[XC_INSTALL_MAX_PATH] = "";
    if (lib->section == UI_LIB_GAMES && ui_library_game(lib) >= 0) {
        snprintf(folder, sizeof folder, "%s", games[ui_library_game(lib)].installed);
    } else if (lib->section == UI_LIB_INSTALLED_GAMES && found_count > 0) {
        const found *f = &founds[ui_library_installed(lib)];
        snprintf(folder, sizeof folder, "%s\\%s", f->root, f->folder);
    }
    char xbe[XC_INSTALL_MAX_PATH + 16];
    snprintf(xbe, sizeof xbe, "%s\\default.xbe", folder);
    if (folder[0] == '\0' || !xc_nx_exists(xbe)) {
        char msg[sizeof xbe + 64];
        snprintf(msg, sizeof msg,
                 "There's no default.xbe in %s, so the game can't start from here.",
                 folder[0] != '\0' ? folder : "its folder");
        ui_library_tell(lib, "CAN'T START", msg);
        return;
    }
    out("\nStarting %s\n", xbe);
    starting = false;
    close_log();
    ui_sdl_close();
    XLaunchXBE(xbe);
    /* XLaunchXBE only returns if the path couldn't be used: back to the dashboard. */
    HalReturnToFirmware(HalQuickRebootRoutine);
}

/* --- SETTINGS > NAS: finding a NAS, logging in, choosing a shared folder (smb.md) ---------- */

#define MAX_BROWSE 256 /* shares or folders listed; more are left out */

static struct {
    char host[XC_SMB_MAX_HOST + 1];
    uint16_t port;
    char label[48]; /* its NetBIOS name if the scan found one, else its address */
    char user[XC_SMB_MAX_USER + 1];
    char domain[XC_SMB_MAX_DOMAIN + 1];
    char nthash[XC_SMB_NTHASH_HEX + 1];
    bool guest;
    bool typed;                       /* typed just now: saved once the NAS takes it */
    xc_smb_conn *conn;                /* to `share` while browsing it */
    char share[XC_SMB_MAX_SHARE + 1]; /* "" while listing the shares */
    char path[XC_SMB_MAX_PATH + 1];   /* inside the share, '/'-separated */
} nas;

#define MAX_NAS (XC_SMB_SCAN_MAX + 16) /* found by the scan, plus saved ones */

static xc_smb_found nas_found[XC_SMB_SCAN_MAX];
static size_t nas_found_n;
static char nas_names[MAX_NAS][40];
static char nas_addrs[MAX_NAS][UI_LIB_NAS_ADDR_MAX];
static char nas_notes[MAX_NAS][96];
static char nas_logins[MAX_NAS][XC_SMB_MAX_DOMAIN + XC_SMB_MAX_USER + 2];
static ui_lib_nas nas_list[MAX_NAS];
static int nas_n;
static char nas_status[200];
static char browse_names[MAX_BROWSE][XC_SMB_MAX_NAME + 1];
static char browse_notes[MAX_BROWSE][48];
static ui_lib_entry browse_entries[MAX_BROWSE];
static int browse_n;
static int browse_images;
static char browse_title[XC_SMB_MAX_PATH + 160];
static char browse_about[240];

/* Shows `line` in the footer at once: the scan and logins take a moment. */
static void nas_busy(ui_library *lib, const char *line)
{
    if (exit_combo()) {
        to_dashboard();
    }
    ui_sdl_begin(ui_col_bg);
    ui_library_draw(&screen, lib, pan_now, line);
    ui_sdl_present();
}

static bool scan_progress(void *ctx, unsigned done, unsigned total)
{
    char line[96];
    snprintf(line, sizeof line, "Asking the network for a NAS: %u of %u addresses...", done, total);
    nas_busy(ctx, line);
    return true;
}

/* "host", or "host:port" when it isn't 445: how the NAS page names an address. */
static void nas_address(const char *host, uint16_t port, char *out, size_t size)
{
    if (port == 0 || port == XC_SMB_PORT) {
        snprintf(out, size, "%s", host);
    } else {
        snprintf(out, size, "%s:%u", host, (unsigned)port);
    }
}

/* Adds a NAS to the page's list unless its address is there already. */
static void nas_add(const char *addr, const char *login)
{
    for (int i = 0; i < nas_n; i++) {
        if (strcmp(nas_addrs[i], addr) == 0) {
            return;
        }
    }
    if (nas_n == MAX_NAS) {
        return;
    }
    int i = nas_n++;
    snprintf(nas_addrs[i], sizeof nas_addrs[i], "%s", addr);
    snprintf(nas_names[i], sizeof nas_names[i], "%s", addr);
    snprintf(nas_notes[i], sizeof nas_notes[i], "%s", addr);
    /* The scan's name for it, and its workgroup, if the scan found it. */
    for (size_t k = 0; k < nas_found_n; k++) {
        const xc_smb_found *f = &nas_found[k];
        char ip[24];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", f->ip[0], f->ip[1], f->ip[2], f->ip[3]);
        if (strcmp(ip, addr) == 0) {
            if (f->name[0] != '\0') {
                snprintf(nas_names[i], sizeof nas_names[i], "%s", f->name);
            }
            snprintf(nas_notes[i], sizeof nas_notes[i], "%s%s%s", addr,
                     f->group[0] != '\0' ? ", workgroup " : "", f->group);
        }
    }
    if (login != NULL) {
        snprintf(nas_logins[i], sizeof nas_logins[i], "%s", login);
    }
    nas_list[i] = (ui_lib_nas){nas_names[i], nas_addrs[i], nas_notes[i],
                               login != NULL ? nas_logins[i] : NULL};
}

/* The NAS page's list: the NASes with a saved login, those the map gets games from (as a
   guest when they have none), then what the last scan found. */
static void nas_refresh(ui_library *lib)
{
    nas_n = 0;
    char addr[UI_LIB_NAS_ADDR_MAX];
    for (size_t i = 0; have_creds && i < creds.section_count; i++) {
        const xc_cred_section *c = &creds.sections[i];
        if (c->smb_user == NULL) {
            continue;
        }
        char login[sizeof nas_logins[0]];
        if (c->smb_domain != NULL) {
            snprintf(login, sizeof login, "%s\\%s", c->smb_domain, c->smb_user);
        } else {
            snprintf(login, sizeof login, "%s", c->smb_user);
        }
        for (size_t k = 0; k < c->pattern_count; k++) {
            const xc_cred_pattern *pt = &c->patterns[k];
            if (!pt->wildcard) {
                nas_address(pt->host, pt->any_port ? XC_SMB_PORT : pt->port, addr, sizeof addr);
                nas_add(addr, login);
            }
        }
    }
    static xc_smb_url u;
    for (size_t i = 0; i < map.smb_folder_count; i++) {
        const char *f = map.smb_folders[i];
        if (xc_smb_url_parse(f, strlen(f), &u) == XC_OK) {
            nas_address(u.host, u.port, addr, sizeof addr);
            nas_add(addr, "a guest"); /* one with a saved login is in the list already */
        }
    }
    for (size_t k = 0; k < nas_found_n; k++) {
        const xc_smb_found *f = &nas_found[k];
        snprintf(addr, sizeof addr, "%u.%u.%u.%u", f->ip[0], f->ip[1], f->ip[2], f->ip[3]);
        nas_add(addr, NULL);
    }
    ui_library_set_nas(lib, nas_list, nas_n, nas_status);
}

/* UI_LIB_NAS_PAGE: the network started, so the page can say where the console is. */
static void nas_page(ui_library *lib)
{
    if (!net_tried) {
        nas_busy(lib, "Starting the network...");
        (void)network();
    }
    nas_refresh(lib);
}

/* UI_LIB_NAS_SCAN */
static void nas_scan(ui_library *lib)
{
    nas_busy(lib, "Starting the network...");
    if (!network()) {
        ui_library_tell(lib, "NO NETWORK",
                        "The network didn't start: check the cable and the dashboard's network "
                        "settings.");
        return;
    }
    xc_smb_scan_opts o;
    memset(&o, 0, sizeof o);
    if (!xc_nx_net_ipv4(o.ip, o.mask)) {
        ui_library_tell(lib, "NO ADDRESS", "The console has no network address yet.");
        return;
    }
    o.batch = 16;
    o.wait_ms = 600;
    o.progress = scan_progress;
    o.ctx = lib;
    size_t n = 0;
    out("NAS scan from %u.%u.%u.%u/%u.%u.%u.%u\n", o.ip[0], o.ip[1], o.ip[2], o.ip[3], o.mask[0],
        o.mask[1], o.mask[2], o.mask[3]);
    xc_err err = xc_smb_scan(&o, nas_found, XC_SMB_SCAN_MAX, &n);
    nas_found_n = n;
    for (size_t i = 0; i < n; i++) {
        const xc_smb_found *f = &nas_found[i];
        out("  found %s at %u.%u.%u.%u%s%s\n", f->name[0] != '\0' ? f->name : "a server", f->ip[0],
            f->ip[1], f->ip[2], f->ip[3], f->group[0] != '\0' ? ", workgroup " : "", f->group);
    }
    if (err != XC_OK) {
        snprintf(nas_status, sizeof nas_status, "The scan stopped: %s.", xc_err_str(err));
    } else if (n == 0) {
        snprintf(nas_status, sizeof nas_status,
                 "No NAS answered on %u.%u.%u.x. Check that SMB is on in the NAS's settings, or "
                 "type its address.",
                 o.ip[0], o.ip[1], o.ip[2]);
    } else {
        snprintf(nas_status, sizeof nas_status, "%u found on %u.%u.%u.x: they're at the top.",
                 (unsigned)n, o.ip[0], o.ip[1], o.ip[2]);
    }
    nas_refresh(lib);
    if (n == 0 || err != XC_OK) {
        ui_library_tell(lib, "NAS SCAN", nas_status);
    }
}

static xc_smb_login nas_login_now(void)
{
    xc_smb_login l = {NULL, NULL, NULL};
    if (!nas.guest) {
        l.user = nas.user;
        l.domain = nas.domain[0] != '\0' ? nas.domain : NULL;
        l.nthash = nas.nthash;
    }
    return l;
}

static void nas_close(void)
{
    xc_smb_disconnect(nas.conn);
    nas.conn = NULL;
}

/* Why a NAS request failed, on a message, and in the log with the server's own words. */
static void nas_failed(ui_library *lib, const char *title, xc_err err, const char *detail)
{
    char msg[sizeof lib->message];
    snprintf(msg, sizeof msg, "%s. %s", xc_err_str(err), xc_err_action(err));
    out("NAS %s: %s (%s)\n", nas.host, xc_err_str(err), detail != NULL ? detail : "");
    ui_library_tell(lib, title, msg);
}

static bool add_share(void *ctx, const char *name, const char *remark)
{
    (void)ctx;
    if (browse_n == MAX_BROWSE) {
        return false;
    }
    snprintf(browse_names[browse_n], sizeof browse_names[browse_n], "%s", name);
    snprintf(browse_notes[browse_n], sizeof browse_notes[browse_n], "%s", remark);
    browse_entries[browse_n] = (ui_lib_entry){browse_names[browse_n], browse_notes[browse_n]};
    browse_n++;
    return true;
}

static int by_entry_name(const void *a, const void *b)
{
    const char *x = ((const ui_lib_entry *)a)->name;
    const char *y = ((const ui_lib_entry *)b)->name;
    for (;; x++, y++) {
        int cx = *x >= 'A' && *x <= 'Z' ? *x - 'A' + 'a' : (unsigned char)*x;
        int cy = *y >= 'A' && *y <= 'Z' ? *y - 'A' + 'a' : (unsigned char)*y;
        if (cx != cy || cx == 0) {
            return cx - cy;
        }
    }
}

/* A credentials section name for the NAS (credentials.md §3): the one that already logs in to
   it, else "nas-" and its address with every other character as '-'. */
static void nas_section_name(char *out, size_t size)
{
    const xc_cred_section *s = have_creds ? xc_cred_for_smb(&creds, nas.host, nas.port) : NULL;
    if (s != NULL) {
        snprintf(out, size, "%s", s->name);
        return;
    }
    int n = snprintf(out, size, "nas-%s", nas.host);
    for (int i = 4; i < n && (size_t)i < size; i++) {
        char c = out[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        out[i] = ok ? c : '-';
    }
    if (n > XC_CRED_MAX_NAME) {
        out[XC_CRED_MAX_NAME] = '\0';
    }
}

/* "host" or "host:port", the credentials file's pattern for the NAS (credentials.md §5). */
static void nas_pattern(char *out, size_t size)
{
    nas_address(nas.host, nas.port, out, size);
}

/* The NAS's login into the credentials file, unless it's there already as it is (a guest has
   none). False, with msg saying why, if it can't be written. */
static bool save_nas_login(char *msg, size_t size)
{
    if (nas.guest) {
        return true;
    }
    const xc_cred_section *c = have_creds ? xc_cred_for_smb(&creds, nas.host, nas.port) : NULL;
    if (c != NULL && strcmp(c->smb_user, nas.user) == 0 &&
        strcmp(c->smb_domain != NULL ? c->smb_domain : "", nas.domain) == 0 &&
        strcmp(c->smb_nthash, nas.nthash) == 0) {
        return true;
    }
    char section_name[XC_CRED_MAX_NAME + 8];
    nas_section_name(section_name, sizeof section_name);
    char pattern[XC_SMB_MAX_HOST + 8];
    nas_pattern(pattern, sizeof pattern);
    static char section[XC_CRED_MAX_NAME + XC_SMB_MAX_HOST + XC_SMB_MAX_USER + XC_SMB_MAX_DOMAIN +
                        XC_SMB_NTHASH_HEX + 96];
    xc_err err = xc_cred_smb_section(section_name, pattern, nas.user,
                                     nas.domain[0] != '\0' ? nas.domain : NULL, nas.nthash, section,
                                     sizeof section);
    if (err != XC_OK) {
        snprintf(msg, size, "%s",
                 "The user name can't be kept in the credentials file (no \\ / : @ or \").");
        return false;
    }
    if (!write_section(section_name, section, msg, size)) {
        return false;
    }
    out("NAS login [%s] for %s written to %s\n", section_name, pattern, CRED_FILE);
    reload_credentials();
    return true;
}

/* The NAS's shares, as the first browse level. */
static void nas_list_shares(ui_library *lib)
{
    nas_close();
    nas.share[0] = '\0';
    nas.path[0] = '\0';
    char line[XC_SMB_MAX_HOST + 48];
    snprintf(line, sizeof line, "Logging in to %s...", nas.label);
    nas_busy(lib, line);
    browse_n = 0;
    xc_smb_login login = nas_login_now();
    char detail[XC_SMB_DETAIL_MAX];
    xc_err err = xc_smb_shares(nas.host, nas.port, &login, add_share, NULL, detail, sizeof detail);
    if (err == XC_ERR_SMB_LOGIN && !nas.typed && !nas.guest) {
        out("NAS %s: the saved login was refused (%s)\n", nas.host, detail);
        char msg[sizeof lib->message];
        snprintf(msg, sizeof msg,
                 "%s refused the saved login for %s: has its password changed? Press X on it "
                 "to log in again.",
                 nas.label, nas.user);
        ui_library_tell(lib, "NOT LOGGED IN", msg);
        ui_library_nas_page(lib);
        return;
    }
    if (err != XC_OK) {
        nas_failed(lib, "NOT LOGGED IN", err, detail);
        ui_library_nas_page(lib);
        return;
    }
    qsort(browse_entries, (size_t)browse_n, sizeof *browse_entries, by_entry_name);
    out("NAS %s: logged in as %s, %d shared folder(s)\n", nas.host,
        nas.guest ? "a guest" : nas.user, browse_n);
    /* A login typed just now is saved as soon as the NAS takes it, so it's typed once. */
    const char *saved = "";
    if (nas.typed && !nas.guest) {
        char msg[320];
        if (save_nas_login(msg, sizeof msg)) {
            saved = " Login saved.";
            refresh_logins(lib);
            nas_refresh(lib);
        } else {
            ui_library_tell(lib, "LOGIN NOT SAVED", msg);
        }
    }
    nas.typed = false;
    snprintf(browse_title, sizeof browse_title, "%s", nas.label);
    if (browse_n == 0) {
        snprintf(browse_about, sizeof browse_about, "No shared folders this login can see.%s",
                 saved);
    } else {
        snprintf(browse_about, sizeof browse_about,
                 "%d shared folder%s. A opens one; X adds a whole one to GAMES.%s", browse_n,
                 browse_n == 1 ? "" : "s", saved);
    }
    ui_library_browse(lib, browse_title, browse_about, browse_entries, browse_n, NULL);
}

static bool browse_game; /* the folder browsed has a default.xbe: it's a game */

static bool add_folder(void *ctx, const char *name, bool is_dir, uint64_t size)
{
    (void)ctx;
    (void)size;
    if (!is_dir) {
        char image[XC_SMB_MAX_NAME + 1];
        browse_images += xc_smb_image_name(name, image, sizeof image, NULL) ? 1 : 0;
        browse_game = browse_game || xc_smb_is_default_xbe(name);
        return true;
    }
    if (browse_n == MAX_BROWSE || xc_smb_skipped_folder(name) ||
        !xc_smb_valid_name(name, strlen(name))) {
        return true;
    }
    snprintf(browse_names[browse_n], sizeof browse_names[browse_n], "%s", name);
    browse_entries[browse_n] = (ui_lib_entry){browse_names[browse_n], NULL};
    browse_n++;
    return true;
}

/* The folders in nas.share/nas.path, after "Add this folder". False if it can't be listed. */
static bool nas_list_folder(ui_library *lib)
{
    nas_busy(lib, "Listing the folder...");
    browse_n = 0;
    browse_images = 0;
    browse_game = false;
    xc_err err = xc_smb_list(nas.conn, nas.path, add_folder, NULL);
    if (err != XC_OK) {
        nas_failed(lib, "CAN'T OPEN", err, xc_smb_detail(nas.conn));
        return false;
    }
    qsort(browse_entries, (size_t)browse_n, sizeof *browse_entries, by_entry_name);
    int n = snprintf(browse_title, sizeof browse_title, "%s > %s", nas.label, nas.share);
    for (const char *p = nas.path; *p != '\0' && n > 0 && (size_t)n < sizeof browse_title;) {
        size_t len = strcspn(p, "/");
        n += snprintf(browse_title + n, sizeof browse_title - (size_t)n, " > %.*s", (int)len, p);
        p += len + (p[len] == '/' ? 1U : 0U);
    }
    if (browse_game) {
        snprintf(browse_about, sizeof browse_about, "%s",
                 "This folder is a game: it has a default.xbe. Adding it lists it in GAMES as one "
                 "game, which installs file by file.");
    } else {
        snprintf(browse_about, sizeof browse_about,
                 "Here: %d image%s (.iso, .xiso, .zip) and %d folder%s. Adding it lists them, and "
                 "the images and game folders (with a default.xbe) one folder down.",
                 browse_images, browse_images == 1 ? "" : "s", browse_n, browse_n == 1 ? "" : "s");
    }
    ui_library_browse(lib, browse_title, browse_about, browse_entries, browse_n,
                      browse_game ? "Add this game to GAMES" : "Add this folder to GAMES");
    return true;
}

/* UI_LIB_NAS_LOGIN: what was typed (or picked) becomes the NAS and its login, kept in memory
   as an NT hash until a folder is added; the password is wiped. */
/* lib->nas_addr as a host and port, into `u`. False if it isn't an address. */
static bool nas_target(const ui_library *lib, xc_smb_url *u)
{
    char probe[UI_LIB_NAS_ADDR_MAX + 16];
    snprintf(probe, sizeof probe, "smb://%s/IPC", lib->nas_addr);
    return strchr(lib->nas_addr, '/') == NULL && xc_smb_url_parse(probe, strlen(probe), u) == XC_OK;
}

/* The NAS about to be logged in to: its address, and its name on the NAS page. */
static void nas_set_target(const ui_library *lib, const xc_smb_url *u)
{
    snprintf(nas.host, sizeof nas.host, "%s", u->host);
    nas.port = u->port;
    snprintf(nas.label, sizeof nas.label, "%s", u->host);
    for (int i = 0; i < lib->nas_count; i++) {
        if (strcmp(lib->nas[i].address, lib->nas_addr) == 0) {
            snprintf(nas.label, sizeof nas.label, "%s", lib->nas[i].name);
        }
    }
}

/* UI_LIB_NAS_SAVED: the NAS's saved login (or a guest one, for a NAS the map has without one),
   straight to its shares. */
static void nas_saved(ui_library *lib)
{
    static xc_smb_url u;
    if (!nas_target(lib, &u)) {
        return;
    }
    if (!network()) {
        ui_library_tell(lib, "NO NETWORK",
                        "The network didn't start: check the cable and the dashboard's network "
                        "settings.");
        return;
    }
    nas_set_target(lib, &u);
    const xc_cred_section *c = have_creds ? xc_cred_for_smb(&creds, u.host, u.port) : NULL;
    nas.guest = c == NULL;
    nas.typed = false;
    nas.user[0] = '\0';
    nas.domain[0] = '\0';
    if (c != NULL) {
        snprintf(nas.user, sizeof nas.user, "%s", c->smb_user);
        snprintf(nas.domain, sizeof nas.domain, "%s", c->smb_domain != NULL ? c->smb_domain : "");
        snprintf(nas.nthash, sizeof nas.nthash, "%s", c->smb_nthash);
    }
    nas_list_shares(lib);
}

static void nas_login(ui_library *lib)
{
    static xc_smb_url u;
    bool addr_ok = nas_target(lib, &u);
    nas.guest = lib->nas_user[0] == '\0';
    nas.typed = true;
    nas.domain[0] = '\0';
    nas.user[0] = '\0';
    xc_err err = XC_OK;
    if (!nas.guest) {
        /* DOMAIN\user, as Windows writes it, or just the user. */
        const char *slash = strchr(lib->nas_user, '\\');
        if (slash != NULL) {
            snprintf(nas.domain, sizeof nas.domain, "%.*s", (int)(slash - lib->nas_user),
                     lib->nas_user);
            snprintf(nas.user, sizeof nas.user, "%s", slash + 1);
        } else {
            snprintf(nas.user, sizeof nas.user, "%s", lib->nas_user);
        }
        err = xc_smb_nthash(lib->nas_password, nas.nthash);
    }
    ui_library_forget_nas_password(lib);
    if (!addr_ok) {
        ui_library_tell(lib, "NOT LOGGED IN",
                        "That isn't a NAS address: type its IP address, like 192.168.1.50, or "
                        "its name, with :port only if it isn't 445.");
        return;
    }
    if (err != XC_OK || (!nas.guest && nas.user[0] == '\0')) {
        ui_library_tell(lib, "NOT LOGGED IN",
                        "The user name or password can't be used (a password is at most 256 "
                        "characters).");
        return;
    }
    if (!network()) {
        ui_library_tell(lib, "NO NETWORK",
                        "The network didn't start: check the cable and the dashboard's network "
                        "settings.");
        return;
    }
    nas_set_target(lib, &u);
    nas_list_shares(lib);
}

/* UI_LIB_NAS_OPEN: a share, or a folder in it. */
static void nas_open(ui_library *lib)
{
    int e = ui_library_browse_entry(lib);
    if (e < 0 || e >= browse_n) {
        return;
    }
    char name[XC_SMB_MAX_NAME + 1];
    snprintf(name, sizeof name, "%s", browse_entries[e].name); /* as listed: sorted */
    if (nas.share[0] == '\0') {
        char line[XC_SMB_MAX_SHARE + 32];
        snprintf(line, sizeof line, "Opening %s...", name);
        nas_busy(lib, line);
        xc_smb_login login = nas_login_now();
        char detail[XC_SMB_DETAIL_MAX];
        xc_err err =
            xc_smb_connect(nas.host, nas.port, name, &login, &nas.conn, detail, sizeof detail);
        if (err != XC_OK) {
            nas_failed(lib, "CAN'T OPEN", err, detail);
            return;
        }
        snprintf(nas.share, sizeof nas.share, "%s", name);
        nas.path[0] = '\0';
        if (!nas_list_folder(lib)) {
            nas_list_shares(lib);
        }
        return;
    }
    size_t len = strlen(nas.path);
    if (len + 1 + strlen(name) > XC_SMB_MAX_PATH) {
        ui_library_tell(lib, "CAN'T OPEN", "The folder's path is too long for an smb:// address.");
        return;
    }
    char was[XC_SMB_MAX_PATH + 1];
    snprintf(was, sizeof was, "%s", nas.path);
    snprintf(nas.path + len, sizeof nas.path - len, "%s%s", len > 0 ? "/" : "", name);
    if (!nas_list_folder(lib)) {
        snprintf(nas.path, sizeof nas.path, "%s", was);
        (void)nas_list_folder(lib);
    }
}

/* UI_LIB_NAS_UP: the folder above, the shares, then the NAS page. */
static void nas_up(ui_library *lib)
{
    if (nas.share[0] == '\0') {
        nas_close();
        ui_library_nas_page(lib);
        return;
    }
    if (nas.path[0] == '\0') {
        nas_list_shares(lib);
        return;
    }
    char *slash = strrchr(nas.path, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        nas.path[0] = '\0';
    }
    if (!nas_list_folder(lib)) {
        nas_list_shares(lib);
    }
}

/* UI_LIB_NAS_USE: the login into the credentials file (unless it's a guest) and an @smb line
   for the folder into the map: the folder browsed (pick -1), or browse entry `pick` in it (a
   share, at the NAS's top). Then the map is loaded again, the folder listed with it. */
static bool nas_use(int pick, char *done, size_t done_size, char *msg, size_t size)
{
    static xc_smb_url u;
    memset(&u, 0, sizeof u);
    snprintf(u.host, sizeof u.host, "%s", nas.host);
    u.port = nas.port;
    u.is_ipv4 = false;
    const char *name = pick >= 0 && pick < browse_n ? browse_entries[pick].name : NULL;
    if (nas.share[0] == '\0') {
        if (name == NULL) {
            snprintf(msg, size, "%s", "Not added: pick a shared folder first.");
            return false;
        }
        snprintf(u.share, sizeof u.share, "%s", name);
    } else {
        snprintf(u.share, sizeof u.share, "%s", nas.share);
        int n = snprintf(u.path, sizeof u.path, "%s%s%s", nas.path,
                         name != NULL && nas.path[0] != '\0' ? "/" : "", name != NULL ? name : "");
        if (n < 0 || (size_t)n >= sizeof u.path) {
            snprintf(msg, size, "%s", "Not added: the folder's path is too long.");
            return false;
        }
    }
    char url[XC_SMB_MAX_URL + 64];
    static char line[XC_MAP_MAX_LINE + 1];
    char why_not[200];
    xc_err err = xc_smb_url_format(&u, url, sizeof url);
    if (err == XC_OK) {
        err =
            xc_map_new_line(XC_MAP_LINE_SMB, NULL, url, line, sizeof line, why_not, sizeof why_not);
    }
    if (err != XC_OK) {
        snprintf(msg, size, "Not added: %s.",
                 err == XC_ERR_LIMIT ? "the address is too long" : why_not);
        return false;
    }
    /* The login first, so the map finds the folder with it. */
    bool had_login = have_creds && xc_cred_for_smb(&creds, nas.host, nas.port) != NULL;
    if (!nas.guest) {
        if (!save_nas_login(msg, size)) {
            return false;
        }
    } else if (had_login) {
        /* A guest now: the login kept before would still be used. */
        char section_name[XC_CRED_MAX_NAME + 8];
        nas_section_name(section_name, sizeof section_name);
        if (!write_section(section_name, NULL, msg, size)) {
            return false;
        }
        out("NAS login [%s] removed: the NAS is used as a guest\n", section_name);
    }
    reload_credentials();
    /* The map: the line once, at the end. */
    char *old = NULL;
    size_t old_len = 0;
    if (!read_map(&old, &old_len, msg, size)) {
        return false;
    }
    static xc_map_line lines[MAX_SOURCES];
    size_t count = 0;
    bool have = false;
    if (old != NULL && xc_map_lines(old, old_len, lines, MAX_SOURCES, &count) == XC_OK) {
        for (size_t i = 0; i < count && i < MAX_SOURCES; i++) {
            have = have || (lines[i].text_len == strlen(line) &&
                            memcmp(lines[i].text, line, lines[i].text_len) == 0);
        }
    }
    if (!have) {
        char *text = NULL;
        size_t len = 0;
        err = xc_map_add_line(old, old_len, line, &text, &len);
        free(old);
        old = NULL;
        if (err != XC_OK) {
            map_error(err, msg, size);
            return false;
        }
        bool ok = write_map(text, len, msg, size);
        free(text);
        if (!ok) {
            return false;
        }
        out("Map: added \"%s\"\n", line);
    }
    free(old);
    snprintf(done, done_size, "%s %s.", have ? "Already in the map:" : "Added", url + 6);
    return true;
}

/* --- a game's picture and details (docs/formats/art.md, ADR 0019) ------------------------ */

#define ART_SLOTS 12        /* games whose art is kept, the least recently shown dropped */
#define ART_SETTLE_MS 350U  /* the highlight rests this long before art is read for it */
#define ART_RETRY_MS 60000U /* a NAS that didn't answer is asked again after this */
#define ART_KEY_MAX (XC_INSTALL_MAX_PATH + 16)

/* Where a game's files are: a folder on a NAS share or on the console, and the image's name. */
typedef struct art_where {
    bool smb;
    xc_smb_url url;                  /* smb: the share, and the folder in url.path */
    char dir[XC_NX_PATH_MAX];        /* on the console: the folder */
    char image[XC_SMB_MAX_NAME + 1]; /* the image's or archive's file name; "" for a folder */
    bool own_folder;                 /* art.md §2: the folder holds just this game */
} art_where;

typedef struct art_slot {
    char key[ART_KEY_MAX]; /* the game's URL or path; "" if the slot is free */
    uint32_t used;         /* art_clock when last shown */
    bool failed;           /* the NAS didn't answer: asked again after ART_RETRY_MS */
    uint32_t failed_at;
    xc_picture pic; /* fitted to the details, opaque; empty if there's none */
    char title[XC_XBE_TITLE_UNITS * 3 + 128];
    char facts[128];
    char about[1800];
    ui_lib_art art;
} art_slot;

static art_slot art_slots[ART_SLOTS];
static uint32_t art_clock;
static char art_shown[ART_KEY_MAX]; /* the key of what the details show, and since when */
static uint32_t art_since;
static xc_synopsis art_syn; /* one load at a time */

/* art.md §2: an @smb folder of the map is that folder (so an image in it isn't alone there). */
static bool listed_folder(const xc_smb_url *dir)
{
    for (size_t i = 0; i < map.smb_folder_count; i++) {
        xc_smb_url f;
        const char *u = map.smb_folders[i];
        if (xc_smb_url_parse(u, strlen(u), &f) == XC_OK && same_ci(f.host, dir->host) &&
            f.port == dir->port && same_ci(f.share, dir->share) && same_ci(f.path, dir->path)) {
            return true;
        }
    }
    return false;
}

/* Where GAMES' game g is, and its key; false if its art isn't looked for (an HTTP source). */
static bool art_where_game(const game *g, art_where *w, char *key, size_t key_size)
{
    memset(w, 0, sizeof *w);
    if (g->entry != NULL) {
        const char *url = g->entry->url;
        size_t n = strlen(url);
        if (!xc_smb_is_url(url) || xc_smb_url_parse(url, n, &w->url) != XC_OK) {
            return false;
        }
        w->smb = true;
        snprintf(key, key_size, "%s", url);
        if (n > 0 && url[n - 1U] == '/') {
            return true; /* a game folder: url.path is it */
        }
        char *slash = strrchr(w->url.path, '/');
        snprintf(w->image, sizeof w->image, "%s", slash != NULL ? slash + 1 : w->url.path);
        if (slash != NULL) {
            *slash = '\0';
        } else {
            w->url.path[0] = '\0';
        }
        w->own_folder = !listed_folder(&w->url);
        return true;
    }
    if (g->path == NULL) {
        return false;
    }
    const char *slash = strrchr(g->path, '\\');
    if (slash == NULL) {
        return false;
    }
    snprintf(w->dir, sizeof w->dir, "%.*s", (int)(slash - g->path), g->path);
    snprintf(w->image, sizeof w->image, "%s", slash + 1);
    snprintf(key, key_size, "%s", g->path);
    return true;
}

/* Where INSTALLED's install f is: a game folder on the console. */
static bool art_where_found(const found *f, art_where *w, char *key, size_t key_size)
{
    memset(w, 0, sizeof *w);
    int n = snprintf(w->dir, sizeof w->dir, "%s\\%s", f->root, f->folder);
    if (n < 0 || (size_t)n >= sizeof w->dir) {
        return false;
    }
    snprintf(key, key_size, "installed:%s", w->dir);
    return true;
}

/* Opens `rel` ('/'-separated, in w's folder) as a source; closed with art_close. */
static xc_err art_open(const art_where *w, xc_smb_conn *c, const char *rel, xc_smb_file **sf,
                       xc_nx_file *lf, xc_source *src)
{
    char path[XC_SMB_MAX_PATH + XC_ART_MAX_PATH + 2];
    int n = w->smb ? snprintf(path, sizeof path, "%s%s%s", w->url.path,
                              w->url.path[0] != '\0' ? "/" : "", rel)
                   : snprintf(path, sizeof path, "%s\\%s", w->dir, rel);
    if (n < 0 || (size_t)n >= sizeof path) {
        return XC_ERR_LIMIT;
    }
    if (w->smb) {
        return xc_smb_open(c, path, sf, src);
    }
    for (char *p = path + strlen(w->dir); *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return xc_nx_file_open(src, lf, path);
}

static void art_close(xc_smb_file **sf, xc_nx_file *lf)
{
    xc_smb_close(*sf);
    *sf = NULL;
    xc_nx_file_close(lf);
    lf->handle = NULL;
}

/* The whole of `rel`, if it's at most `max` bytes; freed by the caller. */
static xc_err art_read(const art_where *w, xc_smb_conn *c, const char *rel, size_t max,
                       uint8_t **data, size_t *len)
{
    xc_smb_file *sf = NULL;
    xc_nx_file lf = {NULL};
    xc_source src;
    *data = NULL;
    xc_err err = art_open(w, c, rel, &sf, &lf, &src);
    if (err == XC_OK && (src.size == 0 || src.size > max)) {
        err = XC_ERR_LIMIT;
    }
    if (err == XC_OK) {
        *data = malloc((size_t)src.size);
        err = *data != NULL ? xc_source_read(&src, 0, *data, (size_t)src.size) : XC_ERR_NOMEM;
        *len = (size_t)src.size;
    }
    art_close(&sf, &lf);
    if (err != XC_OK) {
        free(*data);
        *data = NULL;
    }
    return err;
}

/* art.md §3: the default.xbe beside, or in a disc image's root, into info; and its title image
   into *pic if pic isn't NULL. */
static bool art_xbe(const art_where *w, xc_smb_conn *c, const xc_art_plan *plan, xc_xbe_info *info,
                    xc_picture *pic)
{
    xc_smb_file *sf = NULL;
    xc_nx_file lf = {NULL};
    xc_source src;
    xc_source inside;
    xc_window_source win;
    const xc_source *xbe = NULL;
    size_t n = strlen(w->image);
    if (plan->xbe[0] != '\0') {
        if (art_open(w, c, plan->xbe, &sf, &lf, &src) == XC_OK) {
            xbe = &src;
        }
    } else if (ends_with_ci(w->image, n, ".iso") || ends_with_ci(w->image, n, ".xiso")) {
        xc_xdvdfs_volume vol;
        xc_xdvdfs_entry e;
        bool found = false;
        if (art_open(w, c, w->image, &sf, &lf, &src) == XC_OK &&
            xc_xdvdfs_open(&src, &vol) == XC_OK &&
            xc_xdvdfs_find_root(&src, &vol, "default.xbe", &e, &found) == XC_OK && found &&
            (e.attributes & 0x10U) == 0 && e.offset <= src.size && src.size - e.offset >= e.size) {
            xc_source_window(&inside, &win, &src, e.offset, e.size);
            xbe = &inside;
        }
    }
    bool ok = xbe != NULL && xc_xbe_read_info(xbe, info) == XC_OK;
    if (ok && pic != NULL && info->image_size > 0) {
        uint8_t *buf = malloc(info->image_size);
        if (buf != NULL &&
            xc_source_read(xbe, info->image_offset, buf, info->image_size) == XC_OK) {
            (void)xc_picture_xpr(buf, info->image_size, pic);
        }
        free(buf);
    }
    art_close(&sf, &lf);
    return ok;
}

/* Appends ". "-separated sentence parts to s (of `size`). */
static void art_say(char *s, size_t size, const char *fmt, ...)
{
    size_t n = strlen(s);
    if (n >= size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s + n, size - n, fmt, ap);
    va_end(ap);
}

/* art.md §6: the details from the synopsis and the XBE, around what the item already says. */
static void art_text(art_slot *s, const char *facts, const char *about, bool catalog,
                     const xc_synopsis *syn, const xc_xbe_info *x)
{
    const char *title = syn != NULL && syn->title[0] != '\0' ? syn->title
                        : x != NULL                          ? x->cert.title
                                                             : "";
    snprintf(s->title, sizeof s->title, "%s", title);
    /* Facts: the year and region if the catalog didn't give them, then the item's. */
    s->facts[0] = '\0';
    if (!catalog && syn != NULL && syn->year[0] != '\0' && strstr(facts, syn->year) == NULL) {
        join(s->facts, sizeof s->facts, syn->year);
    }
    const char *region = x != NULL && x->has_region ? xc_art_region(x->region) : "";
    if (!catalog && region[0] != '\0') {
        join(s->facts, sizeof s->facts, region);
    }
    join(s->facts, sizeof s->facts, facts);
    /* About: who made it, what it is, the XBE's numbers, the item's text, then the overview. */
    s->about[0] = '\0';
    if (syn != NULL) {
        if (syn->developer[0] != '\0' && syn->publisher[0] != '\0' &&
            !same_ci(syn->developer, syn->publisher)) {
            art_say(s->about, sizeof s->about, "By %s for %s. ", syn->developer, syn->publisher);
        } else if (syn->developer[0] != '\0' || syn->publisher[0] != '\0') {
            art_say(s->about, sizeof s->about, "By %s. ",
                    syn->developer[0] != '\0' ? syn->developer : syn->publisher);
        }
        if (syn->genre[0] != '\0') {
            art_say(s->about, sizeof s->about, "%s. ", syn->genre);
        }
        if (syn->features[0] != '\0') {
            art_say(s->about, sizeof s->about, "%s. ", syn->features);
        }
        if (syn->online[0] != '\0') {
            art_say(s->about, sizeof s->about, "Online: %s. ", syn->online);
        }
        if (syn->esrb[0] != '\0') {
            art_say(s->about, sizeof s->about, "ESRB %s. ", syn->esrb);
        }
        if (syn->rating[0] != '\0') {
            art_say(s->about, sizeof s->about, "Rated %s. ", syn->rating);
        }
    }
    if (x != NULL) {
        art_say(s->about, sizeof s->about, "Title ID %08lX", (unsigned long)x->cert.title_id);
        if (x->has_region && x->disc > 0 && x->disc < 10) {
            art_say(s->about, sizeof s->about, ", disc %lu", (unsigned long)x->disc + 1UL);
        }
        art_say(s->about, sizeof s->about, ". ");
    }
    art_say(s->about, sizeof s->about, "%s", about != NULL ? about : "");
    if (syn != NULL && syn->overview[0] != '\0') {
        art_say(s->about, sizeof s->about, "\n%s", syn->overview);
    }
    s->art =
        (ui_lib_art){s->pic.px, (int)s->pic.w, (int)s->pic.h, s->title[0] != '\0' ? s->title : NULL,
                     s->facts,  s->about};
}

/* The slot for `key`, or NULL. */
static art_slot *art_find(const char *key)
{
    for (int i = 0; i < ART_SLOTS; i++) {
        if (art_slots[i].key[0] != '\0' && strcmp(art_slots[i].key, key) == 0) {
            return &art_slots[i];
        }
    }
    return NULL;
}

/* A free slot, else the least recently shown, emptied. */
static art_slot *art_take(void)
{
    art_slot *s = &art_slots[0];
    for (int i = 0; i < ART_SLOTS; i++) {
        if (art_slots[i].key[0] == '\0') {
            s = &art_slots[i];
            break;
        }
        if (art_slots[i].used < s->used) {
            s = &art_slots[i];
        }
    }
    xc_picture_free(&s->pic);
    memset(s, 0, sizeof *s);
    return s;
}

/* Reads a game's picture and details (art.md §2-§5) into a slot for `key`. */
static art_slot *art_load(const char *key, const art_where *w, const char *facts, const char *about,
                          bool catalog, uint32_t now)
{
    art_slot *s = art_find(key);
    if (s != NULL) {
        xc_picture_free(&s->pic);
        memset(s, 0, sizeof *s);
    } else {
        s = art_take();
    }
    snprintf(s->key, sizeof s->key, "%s", key);
    xc_smb_conn *c = NULL;
    if (w->smb) {
        char why_smb[XC_SMB_DETAIL_MAX];
        xc_err err = network() ? xc_smb_connect_url(&w->url, have_creds ? &creds : NULL, &c,
                                                    why_smb, sizeof why_smb)
                               : XC_ERR_NET_CONNECT;
        if (err != XC_OK) {
            out("Details of %s: the NAS %s: %s\n", key, w->url.host, xc_err_str(err));
            s->failed = true;
            s->failed_at = now;
            art_text(s, facts, about, catalog, NULL, NULL);
            return s;
        }
    }
    xc_art_plan plan;
    xc_art_plan_for(w->image[0] != '\0' ? w->image : NULL, w->own_folder, &plan);
    xc_picture pic = {NULL, 0, 0};
    const char *cover = "";
    for (size_t i = 0; i < plan.cover_count && pic.px == NULL; i++) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (art_read(w, c, plan.covers[i], XC_PICTURE_MAX_FILE, &data, &len) != XC_OK) {
            continue;
        }
        xc_err err = xc_picture_decode(data, len, &pic);
        free(data);
        if (err == XC_OK) {
            cover = plan.covers[i];
        } else {
            out("Details of %s: %s isn't a picture this app reads (%s)\n", key, plan.covers[i],
                xc_err_str(err));
        }
    }
    bool has_syn = false;
    if (plan.synopsis[0] != '\0') {
        uint8_t *data = NULL;
        size_t len = 0;
        if (art_read(w, c, plan.synopsis, XC_SYNOPSIS_MAX_FILE, &data, &len) == XC_OK) {
            has_syn = xc_synopsis_parse((const char *)data, len, &art_syn) == XC_OK;
        }
        free(data);
    }
    xc_xbe_info info;
    bool has_xbe = art_xbe(w, c, &plan, &info, pic.px == NULL ? &pic : NULL);
    xc_smb_disconnect(c);
    if (pic.px != NULL) {
        uint32_t bg =
            (uint32_t)ui_col_panel.r << 16U | (uint32_t)ui_col_panel.g << 8U | ui_col_panel.b;
        (void)xc_picture_fit(&pic, UI_LIB_ART_W, UI_LIB_ART_H, 2, bg, &s->pic);
    }
    out("Details of %s: picture %s (%lux%lu), synopsis %s, XBE %s\n", key,
        cover[0] != '\0' ? cover
        : pic.px != NULL ? "from the XBE"
                         : "none",
        (unsigned long)pic.w, (unsigned long)pic.h, has_syn ? "yes" : "no", has_xbe ? "yes" : "no");
    xc_picture_free(&pic);
    art_text(s, facts, about, catalog, has_syn ? &art_syn : NULL, has_xbe ? &info : NULL);
    return s;
}

/* Once a frame: gives the details the art of the item they show, reading it once the
   highlight has rested there. True if the screen should be drawn again. */
static bool art_frame(ui_library *lib, const game *games, int count, uint32_t now)
{
    ui_lib_section sec;
    int item = -1;
    char key[ART_KEY_MAX] = "";
    art_where w;
    bool have = ui_library_shown_item(lib, &sec, &item);
    const char *facts = "";
    const char *about = "";
    bool catalog = false;
    if (have && sec == UI_LIB_GAMES && item >= 0 && item < count) {
        have = art_where_game(&games[item], &w, key, sizeof key);
        facts = games[item].facts;
        about = games[item].about;
        catalog = games[item].release != XC_CATALOG_NONE;
    } else if (have && sec == UI_LIB_INSTALLED_GAMES && item >= 0 && item < found_count) {
        have = art_where_found(&founds[item], &w, key, sizeof key);
        facts = founds[item].facts;
        about = founds[item].about;
        catalog = true; /* its facts are its own: size and files */
    } else {
        have = false;
    }
    if (!have) {
        art_shown[0] = '\0';
        return false;
    }
    if (strcmp(key, art_shown) != 0) {
        snprintf(art_shown, sizeof art_shown, "%s", key);
        art_since = now;
    }
    art_slot *s = art_find(key);
    if (s == NULL || (s->failed && now - s->failed_at > ART_RETRY_MS)) {
        if (now - art_since < ART_SETTLE_MS) {
            return false; /* still moving */
        }
        if (w.smb) {
            ui_sdl_begin(ui_col_bg);
            ui_library_draw(&screen, lib, pan_now, "Reading the game's picture and details...");
            ui_sdl_present();
        }
        s = art_load(key, &w, facts, about, catalog, now);
    }
    s->used = ++art_clock;
    if (lib->art != &s->art || lib->art_section != sec || lib->art_item != item) {
        ui_library_set_art(lib, sec, item, &s->art);
        return true;
    }
    return false;
}

/* The top panel: the console's address, and the highlighted game's NAS in GAMES, else the
   map's first, said to be off if none of its folders listed. */
static void show_top(ui_library *lib, const game *games, int count)
{
    int g = lib->section == UI_LIB_GAMES ? ui_library_game(lib) : -1;
    const char *nas = g >= 0 && g < count && games[g].nas[0] != '\0' ? games[g].nas
                      : smb_seen_n > 0                               ? smb_seen[0].host
                                                                     : NULL;
    bool off = false;
    for (int i = 0; nas != NULL && i < smb_seen_n; i++) {
        if (strcmp(smb_seen[i].host, nas) == 0) {
            off = !smb_seen[i].listed;
        }
    }
    ui_lib_top top = {console_ip[0] != '\0' ? console_ip : NULL, nas, off};
    ui_library_set_top(lib, &top);
}

/* The library screen, until the user leaves for the dashboard. */
static void library(game *games, ui_lib_item *items, int count)
{
    static ui_library lib;
    int *shown = calloc(count > 0 ? (size_t)count : 1, sizeof *shown);
    if (shown == NULL) {
        halt("Out of memory for the list of games");
    }
    ui_library_open(&lib, items, count, shown);
    show_installed(&lib);
    refresh_logins(&lib);
    ui_library_set_sources(&lib, sources, list_sources());
    ui_library_set_folders(&lib, settings.install_dir, folder_choices, list_folders());
    ui_library_set_console(&lib, console_line);
    if (settings_problem[0] != '\0') {
        ui_library_tell(&lib, "SETTINGS", settings_problem);
    }
    if (script_problem[0] != '\0') {
        ui_library_tell(&lib, "UI SCRIPT", script_problem);
    }
    if (script_data != NULL) {
        ui_script_start(&player, script_data);
    }
    char drawn[sizeof player.status] = ""; /* the footer last drawn */
    int rows = ui_library_rows(&screen);
    int range = ui_pan_range(&screen);
    char status[160];
    snprintf(status, sizeof status, "%s",
             "Pick a game and press A. Nothing installs until you say so.");
    bool dirty = true;
    int resume = -1; /* the game whose install waits for a login, or -1 */
    for (;;) {
        if (exit_combo()) {
            to_dashboard();
        }
        ui_button b;
        while ((b = next_button(ui_library_screen(&lib))) != UI_BTN_NONE) {
            dirty = true;
            ui_lib_action a = ui_library_input(&lib, b, rows);
            int install_now = -1;
            if (a == UI_LIB_EXIT) {
                to_dashboard();
            } else if (a == UI_LIB_INSTALL) {
                install_now = ui_library_game(&lib);
            } else if (a == UI_LIB_LOGIN) {
                /* Say so first: starting HTTPS and logging in take a few seconds. */
                ui_sdl_begin(ui_col_bg);
                ui_library_draw(&screen, &lib, pan_now, "Logging in to archive.org...");
                ui_sdl_present();
                if (login(&lib, resume >= 0) && resume >= 0) {
                    out("Carrying on with the install that waited for the login\n");
                    install_now = resume;
                }
                resume = -1;
            } else if (a == UI_LIB_LOGOUT) {
                logout(&lib);
            } else if (a == UI_LIB_FORGET) {
                forget(&lib);
            } else if (a == UI_LIB_ADD_SOURCE || a == UI_LIB_REMOVE_SOURCE) {
                char done[200];
                char msg[320];
                bool added = a == UI_LIB_ADD_SOURCE;
                bool ok = added ? add_source(&lib, done, sizeof done, msg, sizeof msg)
                                : remove_source(&lib, done, sizeof done, msg, sizeof msg);
                if (ok) {
                    reload_map(&lib, &games, &items, &count, &shown);
                    resume = -1;
                    char problem[sizeof map.problems[0].message + 16] = "";
                    if (added && lib.source_count > 0) {
                        line_problem(source_line_no[lib.source_count - 1], problem, sizeof problem);
                    }
                    snprintf(msg, sizeof msg, "%s %d game%s listed now.%s%s", done, count,
                             count == 1 ? "" : "s", problem[0] != '\0' ? " " : "", problem);
                }
                ui_library_tell(&lib,
                                ok ? (added ? "SOURCE ADDED" : "SOURCE REMOVED")
                                   : (added ? "NOT ADDED" : "NOT REMOVED"),
                                msg);
            } else if (a == UI_LIB_FOLDER) {
                change_folder(&lib, games, items, count);
            } else if (a == UI_LIB_PLAY) {
                play(&lib, games);
            } else if (a == UI_LIB_DELETE) {
                delete_game(&lib, games, items, count);
            } else if (a == UI_LIB_NAS_PAGE) {
                nas_page(&lib);
            } else if (a == UI_LIB_NAS_SCAN) {
                nas_scan(&lib);
            } else if (a == UI_LIB_NAS_LOGIN) {
                nas_login(&lib);
            } else if (a == UI_LIB_NAS_SAVED) {
                nas_saved(&lib);
            } else if (a == UI_LIB_NAS_OPEN) {
                nas_open(&lib);
            } else if (a == UI_LIB_NAS_UP) {
                nas_up(&lib);
            } else if (a == UI_LIB_NAS_USE) {
                char done[XC_SMB_MAX_URL + 64];
                char msg[320];
                if (nas_use(ui_library_nas_pick(&lib), done, sizeof done, msg, sizeof msg)) {
                    reload_map(&lib, &games, &items, &count, &shown);
                    nas_refresh(&lib);
                    resume = -1;
                    snprintf(msg, sizeof msg, "%s %d game%s listed now, in GAMES.", done, count,
                             count == 1 ? "" : "s");
                    /* Done with the NAS: on to the games it gave. */
                    nas_close();
                    lib.page = UI_LIB_PAGE_NONE;
                    lib.section = UI_LIB_GAMES;
                    lib.in_sections = false;
                    ui_library_tell(&lib, "NAS FOLDER ADDED", msg);
                } else {
                    ui_library_tell(&lib, "NOT ADDED", msg);
                }
            } else if (resume >= 0 && lib.typing_for == UI_LIB_TYPING_NONE) {
                resume = -1; /* the keyboard was cancelled: nothing carries on */
            }
            if (install_now >= 0 && install_game(&lib, games, items, install_now,
                                                 ui_library_to_apps(&lib), status, sizeof status)) {
                resume = install_now;
                ui_library_login(&lib);
            }
        }
        int pan = ui_pan_x(SDL_GetTicks(), range);
        if (pan != pan_now) {
            pan_now = pan;
            dirty = true;
        }
        /* While a script plays, the footer says where it is, then how it ended. */
        const char *footer = player.state != UI_SCRIPT_OFF ? player.status : status;
        if (strcmp(footer, drawn) != 0) {
            snprintf(drawn, sizeof drawn, "%s", footer);
            dirty = true;
        }
        show_top(&lib, games, count);
        if (art_frame(&lib, games, count, SDL_GetTicks())) {
            dirty = true; /* the details have a picture now */
        }
        if (ui_library_tick(&lib, SDL_GetTicks())) {
            dirty = true; /* a long name slides */
        }
        if (dirty) {
            ui_sdl_begin(ui_col_bg);
            ui_library_draw(&screen, &lib, pan_now, footer);
            ui_sdl_present();
            dirty = false;
        } else {
            Sleep(5);
        }
    }
}

int main(void)
{
    InitializeCriticalSection(&view_lock);
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    xc_nx_clock(&clk);
    run_start_ms = now_ms();
    pads_init();
    /* The screens first: from here on, startup is shown on the startup screen. Only if they
       can't open is the debug console used (ADR 0015 decision 1). */
    const char *why_not = NULL;
    if (!ui_sdl_open(&screen, "D:", &why_not)) {
        debugPrint("xiso-cartographer %s\n", xc_version_string());
        debugPrint("Hold LT + RT + Back + Black to return to the dashboard.\n");
        char why[160];
        snprintf(why, sizeof why, "The screens can't start: %s", why_not);
        halt(why);
    }
    screens_open = true;
    ui_startup_open(&startup, xc_version_string());
    starting = true;
    startup_frame();
    io_buf = malloc(IO_BUF_SIZE);
    recv_buf = malloc(RECV_BUF_SIZE);
    if (io_buf == NULL || recv_buf == NULL) {
        halt("Not enough memory to start");
    }
    if (!xc_nx_mount_e()) {
        halt("The E: drive can't be opened");
    }
    if (!xc_nx_mkdirs(ISO_DIR)) {
        halt("Could not create " ISO_DIR);
    }
    open_log();
    out("==== xiso-cartographer %s ====\n", xc_version_string());
    xc_nx_mount_fg(&has_f, &has_g);
    out("Drives: E:%s%s\n", has_f ? ", F:" : "", has_g ? ", G:" : "");
    load_settings();
    if (!xc_nx_mkdirs(settings.install_dir)) {
        char why[160];
        snprintf(why, sizeof why, "Could not create %s", settings.install_dir);
        halt(why);
    }
    load_credentials();
    load_catalog();
    load_script();

    have_map = load_map(&map);
    local_count = list_local(local_files);
    if ((!have_map || map.entry_count == 0) && local_count == 0) {
        seed_samples();
        local_count = list_local(local_files);
    }

    founds = malloc(MAX_FOUND * sizeof *founds);
    if (founds == NULL) {
        out("Out of memory for the list of installed games: none are shown\n");
    }
    scan_games();
    int count = 0;
    game *games = NULL;
    ui_lib_item *items = NULL;
    if (!build_list(&games, &items, &count)) {
        halt("Out of memory for the list of games");
    }
    if (!pads_ok) {
        halt("No controller support (SDL), so nothing can be picked");
    }
    out("List: %d game(s). Pick one on the screen; nothing installs until you do.\n", count);
    starting = false;             /* the library takes over; out() goes to the log only */
    library(games, items, count); /* returns to the dashboard from there */
}
