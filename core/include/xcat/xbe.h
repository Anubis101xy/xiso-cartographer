#ifndef XCAT_XBE_H
#define XCAT_XBE_H

/*
 * The certificate of an Xbox executable (docs/formats/xbe.md): the title ID and name of an
 * installed game's default.xbe, and for a game's details its region, disc and title image.
 * Only header fields and that image's section are read; the file is untrusted.
 */

#include <stdbool.h>
#include <stdint.h>

#include "xcat/error.h"
#include "xcat/source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_XBE_TITLE_UNITS 40 /* §2: UTF-16 code units in the title name */

typedef struct xc_xbe_cert {
    uint32_t title_id;
    char title[3 * XC_XBE_TITLE_UNITS + 1]; /* §3: UTF-8, may be "" */
} xc_xbe_cert;

/* Reads the certificate. XC_ERR_CORRUPT if the file isn't an XBE by §3; errors from the
 * source as they come. */
xc_err xc_xbe_read_cert(const xc_source *src, xc_xbe_cert *cert);

#define XC_XBE_MAX_HEADERS (64U << 10) /* §5: headers read for the sections, at most */
#define XC_XBE_MAX_SECTIONS 512U       /* §5: more and the sections aren't looked at */
#define XC_XBE_MAX_IMAGE (256U << 10)  /* §5: a larger title image section is ignored */

/* §4: the game region's bits. */
#define XC_XBE_REGION_NA 0x1U           /* North America (NTSC-U) */
#define XC_XBE_REGION_JAPAN 0x2U        /* Japan (NTSC-J) */
#define XC_XBE_REGION_WORLD 0x4U        /* the rest of the world (PAL) */
#define XC_XBE_REGION_DEBUG 0x80000000U /* manufacturing / debug */

/* What the details of a game show from its default.xbe (docs/formats/xbe.md §4, §5). */
typedef struct xc_xbe_info {
    xc_xbe_cert cert;
    bool has_region;  /* §4: the certificate reaches region, disc and version */
    uint32_t region;  /* XC_XBE_REGION_* bits, as stored */
    uint32_t disc;    /* the disk number as stored: 0 for the first disc */
    uint32_t version; /* the certificate's version */
    /* §5: the title image's section ($$XTIMAGE, else $$XSIMAGE): its bytes in the file, or
       size 0 if there's none that §5 accepts. */
    uint32_t image_offset;
    uint32_t image_size;
} xc_xbe_info;

/* xc_xbe_read_cert, then §4's fields and §5's title image section, which are left zero if the
 * file doesn't have them (it's still an XBE). XC_ERR_NOMEM if the headers can't be read into
 * memory. */
xc_err xc_xbe_read_info(const xc_source *src, xc_xbe_info *info);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_XBE_H */
