/* Tests for game and app folders in archives (xcat/arc_folder.h, xc_zip_folder_open;
   docs/formats/folder-image.md §7). */
#include "unity.h"
#include "xcat/arc_folder.h"
#include "xcat/xdvdfs.h"
#include "xcat/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *dir;

void setUp(void) {}
void tearDown(void) {}

/* make.py's pattern(), and its LCG for random files. */
static uint8_t pattern(unsigned k, uint64_t i)
{
    return (uint8_t)(k * 31U + i * 7U + (i >> 11));
}

static xc_err fake_read(void *ctx, size_t member, uint64_t offset, void *buf, size_t len)
{
    (void)ctx;
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = pattern((unsigned)member, offset + i);
    }
    return XC_OK;
}

static xc_arc_folder *f;
static xc_source image;
static char why[200];
static xc_xdvdfs_listing l;

static xc_err build(const xc_arc_member *m, size_t n)
{
    xc_arc_folder_free(f);
    f = NULL;
    xc_xdvdfs_listing_free(&l);
    return xc_arc_folder_build(m, n, fake_read, NULL, &f, &image, why, sizeof why);
}

static void list(void)
{
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing_free(&l);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&image, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&image, &vol, NULL, &l));
}

static bool has_file(const char *path)
{
    for (size_t i = 0; i < l.file_count; i++) {
        if (strcmp(l.files[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_dir(const char *path)
{
    for (size_t i = 0; i < l.dir_count; i++) {
        if (strcmp(l.dirs[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

/* §7.1-§7.3: the shallowest default.xbe's folder; what's outside it, and clutter, left out;
   folders no member names made up; '\' as '/'. */
static void test_root_and_contents(void)
{
    static const xc_arc_member m[] = {
        {"readme.txt", 10, false, 0, 0},
        {"Halo/", 0, true, 1, 0},
        {"Halo/DEFAULT.XBE", 5000, false, 2, 0},
        {"Halo\\media\\intro.xmv", 3000, false, 3, 0},
        {"Halo/.DS_Store", 9, false, 4, 0},
        {"__MACOSX/Halo/._default.xbe", 9, false, 5, 0},
        {"Halo/tools/default.xbe", 100, false, 6, 0},
        {"./Halo/deep/er/file.bin", 7, false, 7, 0},
    };
    TEST_ASSERT_EQUAL(XC_OK, build(m, sizeof m / sizeof m[0]));
    TEST_ASSERT_EQUAL_STRING("Halo", xc_arc_folder_root(f));
    TEST_ASSERT_EQUAL_size_t(4, xc_arc_folder_files(f));
    TEST_ASSERT_EQUAL_UINT64(5000 + 3000 + 100 + 7, xc_arc_folder_bytes(f));
    TEST_ASSERT_EQUAL_size_t(40, strlen(xc_arc_folder_validator(f)));
    list();
    TEST_ASSERT_TRUE(has_file("DEFAULT.XBE"));
    TEST_ASSERT_TRUE(has_file("media/intro.xmv"));
    TEST_ASSERT_TRUE(has_file("tools/default.xbe"));
    TEST_ASSERT_TRUE(has_file("deep/er/file.bin"));
    TEST_ASSERT_FALSE(has_file(".DS_Store"));
    TEST_ASSERT_TRUE(has_dir("media"));
    TEST_ASSERT_TRUE(has_dir("deep/er"));
    TEST_ASSERT_TRUE(xc_arc_folder_uses(f, 2));
    TEST_ASSERT_FALSE(xc_arc_folder_uses(f, 0)); /* readme.txt */
    TEST_ASSERT_FALSE(xc_arc_folder_uses(f, 4)); /* .DS_Store */
    /* Bytes come from their own member. */
    for (size_t i = 0; i < l.file_count; i++) {
        if (strcmp(l.files[i].path, "media/intro.xmv") == 0) {
            uint8_t b[2];
            TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&image, l.files[i].offset + 100, b, 2));
            TEST_ASSERT_EQUAL_HEX8(pattern(3, 100), b[0]);
        }
    }
}

static void test_top_level_game(void)
{
    static const xc_arc_member m[] = {
        {"default.xbe", 10, false, 0, 0},
        {"data/a.bin", 10, false, 1, 0},
    };
    TEST_ASSERT_EQUAL(XC_OK, build(m, 2));
    TEST_ASSERT_EQUAL_STRING("", xc_arc_folder_root(f));
    list();
    TEST_ASSERT_TRUE(has_file("default.xbe"));
    TEST_ASSERT_TRUE(has_file("data/a.bin"));
}

static void test_refusals(void)
{
    static const xc_arc_member none[] = {{"Halo/readme.txt", 1, false, 0, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_ZIP_MEMBER, build(none, 1));
    static const xc_arc_member two[] = {{"A/default.xbe", 1, false, 0, 0},
                                        {"B/default.xbe", 1, false, 1, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(two, 2));
    TEST_ASSERT_NOT_NULL(strstr(why, "several games"));
    static const xc_arc_member dots[] = {{"A/default.xbe", 1, false, 0, 0},
                                         {"A/../evil.bin", 1, false, 1, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(dots, 2));
    TEST_ASSERT_NOT_NULL(strstr(why, ".."));
    static const xc_arc_member twice[] = {
        {"A/default.xbe", 1, false, 0, 0}, {"A/x", 1, false, 1, 0}, {"A/x", 2, false, 2, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(twice, 3));
    static const xc_arc_member file_and_dir[] = {
        {"A/default.xbe", 1, false, 0, 0}, {"A/x", 1, false, 1, 0}, {"A/x/y", 2, false, 2, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(file_and_dir, 3));
    static const xc_arc_member clash[] = {{"A/default.xbe", 1, false, 0, 0},
                                          {"A/Data/x", 1, false, 1, 0},
                                          {"A/data/y", 1, false, 2, 0}};
    TEST_ASSERT_EQUAL(XC_ERR_GAME_FOLDER, build(clash, 3));
    TEST_ASSERT_NOT_NULL(strstr(why, "upper and lower case"));
    TEST_ASSERT_TRUE(xc_arc_is_default_xbe("a/b/Default.Xbe"));
    TEST_ASSERT_FALSE(xc_arc_is_default_xbe("a/default.xbe.bak"));
}

/* A real zip (tools/fixtures/sevenz/make_zip.py): Deflate and stored members, inflated as read. */
static void test_zip_folder(void)
{
    char path[512];
    (void)snprintf(path, sizeof path, "%s/app-folder.zip", dir);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    (void)fseek(fp, 0, SEEK_END);
    size_t n = (size_t)ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc(n);
    TEST_ASSERT_EQUAL_size_t(n, fread(data, 1, n, fp));
    (void)fclose(fp);
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, data, n);
    xc_zip_listing zl;
    TEST_ASSERT_EQUAL(XC_OK, xc_zip_list(&src, &zl));
    size_t index = 0;
    TEST_ASSERT_EQUAL(XC_ERR_ZIP_MEMBER, xc_zip_pick_image(&zl, &index));
    xc_zip_folder *z = NULL;
    xc_source img;
    TEST_ASSERT_EQUAL(XC_OK, xc_zip_folder_open(&src, &zl, &z, &img, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("My App", xc_arc_folder_root(xc_zip_folder_arc(z)));
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing zl2;
    memset(&zl2, 0, sizeof zl2);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&img, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&img, &vol, NULL, &zl2));
    TEST_ASSERT_EQUAL_size_t(5, zl2.file_count); /* default.xbe, big, noise, empty, tools/ */
    for (size_t i = 0; i < zl2.file_count; i++) {
        const xc_xdvdfs_entry *e = &zl2.files[i];
        unsigned k = strcmp(e->path, "default.xbe") == 0         ? 1
                     : strcmp(e->path, "skins/big.bin") == 0     ? 2
                     : strcmp(e->path, "tools/default.xbe") == 0 ? 4
                                                                 : 0;
        if (k == 0) {
            continue; /* noise.bin (random), empty.txt */
        }
        uint8_t *buf = malloc((size_t)e->size);
        /* In two halves, the second first: the member is inflated again from its start. */
        size_t half = (size_t)e->size / 2;
        TEST_ASSERT_EQUAL(
            XC_OK, xc_source_read(&img, e->offset + half, buf + half, (size_t)e->size - half));
        TEST_ASSERT_EQUAL(XC_OK, xc_source_read(&img, e->offset, buf, half));
        for (size_t b = 0; b < e->size; b++) {
            if (buf[b] != pattern(k, b)) {
                TEST_FAIL_MESSAGE(e->path);
            }
        }
        free(buf);
    }
    xc_xdvdfs_listing_free(&zl2);
    xc_zip_folder_close(z);
    /* A flipped byte in a Deflate member: its CRC-32, or the inflater, refuses it. */
    size_t at = 0;
    for (size_t i = 0; i < zl.count; i++) {
        if (strcmp(zl.entries[i].name, "My App/default.xbe") == 0) {
            at = (size_t)zl.entries[i].local_offset + 30U + strlen(zl.entries[i].name) + 40U;
        }
    }
    data[at] ^= 0x01;
    TEST_ASSERT_EQUAL(XC_OK, xc_zip_folder_open(&src, &zl, &z, &img, why, sizeof why));
    memset(&zl2, 0, sizeof zl2);
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_open(&img, &vol));
    TEST_ASSERT_EQUAL(XC_OK, xc_xdvdfs_list(&img, &vol, NULL, &zl2));
    xc_err err = XC_OK;
    for (size_t i = 0; i < zl2.file_count; i++) {
        if (strcmp(zl2.files[i].path, "default.xbe") == 0) {
            uint8_t *buf = malloc((size_t)zl2.files[i].size);
            err = xc_source_read(&img, zl2.files[i].offset, buf, (size_t)zl2.files[i].size);
            free(buf);
        }
    }
    TEST_ASSERT_TRUE(err == XC_ERR_ZIP_CRC || err == XC_ERR_CORRUPT);
    xc_xdvdfs_listing_free(&zl2);
    xc_zip_folder_close(z);
    xc_zip_listing_free(&zl);
    free(data);
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : "core/tests/data/zip";
    UNITY_BEGIN();
    RUN_TEST(test_root_and_contents);
    RUN_TEST(test_top_level_game);
    RUN_TEST(test_refusals);
    RUN_TEST(test_zip_folder);
    xc_arc_folder_free(f);
    xc_xdvdfs_listing_free(&l);
    return UNITY_END();
}
