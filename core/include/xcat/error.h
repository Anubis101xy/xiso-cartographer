#ifndef XCAT_ERROR_H
#define XCAT_ERROR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of every fallible libxcat function. Results come back through out-params. */
typedef enum xc_err {
    XC_OK = 0,
    XC_ERR_INVALID_ARG,     /* caller bug: NULL pointer or impossible argument */
    XC_ERR_NOMEM,           /* allocation failed */
    XC_ERR_IO,              /* the underlying read or write failed */
    XC_ERR_RANGE,           /* a read reached outside the source */
    XC_ERR_NOT_XDVDFS,      /* no Xbox volume descriptor at any known layout */
    XC_ERR_CORRUPT,         /* the data violates its format (or is hostile) */
    XC_ERR_LIMIT,           /* the data exceeds a configured safety limit */
    XC_ERR_CANCELLED,       /* the caller asked to stop (e.g. a progress callback) */
    XC_ERR_NOT_INSTALLABLE, /* the game's files can't be stored on FATX (ADR 0008) */
    XC_ERR_URL,             /* not an accepted http(s) URL (docs/formats/http.md §2) */
    XC_ERR_HTTP_PROTOCOL,   /* the server's response breaks docs/formats/http.md */
    XC_ERR_NET_DNS,         /* the server's name doesn't resolve */
    XC_ERR_NET_CONNECT,     /* no connection could be opened */
    XC_ERR_NET_TIMEOUT,     /* the server stopped responding */
    XC_ERR_NET_IO,          /* the connection failed or closed early */
    XC_ERR_HTTP_NO_RANGE,   /* the server ignores Range (ADR 0004) */
    XC_ERR_HTTP_DENIED,     /* 401/403 (ADR 0006: reported, never worked around) */
    XC_ERR_HTTP_NOT_FOUND,  /* 404/410 */
    XC_ERR_HTTP_STATUS,     /* any other error status; see xc_http_client.last_status */
    XC_ERR_HTTP_REDIRECTS,  /* more than 5 redirects */
    XC_ERR_HTTP_CHANGED,    /* the remote file changed between requests */
    XC_ERR_UNSUPPORTED,     /* not supported by this version (e.g. https before M5) */
    XC_ERR_CREDENTIALS,     /* the credentials file is invalid (credentials.md §7) */
    XC_ERR_MAP,             /* the map file itself is rejected (map.md §7) */
    XC_ERR_NO_SPACE,        /* not enough free space for the install */
    XC_ERR_VERIFY,          /* an installed file doesn't match what was received */
    XC_ERR_FOLDER,          /* no free install folder name (ADR 0008 numbering ran out) */
    XC_ERR_TLS,             /* the TLS handshake or a TLS record failed (not retried) */
    XC_ERR_TLS_CERT,        /* the server's certificate isn't trusted for this host */
    XC_ERR_TLS_CERT_TIME,   /* the certificate isn't valid at the current time (clock?) */
    XC_ERR_TLS_NO_TIME,     /* no acceptable time for certificate checks (ADR 0012 §5) */
    XC_ERR_TLS_NO_RANDOM,   /* not enough randomness to start TLS (ADR 0012 §6) */
    XC_ERR_TLS_NOT_TLS,     /* the server answered, but not in TLS (an http:// server?) */
    XC_ERR_NTP,             /* the time server's answer was refused (docs/formats/sntp.md §4) */
    XC_ERR_IMAGE_HASH,      /* the image's MD5/SHA-1 isn't the expected one (journal.md §7) */
    XC_ERR_NOT_ZIP,         /* no zip end record (docs/formats/zip.md §2) */
    XC_ERR_ZIP_UNSUPPORTED, /* split zip, or a method/encryption we can't read (zip.md §6) */
    XC_ERR_ZIP_MEMBER,      /* the zip holds no image, or several (zip.md §5) */
    XC_ERR_ZIP_CRC,         /* the image in the zip failed its CRC-32 check (zip.md §9) */
    XC_ERR_SOURCE_SIZE,     /* the file isn't the size its listing gives (archiveorg.md §8) */
    XC_ERR_CATALOG,         /* the catalog file breaks docs/formats/catalog.md (§7) */
    XC_ERR_SETTINGS,        /* the settings file is invalid (docs/formats/settings.md §5) */
    XC_ERR_UI_SCRIPT,       /* the UI script is invalid (docs/formats/ui-script.md §5) */
    XC_ERR_SMB_URL,         /* not an accepted smb:// URL (docs/formats/smb.md §1) */
    XC_ERR_SMB_LOGIN,       /* the NAS refused the user name or password (smb.md §4) */
    XC_ERR_SMB_SHARE,       /* the NAS has no share of that name (smb.md §4) */
    XC_ERR_SMB_DENIED,      /* the login may not open that share, folder or file (smb.md §4) */
    XC_ERR_SMB_NOT_FOUND,   /* no such folder or file on the share (smb.md §4) */
    XC_ERR_SMB,             /* any other SMB failure; the log has the server's reason */
    XC_ERR_GAME_FOLDER, /* a game folder holds something an install can't take (folder-image.md §2)
                         */
    XC_ERR_COUNT_       /* number of codes; not an error */
} xc_err;

/* Returns a short, user-readable description. Never NULL, even for unknown values. */
const char *xc_err_str(xc_err err);

/*
 * What the user can do about it, as one sentence for the screen after xc_err_str (ADR 0015
 * decision 5): retry, log in again, fix a file, or go back. "" for XC_OK; never NULL.
 */
const char *xc_err_action(xc_err err);

/* True if trying the same thing again can help without leaving the app: the network, the
   server, or something the user can change meanwhile (free space over FTP). */
bool xc_err_retry(xc_err err);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ERROR_H */
