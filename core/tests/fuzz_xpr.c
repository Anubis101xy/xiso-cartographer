/*
 * libFuzzer target for XPR0 textures (docs/formats/xpr.md), an XBE's title image.
 *
 * Decoding must give a picture of at most XC_XPR_MAX_SIDE a side, or one of the errors
 * xc_picture_xpr names. A violation aborts. Seeds are in core/tests/fuzz-seeds/xpr/.
 */
#include <stdint.h>
#include <stdlib.h>

#include "xcat/picture.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_picture p;
    xc_err err = xc_picture_xpr(data, size, &p);
    check(err == XC_OK || err == XC_ERR_UNSUPPORTED || err == XC_ERR_LIMIT ||
          err == XC_ERR_CORRUPT || err == XC_ERR_NOMEM);
    if (err != XC_OK) {
        check(p.px == NULL);
        return 0;
    }
    check(p.w >= 1 && p.h >= 1 && p.w <= XC_XPR_MAX_SIDE && p.h <= XC_XPR_MAX_SIDE);
    uint32_t sum = 0;
    for (size_t i = 0; i < (size_t)p.w * p.h; i++) {
        sum += p.px[i]; /* every pixel was written */
    }
    (void)sum;
    xc_picture_free(&p);
    return 0;
}
