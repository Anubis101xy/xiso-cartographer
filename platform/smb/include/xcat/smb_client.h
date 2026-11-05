#ifndef XCAT_SMB_CLIENT_H
#define XCAT_SMB_CLIENT_H

/*
 * The SMB client (docs/formats/smb.md, ADR 0018): libsmb2 under a small interface, the same
 * code on the console (nxdk, lwIP) and on PCs (xcat-cli). It connects to a NAS's shared
 * folder with an NT-hash login from the credentials file or as a guest, lists shares and
 * folders, lists an @smb folder for the map, and reads an image as an xc_source for installs.
 * It lives in platform/, not core/, because libsmb2 opens its own sockets.
 *
 * Nothing here is thread-safe: one thread uses a connection at a time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/credentials.h"
#include "xcat/error.h"
#include "xcat/smb.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SMB_TIMEOUT_S 20   /* per request (smb.md §4) */
#define XC_SMB_DETAIL_MAX 160 /* the server's reason, for the log */
#define XC_SMB_MAX_SHARES 64  /* shares listed */

/* Who logs in (smb.md §4). user NULL or "": as a guest. */
typedef struct xc_smb_login {
    const char *user;
    const char *domain; /* NULL: none */
    const char *nthash; /* 32 hex digits; NULL with a user: an empty password */
} xc_smb_login;

/* The login the credentials file gives `host`:`port` (xc_cred_for_smb), or a guest one. The
   strings point into `creds`, which must outlive `out`. `creds` may be NULL. */
void xc_smb_login_for(const xc_credentials *creds, const char *host, uint16_t port,
                      xc_smb_login *out);

/* One session to one shared folder. */
typedef struct xc_smb_conn xc_smb_conn;

/*
 * Connects to `share` on host:port and logs in. The login's strings are copied (and the
 * copies wiped when the connection closes), so the connection can log in again by itself after
 * a dropped connection. XC_ERR_SMB_LOGIN, XC_ERR_SMB_SHARE, XC_ERR_SMB_DENIED,
 * XC_ERR_NET_CONNECT, XC_ERR_NET_TIMEOUT, XC_ERR_SMB...; `detail` (may be NULL) gets the
 * server's reason, never the login.
 */
xc_err xc_smb_connect(const char *host, uint16_t port, const char *share, const xc_smb_login *login,
                      xc_smb_conn **out, char *detail, size_t detail_cap);

/* Connects to the share of an smb:// URL with the login `creds` gives its host. */
xc_err xc_smb_connect_url(const xc_smb_url *u, const xc_credentials *creds, xc_smb_conn **out,
                          char *detail, size_t detail_cap);

void xc_smb_disconnect(xc_smb_conn *c);

/* The server's reason for the last failure on `c`, for the log (never a login). */
const char *xc_smb_detail(const xc_smb_conn *c);

/* Called for each disk share that isn't hidden (smb.md §2): its name and its comment ("" if
   none). Return false to stop. */
typedef bool (*xc_smb_share_fn)(void *ctx, const char *name, const char *remark);

/* Lists the shares of host:port (through its IPC$ share). */
xc_err xc_smb_shares(const char *host, uint16_t port, const xc_smb_login *login, xc_smb_share_fn fn,
                     void *ctx, char *detail, size_t detail_cap);

/* Called for each entry of a folder but "." and "..": its name, whether it's a folder, and
   its size. Return false to stop. */
typedef bool (*xc_smb_entry_fn)(void *ctx, const char *name, bool is_dir, uint64_t size);

/* Lists one folder of the share ("" for its top), '/'-separated. */
xc_err xc_smb_list(xc_smb_conn *c, const char *path, xc_smb_entry_fn fn, void *ctx);

/* True for a folder the listings leave out (smb.md §3): names starting with '.' or '@', and
   the recycle bins NAS systems keep ("#recycle", "$RECYCLE.BIN"). */
bool xc_smb_skipped_folder(const char *name);

/* §3, for @smb: the images of folder `path` and of the folders in it, XC_SMB_LIST_DEPTH levels,
   with their paths below `path`. Folders xc_smb_skipped_folder names are left out. A folder
   with a default.xbe is a game folder (§3.1): it's listed as one, and nothing in it is. */
xc_err xc_smb_list_tree(xc_smb_conn *c, const char *path, xc_smb_listing *out);

/* An image opened for reading. */
typedef struct xc_smb_file xc_smb_file;

/*
 * Opens `path` read-only and fills in `src` to read it (size from the server). After a
 * dropped connection a read connects, logs in and opens the file again once, then retries.
 * `c` must outlive the file.
 */
xc_err xc_smb_open(xc_smb_conn *c, const char *path, xc_smb_file **out, xc_source *src);

void xc_smb_close(xc_smb_file *f);

/* Bytes read through a file since it was opened, and reconnections it made. */
uint64_t xc_smb_file_bytes(const xc_smb_file *f);
unsigned xc_smb_file_reconnects(const xc_smb_file *f);

/* A game folder opened for installing (smb.md §3.2): its files as one image
   (docs/formats/folder-image.md). */
typedef struct xc_smb_game xc_smb_game;

/*
 * Lists every file and folder below `path` (smb.md §3.2) and fills `src` with the image they
 * make; the install reads it like any image, opening each file as it gets to it. `c` must
 * outlive the game. XC_ERR_GAME_FOLDER (`why` says which name and why), the listing's
 * errors, XC_ERR_NOMEM. `why` (may be NULL) gets the reason for any failure.
 */
xc_err xc_smb_open_game(xc_smb_conn *c, const char *path, xc_smb_game **out, xc_source *src,
                        char *why, size_t why_cap);
void xc_smb_close_game(xc_smb_game *g);

/* The listing's validator, the install's ETag (folder-image.md §5). */
const char *xc_smb_game_validator(const xc_smb_game *g);
/* Its files, and their total size. */
size_t xc_smb_game_files(const xc_smb_game *g);
uint64_t xc_smb_game_size(const xc_smb_game *g);
/* Bytes read and reconnections made, over all its files so far. */
uint64_t xc_smb_game_bytes(const xc_smb_game *g);
unsigned xc_smb_game_reconnects(const xc_smb_game *g);
/* Why the last read failed, when the NAS didn't say (a file that changed size). */
const char *xc_smb_game_detail(const xc_smb_game *g);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SMB_CLIENT_H */
