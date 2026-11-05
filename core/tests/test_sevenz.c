/* Tests for 7z archives (xcat/sevenz.h, docs/formats/sevenz.md), against the archives 7-Zip made
   in core/tests/data/7z (tools/fixtures/sevenz/make.py: synthetic bytes, computed here too). */
#include "unity.h"
#include "xcat/sevenz.h"
#include "xcat/xdvdfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *dir;
static uint8_t *data;
static size_t data_len;
static xc_mem_source mem;
static xc_source src;
static xc_7z *a;
static char why[256];

void setUp(void) {}

void tearDown(void)
{
    xc_7z_close(a);
    a = NULL;
    free(data);
    data = NULL;
}

/* make.py's pattern(). */
static uint8_t pattern(unsigned k, uint64_t i, bool random)
{
    (void)random;
    return (uint8_t)(k * 31U + i * 7U + (i >> 11));
}

typedef struct expect {
    const char *path;
    unsigned k;
    uint64_t size;
    bool random;
} expect;

static const expect game[] = {
    {"Game/default.xbe", 1, 70000, false},    {"Game/media/intro.xmv", 2, 600000, false},
    {"Game/media/noise.bin", 3, 40000, true}, {"Game/media/empty.bin", 4, 0, false},
    {"Game/data/a.txt", 5, 2049, false},      {"Game/data/b.dat", 6, 1, false},
};
#define GAME_N (sizeof game / sizeof game[0])

/* The LCG bytes of a random file, all of them. */
static uint8_t *lcg(unsigned k, size_t n)
{
    uint8_t *out = malloc(n + 1U);
    uint32_t x = 0x12345678U + k;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245U + 12345U;
        out[i] = (uint8_t)(x >> 16);
    }
    return out;
}

static void load(const char *name)
{
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    (void)fseek(f, 0, SEEK_END);
    data_len = (size_t)ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    data = malloc(data_len);
    TEST_ASSERT_EQUAL_size_t(data_len, fread(data, 1, data_len, f));
    (void)fclose(f);
    xc_source_memory(&src, &mem, data, data_len);
}

static xc_err open_7z(const char *name, size_t max_dict)
{
    load(name);
    return xc_7z_open(&src, max_dict, &a, why, sizeof why);
}

static size_t find(const char *path)
{
    for (size_t i = 0; i < xc_7z_count(a); i++) {
        if (strcmp(xc_7z_path(a, i), path) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(path);
    return SIZE_MAX;
}

/* Every file, read in pieces of `step` bytes, then its middle again (a read that goes back). */
static void check_files(size_t step)
{
    for (size_t g = 0; g < GAME_N; g++) {
        const expect *e = &game[g];
        size_t i = find(e->path);
        TEST_ASSERT_EQUAL_UINT64(e->size, xc_7z_size(a, i));
        TEST_ASSERT_FALSE(xc_7z_is_dir(a, i));
        uint8_t *want = e->random ? lcg(e->k, (size_t)e->size) : NULL;
        uint8_t *buf = malloc(step);
        for (uint64_t off = 0; off < e->size; off += step) {
            size_t n = e->size - off < step ? (size_t)(e->size - off) : step;
            TEST_ASSERT_EQUAL_MESSAGE(XC_OK, xc_7z_read(a, i, off, buf, n), e->path);
            for (size_t b = 0; b < n; b++) {
                uint8_t w = e->random ? want[off + b] : pattern(e->k, off + b, false);
                if (buf[b] != w) {
                    TEST_FAIL_MESSAGE(e->path);
                }
            }
        }
        if (e->size > 10) {
            uint64_t mid = e->size / 2;
            TEST_ASSERT_EQUAL(XC_OK, xc_7z_read(a, i, mid, buf, 1));
            TEST_ASSERT_EQUAL_HEX8(e->random ? want[mid] : pattern(e->k, mid, false), buf[0]);
        }
        free(buf);
        free(want);
    }
}

static void test_methods(void)
{
    static const char *const names[] = {"game-lzma2-solid.7z", "game-lzma2-flat.7z", "game-lzma.7z",
                                        "game-bcj.7z"};
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(XC_OK, open_7z(names[k], 0), names[k]);
        /* Six files and four folders (Game, media, data, empty-dir). */
        TEST_ASSERT_EQUAL_size_t(10, xc_7z_count(a));
        TEST_ASSERT_TRUE(xc_7z_is_dir(a, find("Game/empty-dir")));
        check_files(65536);
        check_files(300001); /* across the decoder's buffers */
        tearDown();
    }
}

static void test_copy(void)
{
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-copy.7z", 0));
    size_t i = find("Game/default.xbe");
    uint64_t at = 0;
    TEST_ASSERT_TRUE(xc_7z_stored_at(a, i, &at));
    TEST_ASSERT_EQUAL_HEX8(pattern(1, 0, false), data[at]);
    TEST_ASSERT_EQUAL_HEX8(pattern(1, 69999, false), data[at + 69999]);
    uint8_t buf[100];
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_read(a, i, 69900, buf, 100));
    TEST_ASSERT_EQUAL_HEX8(pattern(1, 69950, false), buf[50]);
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-lzma2-solid.7z", 0));
    TEST_ASSERT_FALSE(xc_7z_stored_at(a, find("Game/default.xbe"), &at));
}

typedef struct sink {
    uint64_t next;
    unsigned k;
    bool bad;
} sink;

static xc_err take(void *ctx, uint64_t offset, const uint8_t *bytes, size_t len)
{
    sink *s = ctx;
    if (offset != s->next) {
        s->bad = true;
    }
    for (size_t i = 0; i < len; i++) {
        s->bad = s->bad || bytes[i] != pattern(s->k, offset + i, false);
    }
    s->next = offset + len;
    return XC_OK;
}

static void test_image_stream(void)
{
    TEST_ASSERT_EQUAL(XC_OK, open_7z("image-lzma2.7z", 0));
    size_t i = SIZE_MAX;
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_pick_image(a, &i));
    TEST_ASSERT_EQUAL_STRING("Disc.iso", xc_7z_path(a, i));
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_check(a, i, why, sizeof why));
    TEST_ASSERT_TRUE(xc_7z_packed_size(a, i) > 0);
    uint32_t crc = 0;
    TEST_ASSERT_TRUE(xc_7z_crc(a, i, &crc));
    sink s = {0, 7, false};
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_stream(a, i, 0, take, &s));
    TEST_ASSERT_FALSE(s.bad);
    TEST_ASSERT_EQUAL_UINT64(300000, s.next);
    /* From a later offset, as a second pass asks. */
    s = (sink){123457, 7, false};
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_stream(a, i, 123457, take, &s));
    TEST_ASSERT_FALSE(s.bad);
    TEST_ASSERT_EQUAL_UINT64(300000, s.next);
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_stream(a, i, 300000, take, &s));
    TEST_ASSERT_EQUAL(XC_ERR_RANGE, xc_7z_stream(a, i, 300001, take, &s));
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("two-images.7z", 0)); /* an image and an .xbe: one image */
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_pick_image(a, &i));
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("several-images.7z", 0));
    TEST_ASSERT_EQUAL(XC_ERR_ZIP_MEMBER, xc_7z_pick_image(a, &i));
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-lzma2-solid.7z", 0));
    TEST_ASSERT_EQUAL(XC_ERR_ZIP_MEMBER, xc_7z_pick_image(a, &i));
}

static void test_unsupported(void)
{
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-ppmd.7z", 0));
    size_t i = find("Game/default.xbe");
    TEST_ASSERT_EQUAL(XC_ERR_7Z_UNSUPPORTED, xc_7z_check(a, i, why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "PPMd"));
    uint8_t b;
    TEST_ASSERT_EQUAL(XC_ERR_7Z_UNSUPPORTED, xc_7z_read(a, i, 0, &b, 1));
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-password.7z", 0));
    TEST_ASSERT_EQUAL(XC_ERR_7Z_UNSUPPORTED,
                      xc_7z_check(a, find("Game/default.xbe"), why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "password"));
    /* Empty files have no data to decode. */
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_check(a, find("Game/media/empty.bin"), why, sizeof why));
}

static void test_memory_bound(void)
{
    /* The solid block is ~712 KB: its dictionary needn't be more than that (§4). */
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-lzma2-solid.7z", 1U << 20));
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_check(a, find("Game/default.xbe"), why, sizeof why));
    check_files(65536);
    tearDown();
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-lzma2-solid.7z", 64U << 10));
    TEST_ASSERT_EQUAL(XC_ERR_7Z_MEMORY, xc_7z_check(a, find("Game/default.xbe"), why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "dictionary"));
}

static void test_damage(void)
{
    /* A flipped byte in the packed data: refused, by the decoder or the CRC-32. */
    TEST_ASSERT_EQUAL(XC_OK, open_7z("image-lzma2.7z", 0));
    data[200] ^= 0x5A;
    size_t i = 0;
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_pick_image(a, &i));
    sink s = {0, 7, false};
    xc_err err = xc_7z_stream(a, i, 0, take, &s);
    TEST_ASSERT_TRUE(err == XC_ERR_ZIP_CRC || err == XC_ERR_CORRUPT);
    tearDown();
    /* Cut short, or not a 7z at all. */
    load("image-lzma2.7z");
    src.size = 300;
    TEST_ASSERT_EQUAL(XC_ERR_NOT_7Z, xc_7z_open(&src, 0, &a, why, sizeof why));
    TEST_ASSERT_NULL(a);
    memset(data, 'x', 64);
    src.size = data_len;
    TEST_ASSERT_EQUAL(XC_ERR_NOT_7Z, xc_7z_open(&src, 0, &a, why, sizeof why));
}

static void test_folder(void)
{
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-lzma2-solid.7z", 0));
    xc_arc_folder *f = NULL;
    xc_source image;
    TEST_ASSERT_EQUAL(XC_OK, xc_7z_folder_open(a, &f, &image, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("Game", xc_arc_folder_root(f));
    TEST_ASSERT_EQUAL_size_t(6, xc_arc_folder_files(f));
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&image, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&image, &vol, NULL, &l));
    TEST_ASSERT_EQUAL_size_t(6, l.file_count);
    TEST_ASSERT_EQUAL_size_t(3, l.dir_count); /* media, data, empty-dir */
    /* The files, read through the image in its order (the archive's): each byte as made. */
    for (size_t k = 0; k < l.file_count; k++) {
        const xc_xdvdfs_entry *e = &l.files[k];
        char full[300];
        (void)snprintf(full, sizeof full, "Game/%s", e->path);
        const expect *x = NULL;
        for (size_t g = 0; g < GAME_N; g++) {
            if (strcmp(game[g].path, full) == 0) {
                x = &game[g];
            }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(x, full);
        TEST_ASSERT_EQUAL_UINT64(x->size, e->size);
        uint8_t *want = x->random ? lcg(x->k, (size_t)x->size) : NULL;
        uint8_t *buf = malloc((size_t)e->size + 1U);
        if (e->size > 0) {
            TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&image, e->offset, buf, (size_t)e->size));
        }
        for (uint64_t b = 0; b < e->size; b++) {
            if (buf[b] != (x->random ? want[b] : pattern(x->k, b, false))) {
                TEST_FAIL_MESSAGE(full);
            }
        }
        free(buf);
        free(want);
    }
    xc_xdvdfs_listing_free(&l);
    xc_arc_folder_free(f);
    tearDown();
    /* PPMd: refused before anything is read. */
    TEST_ASSERT_EQUAL(XC_OK, open_7z("game-ppmd.7z", 0));
    TEST_ASSERT_EQUAL(XC_ERR_7Z_UNSUPPORTED, xc_7z_folder_open(a, &f, &image, why, sizeof why));
    TEST_ASSERT_NULL(f);
    TEST_ASSERT_NOT_NULL(strstr(why, "PPMd"));
}

static void test_names(void)
{
    TEST_ASSERT_TRUE(xc_7z_is_7z_name("Halo (USA).7z"));
    TEST_ASSERT_TRUE(xc_7z_is_7z_name("smb://nas/x/Halo.7Z"));
    TEST_ASSERT_TRUE(xc_7z_is_7z_name("http://h/a.7z?x=1"));
    TEST_ASSERT_FALSE(xc_7z_is_7z_name("a.zip"));
    TEST_ASSERT_FALSE(xc_7z_is_7z_name("7z"));
    TEST_ASSERT_FALSE(xc_7z_is_7z_name(NULL));
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : "core/tests/data/7z";
    UNITY_BEGIN();
    RUN_TEST(test_methods);
    RUN_TEST(test_copy);
    RUN_TEST(test_image_stream);
    RUN_TEST(test_unsupported);
    RUN_TEST(test_memory_bound);
    RUN_TEST(test_damage);
    RUN_TEST(test_folder);
    RUN_TEST(test_names);
    return UNITY_END();
}
