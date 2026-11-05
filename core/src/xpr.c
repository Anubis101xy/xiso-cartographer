/* XPR0 textures: an XBE's title image (xcat/picture.h, docs/formats/xpr.md). */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "xcat/picture.h"

#define XPR_HEADER 0x20U      /* §2: the file header and one resource header */
#define TYPE_MASK 0x00070000U /* §2: Common's resource type */
#define TYPE_TEXTURE 0x00040000U

/* §3: the colour formats decoded. */
enum {
    F_A1R5G5B5 = 0x02,
    F_X1R5G5B5 = 0x03,
    F_A4R4G4B4 = 0x04,
    F_R5G6B5 = 0x05,
    F_A8R8G8B8 = 0x06,
    F_X8R8G8B8 = 0x07,
    F_DXT1 = 0x0C,
    F_DXT3 = 0x0E, /* and DXT2 */
    F_DXT5 = 0x0F, /* and DXT4 */
    F_LIN_A1R5G5B5 = 0x10,
    F_LIN_R5G6B5 = 0x11,
    F_LIN_A8R8G8B8 = 0x12,
    F_LIN_X1R5G5B5 = 0x1C,
    F_LIN_A4R4G4B4 = 0x1D,
    F_LIN_X8R8G8B8 = 0x1E,
};

static uint32_t expand5(uint32_t v)
{
    return (v << 3U) | (v >> 2U);
}

static uint32_t expand6(uint32_t v)
{
    return (v << 2U) | (v >> 4U);
}

static uint32_t expand4(uint32_t v)
{
    return (v << 4U) | v;
}

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return a << 24U | r << 16U | g << 8U | b;
}

/* §3.1: one 16-bit pixel. */
static uint32_t pixel16(int format, uint32_t v)
{
    switch (format) {
    case F_R5G6B5:
    case F_LIN_R5G6B5:
        return argb(255, expand5(v >> 11U), expand6((v >> 5U) & 0x3FU), expand5(v & 0x1FU));
    case F_A1R5G5B5:
    case F_LIN_A1R5G5B5:
        return argb((v & 0x8000U) != 0 ? 255U : 0U, expand5((v >> 10U) & 0x1FU),
                    expand5((v >> 5U) & 0x1FU), expand5(v & 0x1FU));
    case F_X1R5G5B5:
    case F_LIN_X1R5G5B5:
        return argb(255, expand5((v >> 10U) & 0x1FU), expand5((v >> 5U) & 0x1FU),
                    expand5(v & 0x1FU));
    default: /* A4R4G4B4 */
        return argb(expand4(v >> 12U), expand4((v >> 8U) & 0xFU), expand4((v >> 4U) & 0xFU),
                    expand4(v & 0xFU));
    }
}

/* §3.2: where pixel (x, y) is in a swizzled texture of w x h (powers of two), in pixels. The
   bits of x and y are interleaved, x's first, while both have bits left; then the rest of the
   larger one's. */
static uint32_t swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t off = 0;
    uint32_t bit = 1;
    for (uint32_t m = 1; m < w || m < h; m <<= 1U) {
        if (m < w) {
            if ((x & m) != 0) {
                off |= bit;
            }
            bit <<= 1U;
        }
        if (m < h) {
            if ((y & m) != 0) {
                off |= bit;
            }
            bit <<= 1U;
        }
    }
    return off;
}

/* §3.3: the four colours of a DXT colour block; `four` forces DXT1's four-colour mode
   (DXT3 and DXT5 always use it). The last is transparent black in DXT1's three-colour mode. */
static void dxt_colours(const uint8_t *b, bool four, uint32_t c[4])
{
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8U;
    uint32_t c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8U;
    uint32_t r[4] = {expand5(c0 >> 11U), expand5(c1 >> 11U), 0, 0};
    uint32_t g[4] = {expand6((c0 >> 5U) & 0x3FU), expand6((c1 >> 5U) & 0x3FU), 0, 0};
    uint32_t bl[4] = {expand5(c0 & 0x1FU), expand5(c1 & 0x1FU), 0, 0};
    uint32_t a3 = 255;
    if (four || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        bl[2] = (2 * bl[0] + bl[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        bl[3] = (bl[0] + 2 * bl[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        bl[2] = (bl[0] + bl[1]) / 2;
        a3 = 0; /* transparent black */
    }
    for (int i = 0; i < 4; i++) {
        c[i] = argb(i == 3 ? a3 : 255U, r[i], g[i], bl[i]);
    }
}

/* §3.3: DXT1, DXT3 or DXT5 blocks into w x h pixels. */
static void decode_dxt(int format, const uint8_t *d, uint32_t w, uint32_t h, uint32_t *px)
{
    size_t block = format == F_DXT1 ? 8U : 16U;
    uint32_t bw = (w + 3U) / 4U;
    uint32_t bh = (h + 3U) / 4U;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = d + ((size_t)by * bw + bx) * block;
            const uint8_t *cb = format == F_DXT1 ? b : b + 8;
            uint32_t c[4];
            dxt_colours(cb, format != F_DXT1, c);
            /* DXT5's eight alphas */
            uint32_t a[8] = {b[0], b[1], 0, 0, 0, 0, 0, 0};
            if (format == F_DXT5) {
                if (a[0] > a[1]) {
                    for (uint32_t i = 1; i < 7; i++) {
                        a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                    }
                } else {
                    for (uint32_t i = 1; i < 5; i++) {
                        a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                    }
                    a[6] = 0;
                    a[7] = 255;
                }
            }
            uint64_t abits = 0; /* DXT5: 16 3-bit indices */
            for (int i = 0; i < 6; i++) {
                abits |= (uint64_t)b[2 + i] << (8U * (unsigned)i);
            }
            uint32_t idx = (uint32_t)cb[4] | (uint32_t)cb[5] << 8U | (uint32_t)cb[6] << 16U |
                           (uint32_t)cb[7] << 24U;
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4U + (i & 3U);
                uint32_t y = by * 4U + (i >> 2U);
                if (x >= w || y >= h) {
                    continue;
                }
                uint32_t p = c[(idx >> (2U * i)) & 3U];
                if (format == F_DXT3) {
                    uint32_t nib = ((uint32_t)b[i / 2U] >> (4U * (i & 1U))) & 0xFU;
                    p = (p & 0x00FFFFFFU) | expand4(nib) << 24U;
                } else if (format == F_DXT5) {
                    p = (p & 0x00FFFFFFU) | a[(abits >> (3U * i)) & 7U] << 24U;
                }
                px[(size_t)y * w + x] = p;
            }
        }
    }
}

xc_err xc_picture_xpr(const uint8_t *data, size_t len, xc_picture *out)
{
    *out = (xc_picture){NULL, 0, 0};
    if (data == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    /* §2: the header. */
    uint32_t header = 0;
    uint32_t common = 0;
    uint32_t at = 0;
    uint32_t format = 0;
    uint32_t size = 0;
    if (len < XPR_HEADER || memcmp(data, "XPR0", 4) != 0 ||
        !xc_rd_u32le(data, len, 0x08, &header) || !xc_rd_u32le(data, len, 0x0C, &common) ||
        !xc_rd_u32le(data, len, 0x10, &at) || !xc_rd_u32le(data, len, 0x18, &format) ||
        !xc_rd_u32le(data, len, 0x1C, &size)) {
        return XC_ERR_CORRUPT;
    }
    if (header < XPR_HEADER || header > len || (common & TYPE_MASK) != TYPE_TEXTURE ||
        at > len - header || ((format >> 4U) & 0xFU) != 2U) {
        return XC_ERR_CORRUPT;
    }
    const uint8_t *d = data + header + at;
    size_t have = len - header - at;
    int f = (int)((format >> 8U) & 0xFFU);
    bool linear = f == F_LIN_A1R5G5B5 || f == F_LIN_R5G6B5 || f == F_LIN_A8R8G8B8 ||
                  f == F_LIN_X1R5G5B5 || f == F_LIN_A4R4G4B4 || f == F_LIN_X8R8G8B8;
    bool dxt = f == F_DXT1 || f == F_DXT3 || f == F_DXT5;
    bool swz = f == F_A1R5G5B5 || f == F_X1R5G5B5 || f == F_A4R4G4B4 || f == F_R5G6B5 ||
               f == F_A8R8G8B8 || f == F_X8R8G8B8;
    if (!linear && !dxt && !swz) {
        return XC_ERR_UNSUPPORTED;
    }
    bool wide = f == F_A8R8G8B8 || f == F_X8R8G8B8 || f == F_LIN_A8R8G8B8 || f == F_LIN_X8R8G8B8;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t pitch = 0; /* linear: bytes from one row to the next */
    if (linear) {
        /* §2.2: the size word. */
        w = (size & 0xFFFU) + 1U;
        h = ((size >> 12U) & 0xFFFU) + 1U;
        pitch = (((size >> 24U) & 0xFFU) + 1U) * 64U;
    } else {
        uint32_t lw = (format >> 20U) & 0xFU;
        uint32_t lh = (format >> 24U) & 0xFU;
        if (lw > 11U || lh > 11U) {
            return XC_ERR_LIMIT;
        }
        w = 1U << lw;
        h = 1U << lh;
    }
    if (w > XC_XPR_MAX_SIDE || h > XC_XPR_MAX_SIDE) {
        return XC_ERR_LIMIT;
    }
    uint32_t bpp = wide ? 4U : 2U;
    uint64_t need = dxt ? (uint64_t)((w + 3U) / 4U) * ((h + 3U) / 4U) * (f == F_DXT1 ? 8U : 16U)
                    : linear ? (uint64_t)pitch * (h - 1U) + (uint64_t)w * bpp
                             : (uint64_t)w * h * bpp;
    if (linear && pitch < w * bpp) {
        return XC_ERR_CORRUPT;
    }
    if (need > have) {
        return XC_ERR_CORRUPT;
    }
    uint32_t *px = malloc((size_t)w * h * sizeof *px);
    if (px == NULL) {
        return XC_ERR_NOMEM;
    }
    if (dxt) {
        decode_dxt(f, d, w, h, px);
    } else {
        bool alpha = f == F_A8R8G8B8 || f == F_LIN_A8R8G8B8;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                size_t i = linear ? (size_t)y * pitch / bpp + x : swizzle(x, y, w, h);
                const uint8_t *s = d + i * bpp;
                uint32_t p = 0;
                if (wide) {
                    p = (uint32_t)s[0] | (uint32_t)s[1] << 8U | (uint32_t)s[2] << 16U |
                        (alpha ? (uint32_t)s[3] << 24U : 0xFF000000U);
                } else {
                    p = pixel16(f, (uint32_t)s[0] | (uint32_t)s[1] << 8U);
                }
                px[(size_t)y * w + x] = p;
            }
        }
    }
    *out = (xc_picture){px, w, h};
    return XC_OK;
}
