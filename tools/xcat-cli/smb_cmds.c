/* xcat-cli's SMB commands (smb_cmds.h, docs/formats/smb.md). */
#include "smb_cmds.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/smb_client.h"
#include "xcat/smb_scan.h"

const char *xcat_cli_smb_usage(void)
{
    return "       xcat-cli smb-scan [--port <n>] [--batch <n>] [--wait <ms>] <ip>/<prefix>\n"
           "       xcat-cli smb-shares [--credentials <file>] <host>[:<port>]\n"
           "       xcat-cli smb-ls [--credentials <file>] [--tree] <smb://host/share[/folder]>\n"
           "       xcat-cli smb-hash [user] [domain]   (reads the password from stdin)\n";
}

static bool read_file(const char *path, char **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t cap = 4096;
    size_t n = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap || n > XC_CRED_MAX_FILE) {
            break;
        }
        char *grown = realloc(buf, cap * 2);
        if (grown == NULL) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = grown;
        cap *= 2;
    }
    (void)fclose(f);
    *data = buf;
    *len = n;
    return buf != NULL;
}

static bool load_creds(const char *path, xc_credentials *c)
{
    char *data = NULL;
    size_t len = 0;
    if (!read_file(path, &data, &len)) {
        (void)fprintf(stderr, "xcat-cli: %s: can't read it; continuing without credentials\n",
                      path);
        return false;
    }
    xc_cred_error cerr;
    xc_err err = xc_cred_parse(data, len, c, &cerr);
    free(data);
    if (err != XC_OK) {
        (void)fprintf(stderr, "xcat-cli: %s line %zu: %s; continuing without credentials\n", path,
                      cerr.line, err == XC_ERR_CREDENTIALS ? cerr.message : xc_err_str(err));
        return false;
    }
    return true;
}

static int fail(const char *what, xc_err err, const char *detail)
{
    if (detail != NULL && detail[0] != '\0') {
        (void)fprintf(stderr, "xcat-cli: %s: %s (%s)\n", what, xc_err_str(err), detail);
    } else {
        (void)fprintf(stderr, "xcat-cli: %s: %s\n", what, xc_err_str(err));
    }
    return 1;
}

static bool parse_ip(const char *s, uint8_t out[4], const char **rest)
{
    unsigned v[4];
    int n = 0;
    if (sscanf(s, "%3u.%3u.%3u.%3u%n", &v[0], &v[1], &v[2], &v[3], &n) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (v[i] > 255U) {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }
    *rest = s + n;
    return true;
}

static bool scan_progress(void *ctx, unsigned done, unsigned total)
{
    (void)ctx;
    (void)fprintf(stderr, "\r  asked %u of %u", done, total);
    return true;
}

static int cmd_scan(int argc, char **argv)
{
    xc_smb_scan_opts o;
    memset(&o, 0, sizeof o);
    int i = 2;
    for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        unsigned long v = strtoul(argv[i + 1], NULL, 10);
        if (strcmp(argv[i], "--port") == 0 && v > 0 && v < 65536UL) {
            o.port = (uint16_t)v;
        } else if (strcmp(argv[i], "--batch") == 0 && v > 0 && v <= 64) {
            o.batch = (unsigned)v;
        } else if (strcmp(argv[i], "--wait") == 0 && v > 0 && v < 60000UL) {
            o.wait_ms = (unsigned)v;
        } else {
            return 2;
        }
    }
    const char *rest = NULL;
    if (argc - i != 1 || !parse_ip(argv[i], o.ip, &rest) || rest[0] != '/') {
        return 2;
    }
    unsigned long prefix = strtoul(rest + 1, NULL, 10);
    if (prefix < 1 || prefix > 32) {
        return 2;
    }
    uint32_t m = prefix == 32 ? 0xFFFFFFFFU : ~(0xFFFFFFFFU >> prefix);
    for (int k = 0; k < 4; k++) {
        o.mask[k] = (uint8_t)(m >> (24 - 8 * k));
    }
    o.progress = scan_progress;
    static xc_smb_found found[XC_SMB_SCAN_MAX];
    size_t n = 0;
    xc_err err = xc_smb_scan(&o, found, XC_SMB_SCAN_MAX, &n);
    (void)fprintf(stderr, "\n");
    if (err != XC_OK) {
        return fail("scan", err, NULL);
    }
    for (size_t k = 0; k < n; k++) {
        (void)printf("%u.%u.%u.%u:%u  %s%s%s\n", found[k].ip[0], found[k].ip[1], found[k].ip[2],
                     found[k].ip[3], (unsigned)found[k].port,
                     found[k].name[0] != '\0' ? found[k].name : "(no NetBIOS name)",
                     found[k].group[0] != '\0' ? "  workgroup " : "", found[k].group);
    }
    (void)printf("%zu SMB server%s found\n", n, n == 1 ? "" : "s");
    return 0;
}

/* --credentials <file> before the other arguments; returns the index after them. */
static int creds_opt(int argc, char **argv, int i, xc_credentials *c, bool *have)
{
    *have = false;
    if (i + 1 < argc && strcmp(argv[i], "--credentials") == 0) {
        *have = load_creds(argv[i + 1], c);
        i += 2;
    }
    return i;
}

static bool print_share(void *ctx, const char *name, const char *remark)
{
    (void)ctx;
    (void)printf("%-32s %s\n", name, remark);
    return true;
}

static int cmd_shares(int argc, char **argv)
{
    xc_credentials c;
    memset(&c, 0, sizeof c);
    bool have = false;
    int i = creds_opt(argc, argv, 2, &c, &have);
    if (argc - i != 1) {
        xc_cred_free(&c);
        return 2;
    }
    char host[XC_SMB_MAX_HOST + 1];
    uint16_t port = XC_SMB_PORT;
    const char *colon = strchr(argv[i], ':');
    size_t hl = colon != NULL ? (size_t)(colon - argv[i]) : strlen(argv[i]);
    if (hl == 0 || hl > XC_SMB_MAX_HOST) {
        xc_cred_free(&c);
        return 2;
    }
    memcpy(host, argv[i], hl);
    host[hl] = '\0';
    for (size_t k = 0; k < hl; k++) {
        host[k] = host[k] >= 'A' && host[k] <= 'Z' ? (char)(host[k] - 'A' + 'a') : host[k];
    }
    if (colon != NULL) {
        unsigned long p = strtoul(colon + 1, NULL, 10);
        if (p == 0 || p > 65535UL) {
            xc_cred_free(&c);
            return 2;
        }
        port = (uint16_t)p;
    }
    xc_smb_login login;
    xc_smb_login_for(have ? &c : NULL, host, port, &login);
    (void)fprintf(stderr, "  as %s\n", login.user != NULL ? login.user : "a guest");
    char detail[XC_SMB_DETAIL_MAX];
    xc_err err = xc_smb_shares(host, port, &login, print_share, NULL, detail, sizeof detail);
    xc_cred_free(&c);
    return err == XC_OK ? 0 : fail(argv[i], err, detail);
}

static bool print_entry(void *ctx, const char *name, bool is_dir, uint64_t size)
{
    (void)ctx;
    if (is_dir) {
        (void)printf("%15s  %s/\n", "<folder>", name);
    } else {
        (void)printf("%15" PRIu64 "  %s\n", size, name);
    }
    return true;
}

static int cmd_ls(int argc, char **argv)
{
    xc_credentials c;
    memset(&c, 0, sizeof c);
    bool have = false;
    int i = creds_opt(argc, argv, 2, &c, &have);
    bool tree = i < argc && strcmp(argv[i], "--tree") == 0;
    i += tree ? 1 : 0;
    static xc_smb_url u;
    if (argc - i != 1 || xc_smb_url_parse(argv[i], strlen(argv[i]), &u) != XC_OK) {
        xc_cred_free(&c);
        return 2;
    }
    xc_smb_conn *conn = NULL;
    char detail[XC_SMB_DETAIL_MAX];
    xc_err err = xc_smb_connect_url(&u, have ? &c : NULL, &conn, detail, sizeof detail);
    if (err != XC_OK) {
        xc_cred_free(&c);
        return fail(argv[i], err, detail);
    }
    if (tree) {
        xc_smb_listing l;
        memset(&l, 0, sizeof l);
        err = xc_smb_list_tree(conn, u.path, &l);
        size_t games = 0;
        for (size_t k = 0; err == XC_OK && k < l.count; k++) {
            if (l.files[k].game) {
                games++;
                (void)printf("%15s  %s/  (game folder)\n", "<folder>",
                             l.files[k].path[0] != '\0' ? l.files[k].path : ".");
                continue;
            }
            char name[XC_SMB_MAX_NAME + 1];
            bool zip = false;
            (void)xc_smb_image_name(l.files[k].path, name, sizeof name, &zip);
            (void)printf("%15" PRIu64 "  %s  -> \"%s\"%s\n", l.files[k].size, l.files[k].path, name,
                         zip ? " (zip)" : "");
        }
        if (err == XC_OK) {
            (void)printf("%zu image%s, %zu game folder%s%s\n", l.count - games,
                         l.count - games == 1 ? "" : "s", games, games == 1 ? "" : "s",
                         l.truncated ? " (truncated)" : "");
        }
        xc_smb_listing_free(&l);
    } else {
        err = xc_smb_list(conn, u.path, print_entry, NULL);
    }
    int rc = err == XC_OK ? 0 : fail(argv[i], err, xc_smb_detail(conn));
    xc_smb_disconnect(conn);
    xc_cred_free(&c);
    return rc;
}

/* Prints the credentials section for a login (smb.md §4) from a password on stdin. */
static int cmd_hash(int argc, char **argv)
{
    if (argc > 4) {
        return 2;
    }
    char pw[XC_SMB_MAX_PASSWORD + 2];
    if (fgets(pw, sizeof pw, stdin) == NULL) {
        pw[0] = '\0';
    }
    pw[strcspn(pw, "\r\n")] = '\0';
    char hex[XC_SMB_NTHASH_HEX + 1];
    xc_err err = xc_smb_nthash(pw, hex);
    memset(pw, 0, sizeof pw);
    if (err != XC_OK) {
        return fail("password", err, "UTF-8, at most 256 bytes");
    }
    if (argc >= 3) {
        char sec[512];
        err = xc_cred_smb_section("nas", "NAS-ADDRESS", argv[2], argc == 4 ? argv[3] : NULL, hex,
                                  sec, sizeof sec);
        if (err != XC_OK) {
            return fail("section", err, NULL);
        }
        (void)printf("%s", sec);
        return 0;
    }
    (void)printf("%s\n", hex);
    return 0;
}

int xcat_cli_smb(int argc, char **argv, bool *handled)
{
    /* A NAS that drops the connection must give a write error the client can reconnect after,
       not a SIGPIPE that ends the program (libsmb2 writes with writev). */
    (void)signal(SIGPIPE, SIG_IGN);
    *handled = true;
    if (argc >= 2 && strcmp(argv[1], "smb-scan") == 0) {
        return cmd_scan(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "smb-shares") == 0) {
        return cmd_shares(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "smb-ls") == 0) {
        return cmd_ls(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "smb-hash") == 0) {
        return cmd_hash(argc, argv);
    }
    *handled = false;
    return 2;
}

xc_err xcat_cli_smb_list(void *ctx, const xc_smb_url *folder, xc_smb_listing *out)
{
    const xc_credentials *creds = ctx;
    xc_smb_conn *conn = NULL;
    char detail[XC_SMB_DETAIL_MAX];
    xc_err err = xc_smb_connect_url(folder, creds, &conn, detail, sizeof detail);
    if (err != XC_OK) {
        (void)fprintf(stderr, "  @smb %s: %s\n", folder->host, detail);
        return err;
    }
    err = xc_smb_list_tree(conn, folder->path, out);
    if (err != XC_OK) {
        (void)fprintf(stderr, "  @smb %s: %s\n", folder->host, xc_smb_detail(conn));
    }
    xc_smb_disconnect(conn);
    return err;
}

typedef struct smb_handle {
    xc_smb_conn *conn;
    xc_smb_file *file;
    xc_smb_game *game; /* a game folder (smb.md §3.2) instead of a file */
} smb_handle;

xc_err xcat_cli_smb_open(const char *url, const xc_credentials *creds, xc_source *src,
                         void **handle)
{
    static xc_smb_url u;
    *handle = NULL;
    xc_err err = xc_smb_url_parse(url, strlen(url), &u);
    if (err != XC_OK) {
        return err;
    }
    size_t len = strlen(url);
    bool folder = len > 0 && url[len - 1] == '/'; /* smb.md §3.1 */
    smb_handle *h = calloc(1, sizeof *h);
    if (h == NULL) {
        return XC_ERR_NOMEM;
    }
    char detail[XC_SMB_DETAIL_MAX];
    err = xc_smb_connect_url(&u, creds, &h->conn, detail, sizeof detail);
    if (err == XC_OK && folder) {
        err = xc_smb_open_game(h->conn, u.path, &h->game, src, detail, sizeof detail);
        if (err == XC_OK) {
            (void)fprintf(stderr, "  game folder: %zu files, %" PRIu64 " bytes\n",
                          xc_smb_game_files(h->game), xc_smb_game_size(h->game));
        }
    } else if (err == XC_OK) {
        err = xc_smb_open(h->conn, u.path, &h->file, src);
        if (err != XC_OK) {
            (void)snprintf(detail, sizeof detail, "%s", xc_smb_detail(h->conn));
        }
    }
    if (err != XC_OK) {
        (void)fprintf(stderr, "  %s: %s\n", u.host, detail);
        xc_smb_disconnect(h->conn);
        free(h);
        return err;
    }
    *handle = h;
    return XC_OK;
}

const char *xcat_cli_smb_validator(void *handle)
{
    const smb_handle *h = handle;
    return h != NULL && h->game != NULL ? xc_smb_game_validator(h->game) : NULL;
}

void xcat_cli_smb_close(void *handle)
{
    smb_handle *h = handle;
    if (h == NULL) {
        return;
    }
    if (h->file != NULL) {
        (void)fprintf(stderr, "  SMB: %" PRIu64 " bytes read, %u reconnection%s\n",
                      xc_smb_file_bytes(h->file), xc_smb_file_reconnects(h->file),
                      xc_smb_file_reconnects(h->file) == 1 ? "" : "s");
    }
    if (h->game != NULL) {
        (void)fprintf(stderr, "  SMB: %" PRIu64 " bytes read, %u reconnection%s\n",
                      xc_smb_game_bytes(h->game), xc_smb_game_reconnects(h->game),
                      xc_smb_game_reconnects(h->game) == 1 ? "" : "s");
        if (xc_smb_game_detail(h->game)[0] != '\0') {
            (void)fprintf(stderr, "  last problem: %s\n", xc_smb_game_detail(h->game));
        }
    }
    xc_smb_close_game(h->game);
    xc_smb_close(h->file);
    xc_smb_disconnect(h->conn);
    free(h);
}
