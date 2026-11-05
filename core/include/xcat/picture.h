#ifndef XCAT_PICTURE_H
#define XCAT_PICTURE_H

/*
 * Pictures for a game's details (docs/formats/art.md): a cover in JPEG or PNG, or an XBE's
 * title image, an XPR0 texture (docs/formats/xpr.md), decoded to pixels and fitted to the
 * panel. Every input is untrusted: it comes from a NAS or a game.
 */

#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* art.md §4: limits on what's decoded. */
#define XC_PICTURE_MAX_FILE (4U << 20)        /* bytes of a cover's file */
#define XC_PICTURE_MAX_SIDE 2048U             /* pixels, either way */
#define XC_PICTURE_MAX_PIXELS (1536U * 1536U) /* width times height */
#define XC_PICTURE_MAX_MEMORY (32U << 20)     /* what decoding one may allocate in all */

/* Pixels 0xAARRGGBB, row by row from the top, w * h of them; px NULL and w, h 0 if empty. */
typedef struct xc_picture {
    uint32_t *px;
    uint32_t w;
    uint32_t h;
} xc_picture;

/* Frees the pixels and empties `p`; safe on an empty picture. */
void xc_picture_free(xc_picture *p);

/*
 * art.md §4: decodes a JPEG (baseline or progressive) or a PNG, told apart by their first
 * bytes, not by the file's name. XC_ERR_UNSUPPORTED if it's neither; XC_ERR_LIMIT if the
 * file, its size or its memory is over the limits above; XC_ERR_CORRUPT if it can't be
 * decoded; XC_ERR_NOMEM. On failure `out` is empty.
 */
xc_err xc_picture_decode(const uint8_t *data, size_t len, xc_picture *out);

/*
 * xpr.md: decodes an XPR0 texture, the first level of its first resource. XC_ERR_CORRUPT if
 * it isn't one by §2, XC_ERR_UNSUPPORTED for a texture format §3 doesn't decode, XC_ERR_LIMIT
 * for one over XC_XPR_MAX_SIDE, XC_ERR_NOMEM. On failure `out` is empty.
 */
#define XC_XPR_MAX_SIDE 1024U
xc_err xc_picture_xpr(const uint8_t *data, size_t len, xc_picture *out);

/*
 * art.md §5: `in` scaled to fit inside box_w x box_h with its shape kept, at most `max_up`
 * times its size (1: never larger), at least 1 x 1, laid over the colour `bg` (0xRRGGBB) so
 * it's opaque: every pixel of `out` has alpha 0xFF. XC_ERR_INVALID_ARG for an empty picture
 * or box, XC_ERR_NOMEM.
 */
xc_err xc_picture_fit(const xc_picture *in, uint32_t box_w, uint32_t box_h, uint32_t max_up,
                      uint32_t bg, xc_picture *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_PICTURE_H */
