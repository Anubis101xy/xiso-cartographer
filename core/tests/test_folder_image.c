/* Tests for game folders as images (xcat/folder_image.h, docs/formats/folder-image.md): every
   image built is read back with the XDVDFS reader (docs/formats/xdvdfs.md). */
#include "unity.h"
#include "xcat/folder_image.h"
#include "xcat/xdvdfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* File contents: a pattern of the item and the offset, so misplaced bytes show. */
static uint8_t pattern(size_t item, uint64_t offset)
{
    return (uint8_t)(item * 31U + offset * 7U + (offset >> 11));
}

typedef struct reader {
    unsigned calls;
    size_t fail_item; /* SIZE_MAX: none */
} reader;

static xc_err read_item(void *ctx, size_t item, uint64_t offset, void *buf, size_t len)
{
    reader *r = ctx;
    r->calls++;
    if (item == r->fail_item) {
        return XC_ERR_NET_IO;
    }
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = pattern(item, offset + i);
    }
    return XC_OK;
}

static reader rd;
static xc_fimg *img;
static xc_source src;
static xc_fimg_problem why;

static xc_err build(const xc_fimg_item *items, size_t n)
{
    xc_fimg_free(img);
    img = NULL;
    rd.calls = 0;
    rd.fail_item = SIZE_MAX;
    return xc_fimg_build(items, n, read_item, &rd, &img, &src, &why);
}

/* The item whose path is `path`, or SIZE_MAX. */
static size_t item_of(const xc_fimg_item *items, size_t n, const char *path)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(items[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Lists the image with the XDVDFS reader and checks every file and folder against `items`. */
static void check_listing(const xc_fimg_item *items, size_t n)
{
    xc_xdvdfs_volume vol;
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL(XC_XDVDFS_XISO, vol.layout);
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    size_t files = 0;
    size_t dirs = 0;
    for (size_t i = 0; i < n; i++) {
        if (items[i].dir) {
            dirs++;
        } else {
            files++;
        }
    }
    TEST_ASSERT_EQUAL_size_t(files, l.file_count);
    TEST_ASSERT_EQUAL_size_t(dirs, l.dir_count);
    TEST_ASSERT_EQUAL_size_t(files, xc_fimg_file_count(img));
    uint8_t *buf = malloc(70000);
    TEST_ASSERT_NOT_NULL(buf);
    for (size_t i = 0; i < l.file_count; i++) {
        const xc_xdvdfs_entry *e = &l.files[i];
        size_t it = item_of(items, n, e->path);
        TEST_ASSERT_TRUE_MESSAGE(it != SIZE_MAX, e->path);
        TEST_ASSERT_FALSE(items[it].dir);
        TEST_ASSERT_EQUAL_UINT64(items[it].size, e->size);
        TEST_ASSERT_EQUAL_UINT64(0, e->offset % 2048U);
        /* The file's first and last bytes (all of a small one) come from its own item. */
        size_t len = e->size < 70000U ? e->size : 70000U;
        if (len > 0) {
            TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, e->offset, buf, len));
            for (size_t j = 0; j < len; j++) {
                TEST_ASSERT_EQUAL_HEX8(pattern(it, j), buf[j]);
            }
            TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, e->offset + e->size - 1U, buf, 1));
            TEST_ASSERT_EQUAL_HEX8(pattern(it, e->size - 1U), buf[0]);
        }
    }
    for (size_t i = 0; i < l.dir_count; i++) {
        size_t it = item_of(items, n, l.dirs[i].path);
        TEST_ASSERT_TRUE_MESSAGE(it != SIZE_MAX, l.dirs[i].path);
        TEST_ASSERT_TRUE(items[it].dir);
    }
    free(buf);
    xc_xdvdfs_listing_free(&l);
}

static const xc_fimg_item game[] = {
    {"media/intro.xmv", 70000, 5, false, 0},
    {"default.xbe", 5000, 1, false, 0},
    {"media", 0, 0, true, 0},
    {"media/empty", 0, 0, true, 0},
    {"media/zero.bin", 0, 2, false, 0},
    {"Media2", 0, 0, true, 0},
    {"Media2/sub", 0, 0, true, 0},
    {"Media2/sub/deep.dat", 2048, 3, false, 0},
    {"a", 1, 4, false, 0},
    {"B", 2049, 4, false, 0},
    {"c d (e) [f] é.txt", 12, 4, false, 0},
};
#define GAME_N (sizeof game / sizeof game[0])

static void test_game(void)
{
    TEST_ASSERT_EQUAL(XC_OK, build(game, GAME_N));
    check_listing(game, GAME_N);
    TEST_ASSERT_EQUAL_UINT64(70000U + 5000U + 2048U + 1U + 2049U + 12U, xc_fimg_file_bytes(img));
    TEST_ASSERT_EQUAL_UINT64(0, src.size % 2048U);
    TEST_ASSERT_NULL(src.read_stream);
}

static void test_layout(void)
{
    TEST_ASSERT_EQUAL(XC_OK, build(game, GAME_N));
    uint8_t b[4096];
    /* Nothing but zeros before the descriptor, which has its two magics. */
    TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, 0, b, sizeof b));
    for (size_t i = 0; i < sizeof b; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, b[i]);
    }
    TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, 0x10000, b, 2048));
    TEST_ASSERT_EQUAL_MEMORY("MICROSOFT*XBOX*MEDIA", b, 20);
    TEST_ASSERT_EQUAL_MEMORY("MICROSOFT*XBOX*MEDIA", b + 0x7EC, 20);
    TEST_ASSERT_EQUAL_HEX8(33, b[0x14]); /* the root table right after it */
    /* Padding after a file is zeros: "a" is 1 byte, followed by 2047 of padding. */
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    uint64_t a = UINT64_MAX;
    for (size_t i = 0; i < l.file_count; i++) {
        if (strcmp(l.files[i].path, "a") == 0) {
            a = l.files[i].offset;
        }
    }
    TEST_ASSERT_TRUE(a != UINT64_MAX);
    TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, a, b, 2048));
    for (size_t i = 1; i < 2048; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, b[i]);
    }
    /* Files are in path order (§4), so one read can cover several. */
    for (size_t i = 1; i < l.file_count; i++) {
        TEST_ASSERT_TRUE(l.files[i - 1].offset <= l.files[i].offset);
    }
    /* A read that spans every file at once. */
    uint64_t data = l.files[0].offset;
    size_t span = (size_t)(src.size - data);
    uint8_t *all = malloc(span);
    TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&src, data, all, span));
    for (size_t i = 0; i < l.file_count; i++) {
        const xc_xdvdfs_entry *e = &l.files[i];
        size_t it = item_of(game, GAME_N, e->path);
        for (uint64_t j = 0; j < e->size; j++) {
            TEST_ASSERT_EQUAL_HEX8(pattern(it, j), all[e->offset - data + j]);
        }
    }
    free(all);
    xc_xdvdfs_listing_free(&l);
    TEST_ASSERT_EQUAL(XC_ERR_RANGE, src.read_at(src.ctx, src.size - 1U, b, 2));
    TEST_ASSERT_EQUAL(XC_OK, src.read_at(src.ctx, src.size, b, 0));
}

static void test_read_error(void)
{
    TEST_ASSERT_EQUAL(XC_OK, build(game, GAME_N));
    rd.fail_item = 1; /* default.xbe */
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l)); /* tables: no callback */
    TEST_ASSERT_EQUAL_UINT(0, rd.calls);
    uint8_t b[16];
    for (size_t i = 0; i < l.file_count; i++) {
        if (strcmp(l.files[i].path, "default.xbe") == 0) {
            TEST_ASSERT_EQUAL(XC_ERR_NET_IO, xc_source_read(&src, l.files[i].offset, b, 16));
        }
    }
    xc_xdvdfs_listing_free(&l);
}

static void test_empty(void)
{
    TEST_ASSERT_EQUAL(XC_OK, build(NULL, 0));
    check_listing(NULL, 0);
    TEST_ASSERT_EQUAL_UINT64(33U * 2048U, src.size);
    static const xc_fimg_item dirs[] = {{"x", 0, 0, true, 0}, {"x/y", 0, 0, true, 0}};
    TEST_ASSERT_EQUAL(XC_OK, build(dirs, 2));
    check_listing(dirs, 2);
}

/* A folder with many entries: its table takes several sectors, no entry crossing one. */
static void test_big_folder(void)
{
    enum { N = 3000 };
    static char names[N][64];
    static xc_fimg_item items[N + 1];
    items[0] = (xc_fimg_item){"d", 0, 0, true, 0};
    for (size_t i = 0; i < N; i++) {
        (void)snprintf(names[i], sizeof names[i], "d/file %04zu with a longish name.bin",
                       (size_t)((i * 7919U) % N));
        items[i + 1] = (xc_fimg_item){names[i], i % 5U, 0, false, 0};
    }
    TEST_ASSERT_EQUAL(XC_OK, build(items, N + 1));
    check_listing(items, N + 1);
}

/* §3: a table's entries must start within 0xFFFE * 4 bytes. */
static void test_folder_too_big(void)
{
    enum { N = 1100 };
    static char names[N][260];
    static xc_fimg_item items[N];
    for (size_t i = 0; i < N; i++) {
        memset(names[i], 'n', 255);
        (void)snprintf(names[i], 8, "%04zu", i);
        names[i][4] = 'n';
        names[i][255] = '\0';
        items[i] = (xc_fimg_item){names[i], 0, 0, false, 0};
    }
    TEST_ASSERT_EQUAL(XC_OK, build(items, 880)); /* 7 entries of 272 bytes a sector: fits */
    check_listing(items, 880);
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(items, N));
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, why.item); /* the root folder */
    TEST_ASSERT_NOT_NULL(strstr(why.why, "too many"));
}

static void test_case_clash(void)
{
    static const xc_fimg_item items[] = {
        {"Media", 0, 0, true, 0}, {"x", 1, 0, false, 0}, {"media", 3, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(items, 3));
    TEST_ASSERT_TRUE(why.item == 0 || why.item == 2);
    TEST_ASSERT_NOT_NULL(strstr(why.why, "upper and lower case"));
    /* The same names in different folders are fine. */
    static const xc_fimg_item ok[] = {{"a", 0, 0, true, 0},
                                      {"b", 0, 0, true, 0},
                                      {"a/X.bin", 1, 0, false, 0},
                                      {"b/x.bin", 1, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_OK, build(ok, 4));
    check_listing(ok, 4);
}

static void expect_refused(const char *path, bool dir, uint64_t size, xc_err err)
{
    xc_fimg_item items[2] = {{"ok", 0, 0, true, 0}, {path, size, 0, dir, 0}};
    TEST_ASSERT_EQUAL_MESSAGE(err, build(items, 2), path);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, why.item, path);
}

static void test_bad_names(void)
{
    expect_refused("", false, 1, XC_ERR_GAME_FOLDER);
    expect_refused("/a", false, 1, XC_ERR_GAME_FOLDER);
    expect_refused("ok/", false, 1, XC_ERR_GAME_FOLDER);
    expect_refused("ok//a", false, 1, XC_ERR_GAME_FOLDER);
    expect_refused(".", true, 0, XC_ERR_GAME_FOLDER);
    expect_refused("ok/..", true, 0, XC_ERR_GAME_FOLDER);
    expect_refused("a\\b", false, 1, XC_ERR_GAME_FOLDER);
    expect_refused("a\tb", false, 1, XC_ERR_GAME_FOLDER);
    static char long_name[300];
    memset(long_name, 'x', 256);
    expect_refused(long_name, false, 1, XC_ERR_GAME_FOLDER);
    long_name[255] = '\0';
    xc_fimg_item fine[1] = {{long_name, 1, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_OK, build(fine, 1));
    check_listing(fine, 1);
    /* 4 GiB - 1 is the largest file; it's never read here. */
    expect_refused("huge", false, (uint64_t)UINT32_MAX + 1U, XC_ERR_GAME_FOLDER);
    xc_fimg_item big[1] = {{"big", UINT32_MAX, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_OK, build(big, 1));
    TEST_ASSERT_TRUE(src.size > UINT32_MAX);
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, l.files[0].size);
    xc_xdvdfs_listing_free(&l);
}

static void test_structure(void)
{
    expect_refused("missing/a", false, 1, XC_ERR_INVALID_ARG); /* its folder isn't listed */
    static const xc_fimg_item under_file[] = {{"f", 1, 0, false, 0}, {"f/a", 1, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_INVALID_ARG, build(under_file, 2));
    static const xc_fimg_item twice[] = {{"f", 1, 0, false, 0}, {"f", 1, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_INVALID_ARG, build(twice, 2));
    static const xc_fimg_item no_path[] = {{NULL, 1, 0, false, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_INVALID_ARG, build(no_path, 1));
    TEST_ASSERT_EQUAL(XC_ERR_INVALID_ARG, xc_fimg_build(NULL, 1, read_item, &rd, &img, &src, NULL));
    TEST_ASSERT_EQUAL(XC_ERR_INVALID_ARG, xc_fimg_build(game, 1, NULL, &rd, &img, &src, NULL));
}

static void test_depth(void)
{
    static char paths[34][80];
    static xc_fimg_item items[34];
    for (size_t i = 0; i < 34; i++) {
        size_t n = 0;
        for (size_t j = 0; j <= i; j++) {
            n += (size_t)snprintf(paths[i] + n, sizeof paths[i] - n, j == 0 ? "d" : "/d");
        }
        items[i] = (xc_fimg_item){paths[i], 0, 0, true, 0};
    }
    TEST_ASSERT_EQUAL(XC_OK, build(items, 32));
    check_listing(items, 32);
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(items, 33));
    TEST_ASSERT_EQUAL_size_t(32, why.item);
}

static void test_validator(void)
{
    TEST_ASSERT_EQUAL(XC_OK, build(game, GAME_N));
    char first[XC_FIMG_VALIDATOR_HEX + 1];
    (void)snprintf(first, sizeof first, "%s", xc_fimg_validator(img));
    TEST_ASSERT_EQUAL_size_t(40, strlen(first));
    /* The order the platform lists them in doesn't matter. */
    xc_fimg_item copy[GAME_N];
    for (size_t i = 0; i < GAME_N; i++) {
        copy[i] = game[GAME_N - 1U - i];
    }
    TEST_ASSERT_EQUAL(XC_OK, build(copy, GAME_N));
    TEST_ASSERT_EQUAL_STRING(first, xc_fimg_validator(img));
    copy[3].mtime++;
    TEST_ASSERT_EQUAL(XC_OK, build(copy, GAME_N));
    TEST_ASSERT_TRUE(strcmp(first, xc_fimg_validator(img)) != 0);
    copy[3].mtime--;
    copy[0].size++;
    TEST_ASSERT_EQUAL(XC_OK, build(copy, GAME_N));
    TEST_ASSERT_TRUE(strcmp(first, xc_fimg_validator(img)) != 0);
    copy[0].size--;
    TEST_ASSERT_EQUAL(XC_OK, build(copy, GAME_N));
    TEST_ASSERT_EQUAL_STRING(first, xc_fimg_validator(img));
}

/* §4: `order` lays the files' data out, then path order breaks ties. */
static void test_data_order(void)
{
    static const xc_fimg_item items[] = {
        {"a.bin", 3000, 0, false, 2}, {"b.bin", 10, 0, false, 0}, {"d", 0, 0, true, 0},
        {"d/c.bin", 5, 0, false, 1},  {"e.bin", 7, 0, false, 1},
    };
    TEST_ASSERT_EQUAL(XC_OK, build(items, 5));
    check_listing(items, 5);
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    /* The reader lists files by offset: b (0), d/c and e (1, by path), a (2). */
    TEST_ASSERT_EQUAL_STRING("b.bin", l.files[0].path);
    TEST_ASSERT_EQUAL_STRING("d/c.bin", l.files[1].path);
    TEST_ASSERT_EQUAL_STRING("e.bin", l.files[2].path);
    TEST_ASSERT_EQUAL_STRING("a.bin", l.files[3].path);
    xc_xdvdfs_listing_free(&l);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_game);
    RUN_TEST(test_layout);
    RUN_TEST(test_read_error);
    RUN_TEST(test_empty);
    RUN_TEST(test_big_folder);
    RUN_TEST(test_folder_too_big);
    RUN_TEST(test_case_clash);
    RUN_TEST(test_bad_names);
    RUN_TEST(test_structure);
    RUN_TEST(test_depth);
    RUN_TEST(test_validator);
    RUN_TEST(test_data_order);
    xc_fimg_free(img);
    return UNITY_END();
}
