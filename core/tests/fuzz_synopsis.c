/*
 * libFuzzer target for synopses (docs/formats/synopsis.md), an XBMC4Gamers _resources/default.xml.
 *
 * Every field must come back NUL-terminated inside its room, valid UTF-8 without control
 * characters, without a leading or trailing space or two spaces in a row. A violation aborts.
 * Seeds are in core/tests/fuzz-seeds/synopsis/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/synopsis.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void field(const char *s, size_t cap)
{
    const char *nul = memchr(s, '\0', cap);
    check(nul != NULL);
    size_t n = (size_t)(nul - s);
    check(n == 0 || (s[0] != ' ' && s[n - 1] != ' '));
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80U              ? 1
                   : (c & 0xE0U) == 0xC0U ? 2
                   : (c & 0xF0U) == 0xE0U ? 3
                   : (c & 0xF8U) == 0xF0U ? 4
                                          : 0;
        check(k > 0 && i + k <= n && (k > 1 || (c > 0x20U && c != 0x7FU) || c == ' '));
        check(!(c == ' ' && i + 1 < n && s[i + 1] == ' '));
        for (size_t j = 1; j < k; j++) {
            check(((unsigned char)s[i + j] & 0xC0U) == 0x80U);
        }
        i += k;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_synopsis s;
    xc_err err = xc_synopsis_parse((const char *)data, size, &s);
    check(err == XC_OK || err == XC_ERR_CORRUPT || err == XC_ERR_LIMIT);
    field(s.title, sizeof s.title);
    field(s.developer, sizeof s.developer);
    field(s.publisher, sizeof s.publisher);
    field(s.genre, sizeof s.genre);
    field(s.release_date, sizeof s.release_date);
    field(s.year, sizeof s.year);
    field(s.esrb, sizeof s.esrb);
    field(s.rating, sizeof s.rating);
    field(s.features, sizeof s.features);
    field(s.online, sizeof s.online);
    field(s.title_id, sizeof s.title_id);
    field(s.overview, sizeof s.overview);
    if (err != XC_OK) {
        check(s.title[0] == '\0' && s.overview[0] == '\0');
    }
    return 0;
}
