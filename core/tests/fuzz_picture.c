/*
 * libFuzzer target for pictures (docs/formats/art.md §4-§5): a cover in JPEG or PNG, decoded by
 * stb_image within picture.c's limits, then fitted to the details panel's box.
 *
 * Decoding must give a picture within the limits, or one of the errors xc_picture_decode
 * names, never a crash or a leak; fitting a decoded picture must give an opaque one inside the
 * box. A violation aborts. Seeds are in core/tests/fuzz-seeds/picture/ (copies of the fixtures
 * of core/tests/data/art/).
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
    xc_err err = xc_picture_decode(data, size, &p);
    check(err == XC_OK || err == XC_ERR_UNSUPPORTED || err == XC_ERR_LIMIT ||
          err == XC_ERR_CORRUPT || err == XC_ERR_NOMEM);
    if (err != XC_OK) {
        check(p.px == NULL && p.w == 0 && p.h == 0);
        return 0;
    }
    check(p.px != NULL && p.w > 0 && p.h > 0 && p.w <= XC_PICTURE_MAX_SIDE &&
          p.h <= XC_PICTURE_MAX_SIDE && (uint64_t)p.w * p.h <= XC_PICTURE_MAX_PIXELS);
    xc_picture f;
    check(xc_picture_fit(&p, 150, 130, 2, 0x123456U, &f) == XC_OK);
    check(f.w >= 1 && f.w <= 150 && f.h >= 1 && f.h <= 130);
    for (size_t i = 0; i < (size_t)f.w * f.h; i++) {
        check((f.px[i] >> 24U) == 0xFFU);
    }
    xc_picture_free(&f);
    xc_picture_free(&p);
    return 0;
}
