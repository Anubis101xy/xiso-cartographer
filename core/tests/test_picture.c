/* Tests for pictures (xcat/picture.h, docs/formats/art.md §4-§5, docs/formats/xpr.md), with the
   fixtures tools/fixtures/art/make.py made in core/tests/data/art/. */
#include "unity.h"
#include "xcat/folder_image.h"
#include "xcat/picture.h"
#include "xcat/xbe.h"
#include "xcat/xdvdfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *dir;

static uint8_t *load(const char *name, size_t *len)
{
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long n = ftell(f);
    TEST_ASSERT_TRUE(n >= 0);
    rewind(f);
    uint8_t *data = malloc(n > 0 ? (size_t)n : 1U);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(data, 1, (size_t)n, f));
    fclose(f);
    *len = (size_t)n;
    return data;
}

static int channel(uint32_t p, int shift)
{
    return (int)((p >> (unsigned)shift) & 0xFFU);
}

/* Every channel of p within tol of (r, g, b). */
static void near(uint32_t p, int r, int g, int b, int tol)
{
    TEST_ASSERT_INT_WITHIN((unsigned)tol, r, channel(p, 16));
    TEST_ASSERT_INT_WITHIN((unsigned)tol, g, channel(p, 8));
    TEST_ASSERT_INT_WITHIN((unsigned)tol, b, channel(p, 0));
}

static void test_png(void)
{
    size_t len = 0;
    uint8_t *data = load("cover.png", &len);
    xc_picture p;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_decode(data, len, &p));
    TEST_ASSERT_EQUAL_UINT32(3, p.w);
    TEST_ASSERT_EQUAL_UINT32(2, p.h);
    static const uint32_t want[6] = {0xFFFF0000U, 0x8000FF00U, 0x000000FFU,
                                     0xFF0A141EU, 0xFFC86432U, 0x04010203U};
    TEST_ASSERT_EQUAL_HEX32_ARRAY(want, p.px, 6);
    xc_picture_free(&p);
    TEST_ASSERT_NULL(p.px);
    xc_picture_free(&p); /* again: still fine */

    /* Cut short: not decoded, nothing returned. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_decode(data, len - 20, &p));
    TEST_ASSERT_NULL(p.px);
    free(data);
}

static void test_jpeg(void)
{
    const char *names[] = {"cover.jpg", "progressive.jpg"};
    for (size_t i = 0; i < 2; i++) {
        size_t len = 0;
        uint8_t *data = load(names[i], &len);
        xc_picture p;
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_decode(data, len, &p));
        TEST_ASSERT_EQUAL_UINT32(16, p.w);
        TEST_ASSERT_EQUAL_UINT32(16, p.h);
        /* The quadrants' middles, away from the edges JPEG blurs. */
        near(p.px[3 * 16 + 3], 200, 30, 30, 12);
        near(p.px[3 * 16 + 12], 30, 200, 30, 12);
        near(p.px[12 * 16 + 3], 30, 30, 200, 12);
        near(p.px[12 * 16 + 12], 230, 230, 230, 12);
        TEST_ASSERT_EQUAL_HEX32(0xFF000000U, p.px[0] & 0xFF000000U); /* opaque */
        xc_picture_free(&p);
        free(data);
    }
}

static void test_refused(void)
{
    xc_picture p;
    size_t len = 0;
    uint8_t *huge = load("huge.png", &len);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_picture_decode(huge, len, &p)); /* 4000 x 4000 */
    TEST_ASSERT_NULL(p.px);
    free(huge);

    static const uint8_t gif[] = "GIF89a\x01\x00\x01\x00";
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, xc_picture_decode(gif, sizeof gif, &p));
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, xc_picture_decode(gif, 0, &p));
    static const uint8_t jpeg_start[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F'};
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_decode(jpeg_start, sizeof jpeg_start, &p));
    /* Over the file limit, before anything is decoded. */
    uint8_t *big = calloc(XC_PICTURE_MAX_FILE + 1U, 1);
    TEST_ASSERT_NOT_NULL(big);
    memcpy(big, jpeg_start, sizeof jpeg_start);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_picture_decode(big, XC_PICTURE_MAX_FILE + 1U, &p));
    free(big);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_picture_decode(NULL, 0, &p));
}

/* ---- XPR0 (xpr.md) */

static uint8_t xpr[0x40 + 4096];

static void put32(uint8_t *b, size_t off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8U);
    b[off + 2] = (uint8_t)(v >> 16U);
    b[off + 3] = (uint8_t)(v >> 24U);
}

/* An XPR0 header for a texture of `format` (colour format byte), 2^lw x 2^lh; data at 0x40. */
static void xpr_header(uint32_t colour, uint32_t lw, uint32_t lh, uint32_t size)
{
    memset(xpr, 0, sizeof xpr);
    memcpy(xpr, "XPR0", 4);
    put32(xpr, 0x04, sizeof xpr);
    put32(xpr, 0x08, 0x40);
    put32(xpr, 0x0C, 0x00040001U);
    put32(xpr, 0x18, 0x29U | colour << 8U | 1U << 16U | lw << 20U | lh << 24U);
    put32(xpr, 0x1C, size);
    put32(xpr, 0x20, 0xFFFFFFFFU);
}

static void test_xpr_dxt(void)
{
    xc_picture p;
    /* DXT1, 4 x 4, one block: red and blue ends, four-colour mode (red565 > blue565). */
    xpr_header(0x0C, 2, 2, 0);
    put32(xpr, 0x40, 0xF800U | 0x001FU << 16U); /* color0 red, color1 blue */
    put32(xpr, 0x44, 0xE4E4E4E4U);              /* each row: indices 0, 1, 2, 3 */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x48, &p));
    TEST_ASSERT_EQUAL_UINT32(4, p.w);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0000U, p.px[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000FFU, p.px[1]);
    TEST_ASSERT_EQUAL_HEX32(0xFFAA0055U, p.px[2]); /* 2/3 red + 1/3 blue */
    TEST_ASSERT_EQUAL_HEX32(0xFF5500AAU, p.px[3]);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0000U, p.px[12]);
    xc_picture_free(&p);
    /* Three-colour mode (color0 <= color1): index 3 is transparent. */
    put32(xpr, 0x40, 0x001FU | 0xF800U << 16U);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x48, &p));
    TEST_ASSERT_EQUAL_HEX32(0xFF7F007FU, p.px[2]); /* half and half */
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, p.px[3]);
    xc_picture_free(&p);
    /* One byte short of the block. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, 0x47, &p));

    /* DXT3: explicit 4-bit alphas, then a colour block always in four-colour mode. */
    xpr_header(0x0E, 2, 2, 0);
    put32(xpr, 0x40, 0x0000F0FFU); /* alphas: pixel 0 = 0xF, 1 = 0xF, 2 = 0, 3 = 0xF */
    put32(xpr, 0x48, 0x001FU | 0xF800U << 16U);
    put32(xpr, 0x4C, 0);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x50, &p));
    TEST_ASSERT_EQUAL_HEX32(0xFF0000FFU, p.px[0]);
    TEST_ASSERT_EQUAL_HEX32(0x000000FFU, p.px[2]);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000FFU, p.px[3]);
    TEST_ASSERT_EQUAL_HEX32(0x000000FFU, p.px[4]); /* the rest are 0 */
    xc_picture_free(&p);

    /* DXT5: alpha ends 255 and 0 interpolated in eighths; index 1 everywhere is 0. */
    xpr_header(0x0F, 2, 2, 0);
    xpr[0x40] = 255;
    xpr[0x41] = 0;
    xpr[0x42] = 0x02; /* pixel 0: index 2 = (6*255 + 0)/7 = 218; the rest 0 (alpha0) */
    put32(xpr, 0x48, 0x07E0U | 0x07E0U << 16U); /* green */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x50, &p));
    TEST_ASSERT_EQUAL_HEX32(0xDA00FF00U, p.px[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFF00FF00U, p.px[1]);
    xc_picture_free(&p);
}

static void test_xpr_swizzled(void)
{
    xc_picture p;
    /* A8R8G8B8, 4 x 2, swizzled: pixel (x, y) is at the offset with x's and y's bits
       interleaved, x first: (1,0)->1, (0,1)->2, (1,1)->3, (2,0)->4, (3,1)->7. */
    xpr_header(0x06, 2, 1, 0);
    for (uint32_t i = 0; i < 8; i++) {
        put32(xpr, 0x40 + 4 * i, 0x80000000U | i); /* the value says where it was */
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x60, &p));
    TEST_ASSERT_EQUAL_UINT32(4, p.w);
    TEST_ASSERT_EQUAL_UINT32(2, p.h);
    static const uint32_t order[8] = {0, 1, 4, 5, 2, 3, 6, 7};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX32(0x80000000U | order[i], p.px[i]);
    }
    xc_picture_free(&p);
    /* X8R8G8B8: opaque whatever the fourth byte says. */
    xpr_header(0x07, 1, 1, 0);
    put32(xpr, 0x40, 0x00123456U);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x50, &p));
    TEST_ASSERT_EQUAL_HEX32(0xFF123456U, p.px[0]);
    xc_picture_free(&p);
    /* R5G6B5, 2 x 2: white, black, red, green. */
    xpr_header(0x05, 1, 1, 0);
    put32(xpr, 0x40, 0xFFFFU | 0x0000U << 16U);
    put32(xpr, 0x44, 0xF800U | 0x07E0U << 16U);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x48, &p));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, p.px[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFF000000U, p.px[1]);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0000U, p.px[2]);
    TEST_ASSERT_EQUAL_HEX32(0xFF00FF00U, p.px[3]);
    xc_picture_free(&p);
    /* A1R5G5B5 and A4R4G4B4. */
    xpr_header(0x02, 0, 0, 0);
    put32(xpr, 0x40, 0x7C00U);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x42, &p));
    TEST_ASSERT_EQUAL_HEX32(0x00FF0000U, p.px[0]);
    xc_picture_free(&p);
    xpr_header(0x04, 0, 0, 0);
    put32(xpr, 0x40, 0x8F00U);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x42, &p));
    TEST_ASSERT_EQUAL_HEX32(0x88FF0000U, p.px[0]);
    xc_picture_free(&p);
}

static void test_xpr_linear(void)
{
    xc_picture p;
    /* LIN_A8R8G8B8, 3 x 2, rows 64 bytes apart: the size word says so (§2.2). */
    xpr_header(0x12, 0, 0, 2U | 1U << 12U | 0U << 24U);
    for (uint32_t y = 0; y < 2; y++) {
        for (uint32_t x = 0; x < 3; x++) {
            put32(xpr, 0x40 + 64 * y + 4 * x, 0xFF000000U | (y << 8U) | x);
        }
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xpr, 0x40 + 64 + 12, &p));
    TEST_ASSERT_EQUAL_UINT32(3, p.w);
    TEST_ASSERT_EQUAL_UINT32(2, p.h);
    TEST_ASSERT_EQUAL_HEX32(0xFF000102U, p.px[5]);
    xc_picture_free(&p);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, 0x40 + 64 + 11, &p));
    /* A pitch smaller than a row. */
    xpr_header(0x12, 0, 0, 20U);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, sizeof xpr, &p));
}

static void test_xpr_refused(void)
{
    xc_picture p;
    xpr_header(0x0C, 2, 2, 0);
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, 0x1F, &p)); /* no header */
    xpr[0] = 'Y';
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0C, 2, 2, 0);
    put32(xpr, 0x0C, 0x00010001U); /* a vertex buffer, not a texture */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0C, 2, 2, 0);
    put32(xpr, 0x08, sizeof xpr + 1U); /* the header past the end */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0C, 2, 2, 0);
    put32(xpr, 0x10, sizeof xpr); /* the data past the end */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0B, 2, 2, 0); /* P8: needs a palette */
    TEST_ASSERT_EQUAL_INT(XC_ERR_UNSUPPORTED, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0C, 11, 2, 0); /* 2048 wide */
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_picture_xpr(xpr, sizeof xpr, &p));
    xpr_header(0x0C, 15, 2, 0);
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, xc_picture_xpr(xpr, sizeof xpr, &p));
    TEST_ASSERT_NULL(p.px);
}

/* The fixture XBE's title image, written by the tool that writes the app's own: its DXT1
   decodes to the PNG it was made from, within what DXT1 loses. */
static void test_title_image(void)
{
    size_t xlen = 0;
    uint8_t *xbe = load("title.xbe", &xlen);
    xc_source src;
    xc_mem_source mem;
    xc_source_memory(&src, &mem, xbe, xlen);
    xc_xbe_info info;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xbe_read_info(&src, &info));
    TEST_ASSERT_EQUAL_STRING("Art Fixture", info.cert.title);
    TEST_ASSERT_EQUAL_HEX32(0x58430001U, info.cert.title_id);
    TEST_ASSERT_TRUE(info.has_region);
    TEST_ASSERT_EQUAL_HEX32(XC_XBE_REGION_WORLD, info.region);
    TEST_ASSERT_EQUAL_UINT32(1, info.disc);
    TEST_ASSERT_EQUAL_HEX32(0x102, info.version);
    TEST_ASSERT_EQUAL_UINT32(10240, info.image_size);
    TEST_ASSERT_TRUE(info.image_offset + info.image_size <= xlen);

    xc_picture got;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(xbe + info.image_offset, info.image_size, &got));
    size_t plen = 0;
    uint8_t *png = load("title.png", &plen);
    xc_picture want;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_decode(png, plen, &want));
    TEST_ASSERT_EQUAL_UINT32(128, got.w);
    TEST_ASSERT_EQUAL_UINT32(128, got.h);
    for (size_t i = 0; i < 128U * 128U; i++) {
        uint32_t w = want.px[i];
        near(got.px[i], channel(w, 16), channel(w, 8), channel(w, 0), 12);
    }
    xc_picture_free(&got);
    xc_picture_free(&want);
    free(png);
    free(xbe);
}

/* The way the app reads a disc image's title image (art.md §3): the image's root table only,
   its default.xbe as a window of the image, the section, the texture. The image is a folder
   image (folder-image.md) holding the fixture XBE among other files. */
static const uint8_t *chain_xbe;
static size_t chain_len;

static xc_err chain_read(void *ctx, size_t item, uint64_t offset, void *buf, size_t len)
{
    (void)ctx;
    if (item == 1) { /* default.xbe */
        memcpy(buf, chain_xbe + offset, len);
    } else {
        memset(buf, (int)item, len);
    }
    return XC_OK;
}

static void test_title_image_in_disc_image(void)
{
    size_t xlen = 0;
    uint8_t *xbe = load("title.xbe", &xlen);
    chain_xbe = xbe;
    chain_len = xlen;
    const xc_fimg_item items[] = {
        {"a.bin", 5000, 1, false, 0},      {"default.xbe", chain_len, 1, false, 0},
        {"media", 0, 1, true, 0},          {"media/default.xbe", 7, 1, false, 0},
        {"Z-last.bin", 3000, 1, false, 0},
    };
    xc_fimg *img = NULL;
    xc_source src;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_fimg_build(items, 5, chain_read, NULL, &img, &src, NULL));
    xc_xdvdfs_volume vol;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_open(&src, &vol));
    xc_xdvdfs_entry e;
    bool found = false;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_find_root(&src, &vol, "DEFAULT.XBE", &e, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT32(chain_len, e.size);
    xc_source inside;
    xc_window_source win;
    xc_source_window(&inside, &win, &src, e.offset, e.size);
    xc_xbe_info info;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xbe_read_info(&inside, &info));
    TEST_ASSERT_EQUAL_STRING("Art Fixture", info.cert.title);
    uint8_t *section = malloc(info.image_size);
    TEST_ASSERT_NOT_NULL(section);
    TEST_ASSERT_EQUAL_INT(XC_OK,
                          xc_source_read(&inside, info.image_offset, section, info.image_size));
    xc_picture p;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_xpr(section, info.image_size, &p));
    TEST_ASSERT_EQUAL_UINT32(128, p.w);
    near(p.px[0], 0, 0, 255, 12); /* the gradient's corner: (0, 0, 255) */
    xc_picture_free(&p);
    free(section);
    xc_fimg_free(img);
    free(xbe);
}

/* ---- §5: fitting */

static void test_fit(void)
{
    uint32_t px[8] = {0xFF000000U, 0xFF020202U, 0xFFFFFFFFU, 0xFFFDFDFDU,  /* row 0 */
                      0xFF040404U, 0xFF060606U, 0x00FFFFFFU, 0x00000000U}; /* row 1 */
    xc_picture in = {px, 4, 2};
    xc_picture out;
    /* Into a 2 x 2 box: 4 x 2 keeps its shape, so 2 x 1, each pixel the average of 2 x 2. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_fit(&in, 2, 2, 1, 0x808080U, &out));
    TEST_ASSERT_EQUAL_UINT32(2, out.w);
    TEST_ASSERT_EQUAL_UINT32(1, out.h);
    TEST_ASSERT_EQUAL_HEX32(0xFF030303U, out.px[0]); /* (0 + 2 + 4 + 6) / 4 */
    /* (255 + 253 + 128 + 128) / 4, the clear ones over the grey */
    TEST_ASSERT_EQUAL_HEX32(0xFFBFBFBFU, out.px[1]);
    xc_picture_free(&out);

    /* A box larger than the picture: not enlarged past max_up. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_fit(&in, 100, 100, 1, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(4, out.w);
    TEST_ASSERT_EQUAL_UINT32(2, out.h);
    TEST_ASSERT_EQUAL_HEX32(0xFF000000U, out.px[7]); /* clear over black */
    xc_picture_free(&out);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_fit(&in, 100, 100, 2, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(8, out.w);
    TEST_ASSERT_EQUAL_UINT32(4, out.h);
    TEST_ASSERT_EQUAL_HEX32(0xFF000000U, out.px[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFF000000U, out.px[1]); /* each pixel twice */
    TEST_ASSERT_EQUAL_HEX32(0xFF020202U, out.px[2]);
    xc_picture_free(&out);

    /* A tall box: limited by width. A box of 1: 1 x 1 at least. */
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_fit(&in, 2, 100, 1, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(2, out.w);
    TEST_ASSERT_EQUAL_UINT32(1, out.h);
    xc_picture_free(&out);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_picture_fit(&in, 1, 1, 1, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(1, out.w);
    TEST_ASSERT_EQUAL_UINT32(1, out.h);
    xc_picture_free(&out);

    xc_picture empty = {NULL, 0, 0};
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_picture_fit(&empty, 2, 2, 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_picture_fit(&in, 0, 2, 1, 0, &out));
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : "core/tests/data/art";
    UNITY_BEGIN();
    RUN_TEST(test_png);
    RUN_TEST(test_jpeg);
    RUN_TEST(test_refused);
    RUN_TEST(test_xpr_dxt);
    RUN_TEST(test_xpr_swizzled);
    RUN_TEST(test_xpr_linear);
    RUN_TEST(test_xpr_refused);
    RUN_TEST(test_title_image);
    RUN_TEST(test_title_image_in_disc_image);
    RUN_TEST(test_fit);
    return UNITY_END();
}
