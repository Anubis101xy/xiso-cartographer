/* The SMB client over libsmb2 (xcat/smb_client.h, docs/formats/smb.md). */
#include "xcat/smb_client.h"
#include "xcat/folder_image.h"
#include "xcat/smb_scan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NXDK
/* libsmb2's public header names fd_set: lwIP has it, nxdk's C library doesn't. */
#include <lwip/sockets.h>
#endif

/* libsmb2's headers depend on each other in this order. */
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-share-enum.h>
// clang-format on

#ifndef O_RDONLY
#define O_RDONLY 0 /* as libsmb2 reads open() flags */
#endif

struct xc_smb_conn {
    struct smb2_context *smb2;
    char host[XC_SMB_MAX_HOST + 1];
    uint16_t port;
    char share[XC_SMB_MAX_SHARE + 1];
    char user[XC_SMB_MAX_USER + 1];
    char domain[XC_SMB_MAX_DOMAIN + 1];
    char password[5 + XC_SMB_NTHASH_HEX + 1]; /* "ntlm:<hex>", or "" */
    char detail[XC_SMB_DETAIL_MAX];
};

struct xc_smb_file {
    xc_smb_conn *conn;
    struct smb2fh *fh;
    char path[XC_SMB_MAX_PATH + 1];
    uint64_t size;
    uint64_t bytes;
    unsigned reconnects;
};

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    for (size_t i = 0; i < n; i++) {
        v[i] = 0;
    }
}

static void say(char *out, size_t cap, const char *s)
{
    if (out != NULL && cap > 0) {
        (void)snprintf(out, cap, "%s", s != NULL ? s : "");
    }
}

/* "a: b" (or just b when a is ""), cut to fit: a message for the log. */
static void say2(char *out, size_t cap, const char *a, const char *b)
{
    if (out == NULL || cap == 0) {
        return;
    }
    int n = snprintf(out, cap, "%s%s%s", a, a[0] != '\0' ? ": " : "", b);
    if (n < 0) {
        out[0] = '\0';
    }
}

void xc_smb_login_for(const xc_credentials *creds, const char *host, uint16_t port,
                      xc_smb_login *out)
{
    memset(out, 0, sizeof *out);
    const xc_cred_section *s = creds != NULL ? xc_cred_for_smb(creds, host, port) : NULL;
    if (s != NULL) {
        out->user = s->smb_user;
        out->domain = s->smb_domain;
        out->nthash = s->smb_nthash;
    }
}

/* smb.md §4: what a libsmb2 failure means to the user. */
static xc_err map_error(struct smb2_context *smb2, int rc)
{
    switch ((uint32_t)smb2_get_nterror(smb2)) {
    case SMB2_STATUS_LOGON_FAILURE:
    case SMB2_STATUS_WRONG_PASSWORD:
    case SMB2_STATUS_NO_SUCH_USER:
    case SMB2_STATUS_ACCOUNT_DISABLED:
    case SMB2_STATUS_ACCOUNT_RESTRICTION:
    case SMB2_STATUS_ACCOUNT_EXPIRED:
    case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
    case SMB2_STATUS_PASSWORD_EXPIRED:
    case SMB2_STATUS_PASSWORD_MUST_CHANGE:
    case SMB2_STATUS_INVALID_LOGON_HOURS:
        return XC_ERR_SMB_LOGIN;
    case SMB2_STATUS_BAD_NETWORK_NAME:
        return XC_ERR_SMB_SHARE;
    case SMB2_STATUS_ACCESS_DENIED:
    case SMB2_STATUS_NETWORK_ACCESS_DENIED:
        return XC_ERR_SMB_DENIED;
    case SMB2_STATUS_OBJECT_NAME_NOT_FOUND:
    case SMB2_STATUS_OBJECT_PATH_NOT_FOUND:
    case SMB2_STATUS_NO_SUCH_FILE:
    case SMB2_STATUS_NOT_FOUND:
        return XC_ERR_SMB_NOT_FOUND;
    default:
        break;
    }
    if (rc == -1) {
        /* libsmb2's synchronous calls give a bare -1 when the connection itself failed (a
           broken pipe, a reset, no answer in time): not -EPERM. */
        const char *why = smb2_get_error(smb2);
        return why != NULL && strstr(why, "imeout") != NULL ? XC_ERR_NET_TIMEOUT : XC_ERR_NET_IO;
    }
    switch (-rc) {
    case ENOMEM:
        return XC_ERR_NOMEM;
    case ETIMEDOUT:
        return XC_ERR_NET_TIMEOUT;
    case ECONNREFUSED:
#ifdef EHOSTUNREACH
    case EHOSTUNREACH:
#endif
#ifdef ENETUNREACH
    case ENETUNREACH:
#endif
        return XC_ERR_NET_CONNECT;
    case ECONNRESET:
    case EPIPE:
        return XC_ERR_NET_IO;
    case ENOENT:
        return XC_ERR_SMB_NOT_FOUND;
    case EACCES:
        return XC_ERR_SMB_DENIED;
    default:
        return XC_ERR_SMB;
    }
}

static void keep_detail(xc_smb_conn *c)
{
    say(c->detail, sizeof c->detail, smb2_get_error(c->smb2));
    size_t n = strlen(c->detail);
    while (n > 0 &&
           (c->detail[n - 1] == '\n' || c->detail[n - 1] == '\r' || c->detail[n - 1] == ' ')) {
        c->detail[--n] = '\0';
    }
}

/* A new context for c's server and login. */
static struct smb2_context *new_context(const xc_smb_conn *c)
{
    struct smb2_context *smb2 = smb2_init_context();
    if (smb2 == NULL) {
        return NULL;
    }
    smb2_set_timeout(smb2, XC_SMB_TIMEOUT_S);
    smb2_set_workstation(smb2, "XBOX");
    smb2_set_user(smb2, c->user[0] != '\0' ? c->user : "guest");
    smb2_set_password(smb2, c->password);
    if (c->domain[0] != '\0') {
        smb2_set_domain(smb2, c->domain);
    }
    return smb2;
}

#define PROBE_MS 3000 /* a NAS that's off fails this fast (xc_smb_probe) */

/* Connects c->smb2 to c->share. */
static xc_err open_share(xc_smb_conn *c, const char *share)
{
    /* An address: is anything there? (A name is left to libsmb2's own lookup.) */
    xc_err probe = xc_smb_probe(c->host, c->port, PROBE_MS);
    if (probe == XC_ERR_NET_CONNECT || probe == XC_ERR_NET_IO) {
        say(c->detail, sizeof c->detail, "no answer on the SMB port (is the NAS on?)");
        return XC_ERR_NET_CONNECT;
    }
    c->smb2 = new_context(c);
    if (c->smb2 == NULL) {
        say(c->detail, sizeof c->detail, "out of memory");
        return XC_ERR_NOMEM;
    }
    char server[XC_SMB_MAX_HOST + 8];
    if (c->port == XC_SMB_PORT) {
        (void)snprintf(server, sizeof server, "%s", c->host);
    } else {
        (void)snprintf(server, sizeof server, "%s:%u", c->host, (unsigned)c->port);
    }
    int rc = smb2_connect_share(c->smb2, server, share, NULL);
    if (rc < 0) {
        keep_detail(c);
        xc_err err = map_error(c->smb2, rc);
        smb2_destroy_context(c->smb2);
        c->smb2 = NULL;
        return err;
    }
    return XC_OK;
}

static xc_err new_conn(const char *host, uint16_t port, const char *share,
                       const xc_smb_login *login, xc_smb_conn **out)
{
    *out = NULL;
    if (host == NULL || share == NULL || login == NULL || strlen(host) > XC_SMB_MAX_HOST ||
        strlen(share) > XC_SMB_MAX_SHARE ||
        (login->user != NULL && strlen(login->user) > XC_SMB_MAX_USER) ||
        (login->domain != NULL && strlen(login->domain) > XC_SMB_MAX_DOMAIN) ||
        (login->nthash != NULL && !xc_smb_valid_nthash(login->nthash))) {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_conn *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return XC_ERR_NOMEM;
    }
    (void)snprintf(c->host, sizeof c->host, "%s", host);
    c->port = port != 0 ? port : XC_SMB_PORT;
    (void)snprintf(c->share, sizeof c->share, "%s", share);
    bool user = login->user != NULL && login->user[0] != '\0';
    if (user) {
        (void)snprintf(c->user, sizeof c->user, "%s", login->user);
        if (login->domain != NULL) {
            (void)snprintf(c->domain, sizeof c->domain, "%s", login->domain);
        }
        if (login->nthash != NULL) {
            /* libsmb2 takes the NT hash for the password as "ntlm:<32 hex digits>". */
            (void)snprintf(c->password, sizeof c->password, "ntlm:%s", login->nthash);
        }
    }
    *out = c;
    return XC_OK;
}

static void free_conn(xc_smb_conn *c)
{
    if (c->smb2 != NULL) {
        (void)smb2_disconnect_share(c->smb2);
        smb2_destroy_context(c->smb2);
    }
    wipe(c, sizeof *c);
    free(c);
}

xc_err xc_smb_connect(const char *host, uint16_t port, const char *share, const xc_smb_login *login,
                      xc_smb_conn **out, char *detail, size_t detail_cap)
{
    say(detail, detail_cap, "");
    if (out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_conn *c = NULL;
    xc_err err = new_conn(host, port, share, login, &c);
    if (err != XC_OK) {
        *out = NULL;
        return err;
    }
    err = open_share(c, share);
    if (err != XC_OK) {
        say(detail, detail_cap, c->detail);
        free_conn(c);
        *out = NULL;
        return err;
    }
    *out = c;
    return XC_OK;
}

xc_err xc_smb_connect_url(const xc_smb_url *u, const xc_credentials *creds, xc_smb_conn **out,
                          char *detail, size_t detail_cap)
{
    if (u == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_login login;
    xc_smb_login_for(creds, u->host, u->port, &login);
    return xc_smb_connect(u->host, u->port, u->share, &login, out, detail, detail_cap);
}

void xc_smb_disconnect(xc_smb_conn *c)
{
    if (c != NULL) {
        free_conn(c);
    }
}

const char *xc_smb_detail(const xc_smb_conn *c)
{
    return c != NULL ? c->detail : "";
}

xc_err xc_smb_shares(const char *host, uint16_t port, const xc_smb_login *login, xc_smb_share_fn fn,
                     void *ctx, char *detail, size_t detail_cap)
{
    say(detail, detail_cap, "");
    if (fn == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_conn *c = NULL;
    xc_err err = new_conn(host, port, "IPC$", login, &c);
    if (err != XC_OK) {
        return err;
    }
    err = open_share(c, "IPC$");
    if (err == XC_OK) {
        struct smb2_share_enum_reply *r = smb2_share_enum_sync(c->smb2, SMB2_SHARE_INFO_1);
        if (r == NULL) {
            keep_detail(c);
            err = map_error(c->smb2, -EIO);
        } else {
            size_t listed = 0;
            for (uint32_t i = 0; i < r->entries_read && listed < XC_SMB_MAX_SHARES; i++) {
                const struct smb2_share_info_1 *s = &r->share_info.info_1[i];
                if (s->netname == NULL) {
                    continue;
                }
                size_t n = strlen(s->netname);
                bool hidden =
                    (s->type & SMB2_SHARE_TYPE_HIDDEN) != 0 || (n > 0 && s->netname[n - 1] == '$');
                if ((s->type & 3U) != SMB2_SHARE_TYPE_DISKTREE || hidden ||
                    !xc_smb_valid_name(s->netname, n) || n > XC_SMB_MAX_SHARE) {
                    continue;
                }
                listed++;
                if (!fn(ctx, s->netname, s->remark != NULL ? s->remark : "")) {
                    break;
                }
            }
            smb2_free_data(c->smb2, r);
        }
    }
    say(detail, detail_cap, c->detail);
    free_conn(c);
    return err;
}

/* One folder's entries but "." and "..", with when each last changed (ns since 1970). */
typedef bool (*entry_time_fn)(void *ctx, const char *name, bool is_dir, uint64_t size,
                              uint64_t mtime);

static xc_err list_dir(xc_smb_conn *c, const char *path, entry_time_fn fn, void *ctx)
{
    if (c == NULL || c->smb2 == NULL || path == NULL || fn == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    smb2_set_error(c->smb2, ""); /* no status left from an earlier request (map_error) */
    struct smb2dir *dir = smb2_opendir(c->smb2, path);
    if (dir == NULL) {
        keep_detail(c);
        return map_error(c->smb2, -EIO);
    }
    struct smb2dirent *e;
    while ((e = smb2_readdir(c->smb2, dir)) != NULL) {
        if (e->name == NULL || strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) {
            continue;
        }
        bool is_dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
        uint64_t mtime = (uint64_t)e->st.smb2_mtime * 1000000000U + e->st.smb2_mtime_nsec;
        if (!fn(ctx, e->name, is_dir, is_dir ? 0 : e->st.smb2_size, mtime)) {
            break;
        }
    }
    smb2_closedir(c->smb2, dir);
    return XC_OK;
}

typedef struct plain_list {
    xc_smb_entry_fn fn;
    void *ctx;
} plain_list;

static bool plain_entry(void *ctx, const char *name, bool is_dir, uint64_t size, uint64_t mtime)
{
    (void)mtime;
    const plain_list *p = ctx;
    return p->fn(p->ctx, name, is_dir, size);
}

xc_err xc_smb_list(xc_smb_conn *c, const char *path, xc_smb_entry_fn fn, void *ctx)
{
    if (fn == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    plain_list p = {fn, ctx};
    return list_dir(c, path, plain_entry, &p);
}

bool xc_smb_skipped_folder(const char *name)
{
    if (name[0] == '.' || name[0] == '@') {
        return true;
    }
    static const char *const bins[] = {"#recycle", "$recycle.bin", "#snapshot", "lost+found"};
    for (size_t i = 0; i < sizeof bins / sizeof bins[0]; i++) {
        const char *a = name;
        const char *b = bins[i];
        while (*a != '\0' && *b != '\0' &&
               (*a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a) == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return true;
        }
    }
    return false;
}

/* What xc_smb_list_tree gathers in one folder before going into its folders. */
typedef struct tree_walk {
    xc_smb_listing *out;
    const char *rel; /* the folder's path below the listed one, "" at the top */
    char (*dirs)[XC_SMB_MAX_NAME + 1];
    size_t dir_count;
    size_t dir_cap;
    bool has_xbe; /* a game folder (§3.1) */
    xc_err err;
} tree_walk;

static bool tree_entry(void *ctx, const char *name, bool is_dir, uint64_t size)
{
    tree_walk *w = ctx;
    size_t n = strlen(name);
    if (!xc_smb_valid_name(name, n)) {
        return true; /* a name an smb:// URL can't hold: left out */
    }
    if (is_dir) {
        if (!xc_smb_skipped_folder(name) && w->dir_count < w->dir_cap) {
            memcpy(w->dirs[w->dir_count++], name, n + 1);
        }
        return true;
    }
    if (xc_smb_is_default_xbe(name)) {
        w->has_xbe = true;
        return true;
    }
    char image[XC_SMB_MAX_NAME + 1];
    if (!xc_smb_image_name(name, image, sizeof image, NULL)) {
        return true; /* only images go into the listing (§3) */
    }
    char path[XC_SMB_MAX_PATH + 1];
    int len = w->rel[0] != '\0' ? snprintf(path, sizeof path, "%s/%s", w->rel, name)
                                : snprintf(path, sizeof path, "%s", name);
    if (len < 0 || (size_t)len >= sizeof path) {
        return true;
    }
    w->err = xc_smb_listing_add(w->out, path, size);
    return w->err == XC_OK && !w->out->truncated;
}

#define TREE_DIRS 256 /* folders gone into per folder */

static xc_err list_level(xc_smb_conn *c, const char *base, const char *rel, int depth,
                         xc_smb_listing *out)
{
    char path[XC_SMB_MAX_PATH + 1];
    int len = base[0] != '\0' && rel[0] != '\0' ? snprintf(path, sizeof path, "%s/%s", base, rel)
                                                : snprintf(path, sizeof path, "%s%s", base, rel);
    if (len < 0 || (size_t)len >= sizeof path) {
        return XC_OK;
    }
    tree_walk w = {out, rel, NULL, 0, depth + 1 < XC_SMB_LIST_DEPTH ? TREE_DIRS : 0, false, XC_OK};
    if (w.dir_cap > 0) {
        w.dirs = malloc(w.dir_cap * sizeof *w.dirs);
        if (w.dirs == NULL) {
            return XC_ERR_NOMEM;
        }
    }
    size_t before = out->count;
    xc_err err = xc_smb_list(c, path, tree_entry, &w);
    if (err == XC_OK) {
        err = w.err;
    }
    if (err == XC_OK && w.has_xbe) {
        /* A game folder (§3.1): one entry, and nothing inside it is an image of its own. */
        xc_smb_listing_cut(out, before);
        free(w.dirs);
        return xc_smb_listing_add_game(out, rel);
    }
    for (size_t i = 0; err == XC_OK && i < w.dir_count && !out->truncated; i++) {
        char sub[XC_SMB_MAX_PATH + 1];
        len = rel[0] != '\0' ? snprintf(sub, sizeof sub, "%s/%s", rel, w.dirs[i])
                             : snprintf(sub, sizeof sub, "%s", w.dirs[i]);
        if (len < 0 || (size_t)len >= sizeof sub) {
            continue;
        }
        err = list_level(c, base, sub, depth + 1, out);
        if (err == XC_ERR_SMB_DENIED || err == XC_ERR_SMB_NOT_FOUND) {
            err = XC_OK; /* a folder this login may not open, or gone meanwhile: skipped */
        }
    }
    free(w.dirs);
    return err;
}

xc_err xc_smb_list_tree(xc_smb_conn *c, const char *path, xc_smb_listing *out)
{
    if (c == NULL || path == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    return list_level(c, path, "", 0, out);
}

/* --- files ------------------------------------------------------------------------------ */

static xc_err open_handle(xc_smb_file *f)
{
    xc_smb_conn *c = f->conn;
    smb2_set_error(c->smb2, "");
    f->fh = smb2_open(c->smb2, f->path, O_RDONLY);
    if (f->fh == NULL) {
        keep_detail(c);
        return map_error(c->smb2, -EIO);
    }
    /* The size the server gave in its answer to the open (no request of its own: some servers
       don't answer every information class a stat asks for). */
    uint64_t end = 0;
    int64_t rc = smb2_lseek(c->smb2, f->fh, 0, SEEK_END, &end);
    if (rc < 0) {
        keep_detail(c);
        (void)smb2_close(c->smb2, f->fh);
        f->fh = NULL;
        return XC_ERR_SMB;
    }
    f->size = end;
    return XC_OK;
}

static void closed(struct smb2_context *smb2, int status, void *data, void *cb_data)
{
    (void)smb2;
    (void)status;
    (void)data;
    (void)cb_data;
}

/* After a failed read: a new session and handle, once per failure. */
static xc_err reconnect(xc_smb_file *f)
{
    xc_smb_conn *c = f->conn;
    uint64_t size = f->size;
    if (c->smb2 != NULL) {
        /* The connection is broken: no goodbye goes out. Queuing the handle's close lets
           destroying the context free it (its callback runs with SHUTDOWN). */
        if (f->fh != NULL) {
            (void)smb2_close_async(c->smb2, f->fh, closed, NULL);
        }
        smb2_destroy_context(c->smb2);
        c->smb2 = NULL;
    }
    f->fh = NULL;
    xc_err err = open_share(c, c->share);
    if (err == XC_OK) {
        err = open_handle(f);
    }
    if (err == XC_OK && f->size != size) {
        say(c->detail, sizeof c->detail, "the file changed size while it was read");
        f->size = size;
        err = XC_ERR_SOURCE_SIZE;
    }
    f->reconnects++;
    return err;
}

static bool worth_reconnecting(xc_err err)
{
    return err == XC_ERR_NET_IO || err == XC_ERR_NET_TIMEOUT || err == XC_ERR_NET_CONNECT ||
           err == XC_ERR_SMB;
}

static xc_err file_read_at(void *ctx, uint64_t offset, void *buf, size_t len)
{
    xc_smb_file *f = ctx;
    if (offset > f->size || len > f->size - offset) {
        return XC_ERR_RANGE;
    }
    uint8_t *p = buf;
    size_t done = 0;
    bool retried = false;
    while (done < len) {
        xc_smb_conn *c = f->conn;
        xc_err err = XC_OK;
        int n = -1;
        if (f->fh != NULL && c->smb2 != NULL) {
            uint32_t want = smb2_get_max_read_size(c->smb2);
            if (want == 0 || want > 8U * 1024U * 1024U) {
                want = 1024U * 1024U;
            }
            if ((size_t)want > len - done) {
                want = (uint32_t)(len - done);
            }
            smb2_set_error(c->smb2, "");
            n = smb2_pread(c->smb2, f->fh, p + done, want, offset + done);
            if (n < 0) {
                keep_detail(c);
                err = map_error(c->smb2, n);
            } else if (n == 0) {
                say(c->detail, sizeof c->detail, "the file ended early (changed on the NAS?)");
                return XC_ERR_SOURCE_SIZE;
            }
        } else {
            err = XC_ERR_NET_IO;
        }
        if (err != XC_OK) {
            if (retried || !worth_reconnecting(err)) {
                return err;
            }
            retried = true;
            err = reconnect(f);
            if (err != XC_OK) {
                return err;
            }
            continue;
        }
        done += (size_t)n;
        f->bytes += (uint64_t)n;
        retried = false; /* progress: a later failure may reconnect again */
    }
    return XC_OK;
}

xc_err xc_smb_open(xc_smb_conn *c, const char *path, xc_smb_file **out, xc_source *src)
{
    if (c == NULL || c->smb2 == NULL || path == NULL || out == NULL || src == NULL ||
        strlen(path) > XC_SMB_MAX_PATH || path[0] == '\0') {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_file *f = calloc(1, sizeof *f);
    if (f == NULL) {
        return XC_ERR_NOMEM;
    }
    f->conn = c;
    (void)snprintf(f->path, sizeof f->path, "%s", path);
    xc_err err = open_handle(f);
    if (err != XC_OK) {
        free(f);
        *out = NULL;
        return err;
    }
    memset(src, 0, sizeof *src);
    src->read_at = file_read_at;
    src->ctx = f;
    src->size = f->size;
    *out = f;
    return XC_OK;
}

void xc_smb_close(xc_smb_file *f)
{
    if (f == NULL) {
        return;
    }
    if (f->fh != NULL && f->conn->smb2 != NULL) {
        (void)smb2_close(f->conn->smb2, f->fh);
    }
    free(f);
}

uint64_t xc_smb_file_bytes(const xc_smb_file *f)
{
    return f != NULL ? f->bytes : 0;
}

unsigned xc_smb_file_reconnects(const xc_smb_file *f)
{
    return f != NULL ? f->reconnects : 0;
}

/* --- game folders (smb.md §3.2, docs/formats/folder-image.md) --------------------------- */

struct xc_smb_game {
    xc_smb_conn *conn;
    char base[XC_SMB_MAX_PATH + 1];
    xc_fimg_item *items; /* paths owned */
    size_t count;
    size_t cap;
    xc_fimg *img;
    size_t open_item; /* SIZE_MAX: none */
    xc_smb_file *file;
    xc_source file_src;
    uint64_t bytes_closed; /* read through files already closed */
    unsigned reconnects_closed;
    char detail[XC_SMB_DETAIL_MAX];
    /* while listing */
    size_t parent; /* the folder being listed: an item, or SIZE_MAX for the game folder */
    xc_err err;
    char why[XC_SMB_DETAIL_MAX];
};

static bool game_refuse(xc_smb_game *g, xc_err err, const char *path, const char *why)
{
    g->err = err;
    say2(g->why, sizeof g->why, path, why);
    return false;
}

static bool game_entry(void *ctx, const char *name, bool is_dir, uint64_t size, uint64_t mtime)
{
    xc_smb_game *g = ctx;
    /* §3.2: what NAS systems and other computers leave in folders isn't part of the game. */
    if (name[0] == '.' || (is_dir && xc_smb_skipped_folder(name))) {
        return true;
    }
    const char *parent = g->parent == SIZE_MAX ? "" : g->items[g->parent].path;
    char rel[XC_FIMG_MAX_PATH + 2];
    int len = parent[0] != '\0' ? snprintf(rel, sizeof rel, "%s/%s", parent, name)
                                : snprintf(rel, sizeof rel, "%s", name);
    if (len < 0 || (size_t)len > XC_FIMG_MAX_PATH ||
        strlen(g->base) + 1U + (size_t)len > XC_SMB_MAX_PATH) {
        return game_refuse(g, XC_ERR_GAME_FOLDER, name, "its path is too long");
    }
    if (!xc_smb_valid_name(name, strlen(name))) {
        return game_refuse(g, XC_ERR_GAME_FOLDER, rel,
                           "a name with a character the console can't ask the NAS for");
    }
    if (g->count == XC_FIMG_MAX_ITEMS) {
        return game_refuse(g, XC_ERR_GAME_FOLDER, "", "more than 65,536 files and folders");
    }
    if (g->count == g->cap) {
        size_t cap = g->cap == 0 ? 256 : g->cap * 2;
        xc_fimg_item *grown = realloc(g->items, cap * sizeof *grown);
        if (grown == NULL) {
            g->err = XC_ERR_NOMEM;
            return false;
        }
        g->items = grown;
        g->cap = cap;
    }
    char *copy = malloc((size_t)len + 1U);
    if (copy == NULL) {
        g->err = XC_ERR_NOMEM;
        return false;
    }
    memcpy(copy, rel, (size_t)len + 1U);
    g->items[g->count++] = (xc_fimg_item){copy, is_dir ? 0 : size, mtime, is_dir};
    return true;
}

/* The path on the share of a game's file or folder ("" for the game folder). */
static void game_path(const xc_smb_game *g, const char *rel, char *out, size_t cap)
{
    if (g->base[0] != '\0' && rel[0] != '\0') {
        (void)snprintf(out, cap, "%s/%s", g->base, rel);
    } else {
        (void)snprintf(out, cap, "%s%s", g->base, rel);
    }
}

static void game_close_file(xc_smb_game *g)
{
    if (g->file != NULL) {
        g->bytes_closed += xc_smb_file_bytes(g->file);
        g->reconnects_closed += xc_smb_file_reconnects(g->file);
        xc_smb_close(g->file);
        g->file = NULL;
    }
    g->open_item = SIZE_MAX;
}

static xc_err game_read(void *ctx, size_t item, uint64_t offset, void *buf, size_t len)
{
    xc_smb_game *g = ctx;
    if (item >= g->count || g->items[item].dir) {
        return XC_ERR_INVALID_ARG;
    }
    if (g->open_item != item) {
        game_close_file(g);
        char path[XC_SMB_MAX_PATH + 1];
        game_path(g, g->items[item].path, path, sizeof path);
        xc_err err = xc_smb_open(g->conn, path, &g->file, &g->file_src);
        if (err != XC_OK) {
            g->file = NULL;
            say2(g->detail, sizeof g->detail, g->items[item].path, xc_smb_detail(g->conn));
            return err;
        }
        if (g->file_src.size != g->items[item].size) {
            say2(g->detail, sizeof g->detail, g->items[item].path, "it changed size on the NAS");
            game_close_file(g);
            return XC_ERR_SOURCE_SIZE;
        }
        g->open_item = item;
    }
    xc_err err = g->file_src.read_at(g->file_src.ctx, offset, buf, len);
    if (err != XC_OK) {
        say2(g->detail, sizeof g->detail, g->items[item].path,
             xc_smb_detail(g->conn)[0] != '\0' ? xc_smb_detail(g->conn) : xc_err_str(err));
        game_close_file(g); /* the next read opens it again */
    }
    return err;
}

xc_err xc_smb_open_game(xc_smb_conn *c, const char *path, xc_smb_game **out, xc_source *src,
                        char *why, size_t why_cap)
{
    if (why != NULL && why_cap > 0) {
        why[0] = '\0';
    }
    if (c == NULL || c->smb2 == NULL || path == NULL || out == NULL || src == NULL ||
        strlen(path) > XC_SMB_MAX_PATH) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    xc_smb_game *g = calloc(1, sizeof *g);
    if (g == NULL) {
        return XC_ERR_NOMEM;
    }
    g->conn = c;
    g->open_item = SIZE_MAX;
    (void)snprintf(g->base, sizeof g->base, "%s", path);
    /* Every folder, the game folder first: the items list is also the queue (§3.2). */
    xc_err err = XC_OK;
    for (size_t next = SIZE_MAX;; next = next == SIZE_MAX ? 0 : next + 1U) {
        if (next != SIZE_MAX) {
            while (next < g->count && !g->items[next].dir) {
                next++;
            }
            if (next == g->count) {
                break;
            }
        }
        g->parent = next;
        char dir[XC_SMB_MAX_PATH + 1];
        game_path(g, next == SIZE_MAX ? "" : g->items[next].path, dir, sizeof dir);
        g->err = XC_OK;
        err = list_dir(c, dir, game_entry, g);
        if (err == XC_OK) {
            err = g->err;
        } else {
            say2(g->why, sizeof g->why, next == SIZE_MAX ? "the game folder" : g->items[next].path,
                 c->detail[0] != '\0' ? c->detail : xc_err_str(err));
        }
        if (err != XC_OK) {
            break;
        }
    }
    if (err == XC_OK) {
        xc_fimg_problem problem;
        err = xc_fimg_build(g->items, g->count, game_read, g, &g->img, src, &problem);
        if (err != XC_OK) {
            say2(g->why, sizeof g->why, problem.item < g->count ? g->items[problem.item].path : "",
                 problem.why);
        }
    }
    if (err == XC_OK) {
        bool xbe = false;
        for (size_t i = 0; i < g->count && !xbe; i++) {
            xbe = !g->items[i].dir && xc_smb_is_default_xbe(g->items[i].path);
        }
        if (!xbe) {
            say(g->why, sizeof g->why, "no default.xbe in the folder");
            err = XC_ERR_GAME_FOLDER;
        }
    }
    if (err != XC_OK) {
        say(why, why_cap, g->why);
        xc_smb_close_game(g);
        memset(src, 0, sizeof *src);
        return err;
    }
    *out = g;
    return XC_OK;
}

void xc_smb_close_game(xc_smb_game *g)
{
    if (g == NULL) {
        return;
    }
    game_close_file(g);
    xc_fimg_free(g->img);
    for (size_t i = 0; i < g->count; i++) {
        free((char *)g->items[i].path);
    }
    free(g->items);
    free(g);
}

const char *xc_smb_game_validator(const xc_smb_game *g)
{
    return g != NULL ? xc_fimg_validator(g->img) : "";
}

size_t xc_smb_game_files(const xc_smb_game *g)
{
    return g != NULL ? xc_fimg_file_count(g->img) : 0;
}

uint64_t xc_smb_game_size(const xc_smb_game *g)
{
    return g != NULL ? xc_fimg_file_bytes(g->img) : 0;
}

uint64_t xc_smb_game_bytes(const xc_smb_game *g)
{
    if (g == NULL) {
        return 0;
    }
    return g->bytes_closed + (g->file != NULL ? xc_smb_file_bytes(g->file) : 0);
}

unsigned xc_smb_game_reconnects(const xc_smb_game *g)
{
    if (g == NULL) {
        return 0;
    }
    return g->reconnects_closed + (g->file != NULL ? xc_smb_file_reconnects(g->file) : 0);
}

const char *xc_smb_game_detail(const xc_smb_game *g)
{
    return g != NULL ? g->detail : "";
}
