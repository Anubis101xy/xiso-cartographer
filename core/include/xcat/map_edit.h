#ifndef XCAT_MAP_EDIT_H
#define XCAT_MAP_EDIT_H

/*
 * Editing a map file's sources (docs/formats/map.md §9), for SETTINGS' Map sources page: the
 * lines that are entries or directives, a new line built and checked by the reader's own
 * rules, and the file with one line added or removed. Every other byte stays as it was, so
 * comments and blank lines survive. Only the map's own lines are listed, never those of the
 * maps it includes.
 */

#include <stddef.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xc_map_line_kind {
    XC_MAP_LINE_ENTRY,      /* <key> = <url> (§4) */
    XC_MAP_LINE_INCLUDE,    /* @include <location> (§5.1) */
    XC_MAP_LINE_ARCHIVEORG, /* @archiveorg <identifier> (§5.2) */
    XC_MAP_LINE_DIRECTIVE,  /* any other @directive (§5.3) */
    XC_MAP_LINE_SMB,        /* @smb <folder> (§5.4) */
} xc_map_line_kind;

/* One source line. The strings point into the data and are not NUL-terminated. */
typedef struct xc_map_line {
    xc_map_line_kind kind;
    size_t line;      /* 1-based */
    const char *text; /* the line, trimmed */
    size_t text_len;
    const char *arg; /* the URL, location or identifier, trimmed; for entries, after the '=' */
    size_t arg_len;
    size_t key_len; /* entries: the key is text[0, key_len), trimmed */
} xc_map_line;

/*
 * The source lines of the map in data[0, len), in order: up to `max` in out[], and how many
 * there are in all in *count. XC_ERR_MAP if the file has no valid header (§3), or a newer
 * major version; XC_ERR_LIMIT if it's larger than XC_MAP_MAX_FILE.
 */
xc_err xc_map_lines(const char *data, size_t len, xc_map_line *out, size_t max, size_t *count);

/*
 * Builds the line for a new source into out[0, cap) and checks it as the reader would, alone
 * in a map: ENTRY from `key` ("4D530004" or "4D530004@PAL", any case) and the image's `url`;
 * INCLUDE from a map's `url` (http or https only); ARCHIVEORG from an item's identifier in
 * `url`; SMB from a NAS folder's smb:// `url` (checked, not listed: smb.md §1). XC_ERR_MAP if it
 * would be an invalid line, with why[0, why_cap) saying why in words for the screen; XC_ERR_LIMIT
 * if it's longer than XC_MAP_MAX_LINE.
 */
xc_err xc_map_new_line(xc_map_line_kind kind, const char *key, const char *url, char *out,
                       size_t cap, char *why, size_t why_cap);

/*
 * The file with `line` (no line ending) added at the end, or, with no file (data NULL, len
 * 0), a new one: the header, a blank line, then the line. XC_ERR_MAP if the file has no valid
 * header; XC_ERR_LIMIT if the result is larger than XC_MAP_MAX_FILE. On success *out
 * (malloc'd, NUL-terminated, *out_len bytes) is the caller's to free.
 */
xc_err xc_map_add_line(const char *data, size_t len, const char *line, char **out, size_t *out_len);

/*
 * The file without source line number `line` (as xc_map_lines gives it), line ending and
 * all. XC_ERR_INVALID_ARG if that line isn't a source line; XC_ERR_MAP if the file has no
 * valid header. *out as for xc_map_add_line.
 */
xc_err xc_map_remove_line(const char *data, size_t len, size_t line, char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_MAP_EDIT_H */
