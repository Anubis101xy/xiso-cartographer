/* XBE certificates (xcat/xbe.h, docs/formats/xbe.md). */
#include "xcat/xbe.h"

#include <stdlib.h>
#include <string.h>

#include "bytes.h"

#define HEADER_MIN 0x178U /* §3: the image header up to the fields we read */
#define CERT_MIN 0x5CU    /* §3: the certificate up to the end of the title name */

/* §3: the title name as UTF-8, unrepresentable units and control characters as '?'. */
static void title_utf8(const uint8_t *units, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < XC_XBE_TITLE_UNITS; i++) {
        uint32_t u = (uint32_t)units[2 * i] | (uint32_t)units[2 * i + 1] << 8U;
        if (u == 0) {
            break;
        }
        if ((u >= 0xD800U && u <= 0xDFFFU) || u < 0x20U || (u >= 0x7FU && u <= 0x9FU)) {
            out[n++] = '?';
        } else if (u < 0x80U) {
            out[n++] = (char)u;
        } else if (u < 0x800U) {
            out[n++] = (char)(0xC0U | (u >> 6U));
            out[n++] = (char)(0x80U | (u & 0x3FU));
        } else {
            out[n++] = (char)(0xE0U | (u >> 12U));
            out[n++] = (char)(0x80U | ((u >> 6U) & 0x3FU));
            out[n++] = (char)(0x80U | (u & 0x3FU));
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

xc_err xc_xbe_read_cert(const xc_source *src, xc_xbe_cert *cert)
{
    memset(cert, 0, sizeof *cert);
    if (src->size < HEADER_MIN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t head[HEADER_MIN];
    xc_err err = xc_source_read(src, 0, head, sizeof head);
    if (err != XC_OK) {
        return err;
    }
    uint32_t base = 0;
    uint32_t headers = 0;
    uint32_t cert_addr = 0;
    (void)xc_rd_u32le(head, sizeof head, 0x104, &base);
    (void)xc_rd_u32le(head, sizeof head, 0x108, &headers);
    (void)xc_rd_u32le(head, sizeof head, 0x118, &cert_addr);
    if (memcmp(head, "XBEH", 4) != 0 || headers < HEADER_MIN || headers > src->size ||
        cert_addr < base) {
        return XC_ERR_CORRUPT;
    }
    uint32_t off = cert_addr - base;
    if (off < HEADER_MIN || off > headers || headers - off < CERT_MIN) {
        return XC_ERR_CORRUPT;
    }
    uint8_t c[CERT_MIN];
    err = xc_source_read(src, off, c, sizeof c);
    if (err != XC_OK) {
        return err;
    }
    uint32_t size = 0;
    (void)xc_rd_u32le(c, sizeof c, 0, &size);
    if (size < CERT_MIN) {
        return XC_ERR_CORRUPT;
    }
    (void)xc_rd_u32le(c, sizeof c, 8, &cert->title_id);
    title_utf8(c + 0x0C, cert->title);
    return XC_OK;
}

#define CERT_FIELDS 0xB0U  /* §4: the certificate up to the end of the version */
#define SECTION_HEADER 56U /* §5 */
#define IMAGE_MIN 0x20U    /* §5: an XPR0 header's fixed part */

/* §5: the NUL-terminated name at file offset `off` of the headers h[0, n) is `want`. */
static bool name_is(const uint8_t *h, size_t n, uint32_t off, const char *want)
{
    size_t len = strlen(want) + 1U; /* with its NUL */
    return off <= n && n - off >= len && memcmp(h + off, want, len) == 0;
}

xc_err xc_xbe_read_info(const xc_source *src, xc_xbe_info *info)
{
    memset(info, 0, sizeof *info);
    xc_err err = xc_xbe_read_cert(src, &info->cert);
    if (err != XC_OK) {
        return err;
    }
    /* §3 has checked these. */
    uint8_t head[HEADER_MIN];
    err = xc_source_read(src, 0, head, sizeof head);
    if (err != XC_OK) {
        return err;
    }
    uint32_t base = 0;
    uint32_t headers = 0;
    uint32_t cert_addr = 0;
    uint32_t sections = 0;
    uint32_t sections_addr = 0;
    (void)xc_rd_u32le(head, sizeof head, 0x104, &base);
    (void)xc_rd_u32le(head, sizeof head, 0x108, &headers);
    (void)xc_rd_u32le(head, sizeof head, 0x118, &cert_addr);
    (void)xc_rd_u32le(head, sizeof head, 0x11C, &sections);
    (void)xc_rd_u32le(head, sizeof head, 0x120, &sections_addr);
    size_t n = headers < XC_XBE_MAX_HEADERS ? headers : XC_XBE_MAX_HEADERS;
    uint8_t *h = malloc(n);
    if (h == NULL) {
        return XC_ERR_NOMEM;
    }
    err = xc_source_read(src, 0, h, n);
    if (err != XC_OK) {
        free(h);
        return err;
    }

    /* §4: region, disc and version, if the certificate reaches them. */
    uint32_t off = cert_addr - base;
    uint32_t cert_size = 0;
    (void)xc_rd_u32le(h, n, off, &cert_size);
    if (cert_size >= CERT_FIELDS && off <= n && n - off >= CERT_FIELDS) {
        info->has_region = true;
        (void)xc_rd_u32le(h, n, off + 0xA0U, &info->region);
        (void)xc_rd_u32le(h, n, off + 0xA8U, &info->disc);
        (void)xc_rd_u32le(h, n, off + 0xACU, &info->version);
    }

    /* §5: the title image's section. */
    uint32_t at = sections_addr - base;
    if (sections_addr >= base && sections > 0 && sections <= XC_XBE_MAX_SECTIONS && at <= n &&
        (n - at) / SECTION_HEADER >= sections) {
        uint32_t best = 0; /* 2: $$XTIMAGE, 1: $$XSIMAGE */
        for (uint32_t i = 0; i < sections && best < 2; i++) {
            size_t s = at + (size_t)i * SECTION_HEADER;
            uint32_t raw = 0;
            uint32_t raw_size = 0;
            uint32_t name_addr = 0;
            (void)xc_rd_u32le(h, n, s + 0x0C, &raw);
            (void)xc_rd_u32le(h, n, s + 0x10, &raw_size);
            (void)xc_rd_u32le(h, n, s + 0x14, &name_addr);
            if (name_addr < base) {
                continue;
            }
            uint32_t rank = name_is(h, n, name_addr - base, "$$XTIMAGE")   ? 2U
                            : name_is(h, n, name_addr - base, "$$XSIMAGE") ? 1U
                                                                           : 0U;
            if (rank > best && raw_size >= IMAGE_MIN && raw_size <= XC_XBE_MAX_IMAGE &&
                raw <= src->size && src->size - raw >= raw_size) {
                best = rank;
                info->image_offset = raw;
                info->image_size = raw_size;
            }
        }
    }
    free(h);
    return XC_OK;
}
