#ifndef XCAT_SMB_H
#define XCAT_SMB_H

/*
 * smb:// sources (docs/formats/smb.md, ADR 0018): the addresses of a NAS's shared folders
 * and of the images in them, which files of a folder are images, the NT hash a login is
 * stored as, and the listing a folder gives for the map's @smb directive. Parsing only: the
 * client that talks to the NAS is platform/smb (xcat/smb_client.h), since it opens sockets.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SMB_PORT 445
#define XC_SMB_MAX_URL 1024  /* §1: bytes in an smb:// URL as written */
#define XC_SMB_MAX_HOST 253  /* §1.1 */
#define XC_SMB_MAX_SHARE 80  /* §1.2: bytes in a share name, decoded */
#define XC_SMB_MAX_PATH 768  /* §1.3: bytes in the path inside the share, decoded */
#define XC_SMB_MAX_NAME 255  /* §1.3: bytes in one folder or file name, decoded */
#define XC_SMB_MAX_USER 128  /* §4: bytes in a user name */
#define XC_SMB_MAX_DOMAIN 64 /* §4: bytes in a domain (workgroup) name */
#define XC_SMB_MAX_PASSWORD 256
#define XC_SMB_NTHASH_HEX 32
#define XC_SMB_MAX_LISTED 10000 /* §3: files one @smb folder may give (the map's entry limit) */
#define XC_SMB_LIST_DEPTH 2     /* §3: the folder, and the folders in it */

/* An smb:// URL (§1). */
typedef struct xc_smb_url {
    char host[XC_SMB_MAX_HOST + 1]; /* lowercased */
    uint16_t port;                  /* explicit, or 445 */
    bool is_ipv4;
    char share[XC_SMB_MAX_SHARE + 1]; /* decoded; never empty */
    /* Decoded, its names joined with '/', without a leading or trailing '/'. "" is the share
       itself. */
    char path[XC_SMB_MAX_PATH + 1];
} xc_smb_url;

/* True if `text` starts with "smb://" (the scheme in any case). */
bool xc_smb_is_url(const char *text);

/* Parses `len` bytes of `text` (§1). XC_ERR_SMB_URL if it isn't an accepted smb:// URL. */
xc_err xc_smb_url_parse(const char *text, size_t len, xc_smb_url *out);

/* Writes the canonical form of `u` (§1.4): what parses back to the same URL. XC_ERR_LIMIT if
   it doesn't fit in cap bytes with its NUL. */
xc_err xc_smb_url_format(const xc_smb_url *u, char *out, size_t cap);

/* `dir` with the decoded, '/'-separated path `rel` below it. XC_ERR_SMB_URL if a name in `rel`
   breaks §1.3, or the path gets too long. `out` may be `dir`. */
xc_err xc_smb_url_child(const xc_smb_url *dir, const char *rel, xc_smb_url *out);

/* True if `name` is a valid folder or file name (§1.3): `len` bytes of UTF-8, 1-255 of them,
   with no control character, none of \ / : * ? " < > |, and not "." or "..". */
bool xc_smb_valid_name(const char *name, size_t len);

/* §3: if the file name `name` is an image's (it ends in .xiso.iso, .iso, .xiso or .zip, any
   case), its entry name: without that ending, longest first. False if it isn't one, nothing
   would be left, or out (cap bytes) is too small. *zip says whether it ends in .zip. */
bool xc_smb_image_name(const char *name, char *out, size_t cap, bool *zip);

/* The NT hash of a password (MD4 of its UTF-16LE form, as SMB's NTLM logins use it; §4), as
   32 upper-case hex digits. XC_ERR_INVALID_ARG if the password isn't valid UTF-8 or is longer
   than XC_SMB_MAX_PASSWORD bytes. */
xc_err xc_smb_nthash(const char *password, char hex[XC_SMB_NTHASH_HEX + 1]);

/* True if `hex` is an NT hash as the credentials file holds one: 32 hex digits. */
bool xc_smb_valid_nthash(const char *hex);

/* One image a folder listing found: its path below the folder, '/'-separated, and its size;
   or a game folder (§3.1): its path ("" for the listed folder itself), size 0. */
typedef struct xc_smb_listed {
    char *path; /* owned */
    uint64_t size;
    bool game; /* a game folder, not an image */
} xc_smb_listed;

/* The files of a folder and the folders in it (§3), as the platform lists them for @smb. */
typedef struct xc_smb_listing {
    xc_smb_listed *files;
    size_t count;
    size_t cap;
    bool truncated; /* more than XC_SMB_MAX_LISTED: the rest were left out */
} xc_smb_listing;

/* Adds a copy of `path`. Past XC_SMB_MAX_LISTED files it sets truncated and adds nothing;
   both are XC_OK. XC_ERR_NOMEM. */
xc_err xc_smb_listing_add(xc_smb_listing *l, const char *path, uint64_t size);

/* Adds a game folder (§3.1), as xc_smb_listing_add does a file. */
xc_err xc_smb_listing_add_game(xc_smb_listing *l, const char *path);

/* Removes the entries from `count` on (a folder found to be a game after its images were
   added, §3.1). */
void xc_smb_listing_cut(xc_smb_listing *l, size_t count);

/* §3.1: true if `name` is a game folder's executable, "default.xbe" in any case. */
bool xc_smb_is_default_xbe(const char *name);

/* Frees the files; the listing is then empty. */
void xc_smb_listing_free(xc_smb_listing *l);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SMB_H */
