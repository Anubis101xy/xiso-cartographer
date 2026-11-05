/* Pictures for a game's details (xcat/picture.h, docs/formats/art.md §4-§5). */
#include "xcat/picture.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* core/src/stb_image_impl.c */
void *xc_stb_malloc(size_t n);
void *xc_stb_realloc(void *p, size_t old_n, size_t n);
void xc_stb_free(void *p);
int xc_stb_info(const unsigned char *data, int len, int *w, int *h);
unsigned char *xc_stb_load(const unsigned char *data, int len, int *w, int *h);
void xc_stb_image_free(void *p);

void xc_picture_free(xc_picture *p)
{
    free(p->px);
    p->px = NULL;
    p->w = 0;
    p->h = 0;
}

/* ---- stb_image's allocator: what one decode allocates, capped (§4). One decode at a time. */

#define HEAD 16U /* keeps malloc's alignment; holds the block's size */

static size_t mem_used;
static bool mem_over;   /* a request went over XC_PICTURE_MAX_MEMORY */
static bool mem_failed; /* malloc said no */

void *xc_stb_malloc(size_t n)
{
    if (n > XC_PICTURE_MAX_MEMORY || mem_used > XC_PICTURE_MAX_MEMORY - n) {
        mem_over = true;
        return NULL;
    }
    unsigned char *b = malloc(HEAD + n);
    if (b == NULL) {
        mem_failed = true;
        return NULL;
    }
    memcpy(b, &n, sizeof n);
    mem_used += n;
    return b + HEAD;
}

void xc_stb_free(void *p)
{
    if (p == NULL) {
        return;
    }
    unsigned char *b = (unsigned char *)p - HEAD;
    size_t n = 0;
    memcpy(&n, b, sizeof n);
    mem_used -= n;
    free(b);
}

void *xc_stb_realloc(void *p, size_t old_n, size_t n)
{
    (void)old_n; /* the block knows its size */
    if (p == NULL) {
        return xc_stb_malloc(n);
    }
    unsigned char *b = (unsigned char *)p - HEAD;
    size_t had = 0;
    memcpy(&had, b, sizeof had);
    if (n > XC_PICTURE_MAX_MEMORY || mem_used - had > XC_PICTURE_MAX_MEMORY - n) {
        mem_over = true;
        return NULL;
    }
    unsigned char *nb = realloc(b, HEAD + n);
    if (nb == NULL) {
        mem_failed = true;
        return NULL;
    }
    memcpy(nb, &n, sizeof n);
    mem_used = mem_used - had + n;
    return nb + HEAD;
}

/* ---- §4: JPEG and PNG */

static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

xc_err xc_picture_decode(const uint8_t *data, size_t len, xc_picture *out)
{
    *out = (xc_picture){NULL, 0, 0};
    if (data == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    bool jpeg = len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    bool png = len >= sizeof png_magic && memcmp(data, png_magic, sizeof png_magic) == 0;
    if (!jpeg && !png) {
        return XC_ERR_UNSUPPORTED;
    }
    if (len > XC_PICTURE_MAX_FILE) {
        return XC_ERR_LIMIT;
    }
    int w = 0;
    int h = 0;
    if (!xc_stb_info(data, (int)len, &w, &h) || w <= 0 || h <= 0) {
        return XC_ERR_CORRUPT;
    }
    if ((uint32_t)w > XC_PICTURE_MAX_SIDE || (uint32_t)h > XC_PICTURE_MAX_SIDE ||
        (uint64_t)w * (uint64_t)h > XC_PICTURE_MAX_PIXELS) {
        return XC_ERR_LIMIT;
    }
    mem_used = 0;
    mem_over = false;
    mem_failed = false;
    int dw = 0;
    int dh = 0;
    unsigned char *rgba = xc_stb_load(data, (int)len, &dw, &dh);
    if (rgba == NULL) {
        return mem_over ? XC_ERR_LIMIT : mem_failed ? XC_ERR_NOMEM : XC_ERR_CORRUPT;
    }
    if (dw != w || dh != h) {
        xc_stb_image_free(rgba);
        return XC_ERR_CORRUPT;
    }
    size_t n = (size_t)w * (size_t)h;
    uint32_t *px = malloc(n * sizeof *px);
    if (px == NULL) {
        xc_stb_image_free(rgba);
        return XC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char *s = rgba + 4 * i;
        px[i] = (uint32_t)s[3] << 24U | (uint32_t)s[0] << 16U | (uint32_t)s[1] << 8U | s[2];
    }
    xc_stb_image_free(rgba);
    *out = (xc_picture){px, (uint32_t)w, (uint32_t)h};
    return XC_OK;
}

/* ---- §5: fitting to the panel */

xc_err xc_picture_fit(const xc_picture *in, uint32_t box_w, uint32_t box_h, uint32_t max_up,
                      uint32_t bg, xc_picture *out)
{
    *out = (xc_picture){NULL, 0, 0};
    if (in->px == NULL || in->w == 0 || in->h == 0 || box_w == 0 || box_h == 0) {
        return XC_ERR_INVALID_ARG;
    }
    if (max_up == 0) {
        max_up = 1;
    }
    /* The largest w x h of the picture's shape inside the box and max_up times its size. */
    uint64_t lim_w = (uint64_t)in->w * max_up < box_w ? (uint64_t)in->w * max_up : box_w;
    uint64_t lim_h = (uint64_t)in->h * max_up < box_h ? (uint64_t)in->h * max_up : box_h;
    uint64_t tw = lim_w;
    uint64_t th = (uint64_t)in->h * tw / in->w;
    if (th > lim_h) {
        th = lim_h;
        tw = (uint64_t)in->w * th / in->h;
    }
    tw = tw > 0 ? tw : 1;
    th = th > 0 ? th : 1;
    uint32_t *px = malloc((size_t)(tw * th) * sizeof *px);
    if (px == NULL) {
        return XC_ERR_NOMEM;
    }
    uint32_t br = (bg >> 16U) & 0xFFU;
    uint32_t bgc = (bg >> 8U) & 0xFFU;
    uint32_t bb = bg & 0xFFU;
    for (uint64_t oy = 0; oy < th; oy++) {
        /* The source rows and columns this pixel covers: at least one. */
        uint64_t y0 = oy * in->h / th;
        uint64_t y1 = (oy + 1) * in->h / th;
        y1 = y1 > y0 ? y1 : y0 + 1;
        for (uint64_t ox = 0; ox < tw; ox++) {
            uint64_t x0 = ox * in->w / tw;
            uint64_t x1 = (ox + 1) * in->w / tw;
            x1 = x1 > x0 ? x1 : x0 + 1;
            uint64_t r = 0;
            uint64_t g = 0;
            uint64_t b = 0;
            for (uint64_t y = y0; y < y1; y++) {
                const uint32_t *row = in->px + y * in->w;
                for (uint64_t x = x0; x < x1; x++) {
                    uint32_t p = row[x];
                    uint32_t a = p >> 24U;
                    r += (((p >> 16U) & 0xFFU) * a + br * (255U - a) + 127U) / 255U;
                    g += (((p >> 8U) & 0xFFU) * a + bgc * (255U - a) + 127U) / 255U;
                    b += ((p & 0xFFU) * a + bb * (255U - a) + 127U) / 255U;
                }
            }
            uint64_t n = (y1 - y0) * (x1 - x0);
            px[oy * tw + ox] = 0xFF000000U | (uint32_t)((r + n / 2) / n) << 16U |
                               (uint32_t)((g + n / 2) / n) << 8U | (uint32_t)((b + n / 2) / n);
        }
    }
    *out = (xc_picture){px, (uint32_t)tw, (uint32_t)th};
    return XC_OK;
}
