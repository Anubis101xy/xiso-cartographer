/*
 * libFuzzer target for XBE certificates (docs/formats/xbe.md), and the region, disc and title
 * image section read with them (§4-§5).
 *
 * The input is a default.xbe. A successful read must give a NUL-terminated title of valid
 * UTF-8 with no control characters and no trailing space; anything else must be
 * XC_ERR_CORRUPT. A violation aborts. Seeds are in core/tests/fuzz-seeds/xbe/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/xbe.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_source src;
    xc_mem_source mem;
    xc_source_memory(&src, &mem, data, size);
    xc_xbe_cert cert;
    xc_err err = xc_xbe_read_cert(&src, &cert);
    check(err == XC_OK || err == XC_ERR_CORRUPT);
    const char *nul = memchr(cert.title, '\0', sizeof cert.title);
    check(nul != NULL);
    size_t n = (size_t)(nul - cert.title);
    if (err != XC_OK) {
        check(n == 0 && cert.title_id == 0);
        return 0;
    }
    check(n == 0 || cert.title[n - 1] != ' ');
    /* §4-§5: the same certificate, and a title image inside the file and the limit. */
    xc_xbe_info info;
    check(xc_xbe_read_info(&src, &info) == XC_OK);
    check(info.cert.title_id == cert.title_id && strcmp(info.cert.title, cert.title) == 0);
    check(info.image_size == 0 ||
          (info.image_size >= 0x20U && info.image_size <= XC_XBE_MAX_IMAGE &&
           (uint64_t)info.image_offset + info.image_size <= size));
    check(info.has_region || (info.region == 0 && info.disc == 0 && info.version == 0));
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)cert.title[i];
        size_t k = c < 0x80U ? 1 : (c & 0xE0U) == 0xC0U ? 2 : (c & 0xF0U) == 0xE0U ? 3 : 0;
        check(k > 0 && i + k <= n && (k > 1 || (c >= 0x20U && c != 0x7FU)));
        for (size_t j = 1; j < k; j++) {
            check(((unsigned char)cert.title[i + j] & 0xC0U) == 0x80U);
        }
        i += k;
    }
    return 0;
}
