/*
 * Tests for the XDVDFS reader (docs/formats/xdvdfs.md).
 *
 * Usage: test_xdvdfs <fixtures-dir> <fixture-name>...
 * Fixture tests compare the listing with <name>.tsv from tools/fixtures/ (whose
 * images are cross-checked against extract-xiso in CI). The other tests build
 * small images in memory to hit each rejection rule in the spec.
 */
#include "unity.h"
#include "xcat/xdvdfs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/host.h"

#define SECTOR 2048U
#define IMG_SECTORS 40U
#define VD_SECTOR 32U
#define ROOT_SECTOR 33U
#define SUB_SECTOR 34U
#define DATA_SECTOR 35U

void setUp(void) {}
void tearDown(void) {}

/* --- in-memory images ------------------------------------------------------- */

static uint8_t img[IMG_SECTORS * SECTOR];
/* No terminator: the descriptor holds exactly these 20 bytes. */
static const uint8_t magic[20] = {'M', 'I', 'C', 'R', 'O', 'S', 'O', 'F', 'T', '*',
                                  'X', 'B', 'O', 'X', '*', 'M', 'E', 'D', 'I', 'A'};

static uint8_t *sector_ptr(uint32_t sector)
{
    return img + (size_t)sector * SECTOR;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8U);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
}

/* Blank image: volume descriptor pointing at the given root; tables pre-filled with 0xFF. */
static void img_reset(uint32_t root_sector, uint32_t root_size)
{
    memset(img, 0, sizeof img);
    uint8_t *vd = sector_ptr(VD_SECTOR);
    memcpy(vd, magic, sizeof magic);
    put32(vd + 0x14, root_sector);
    put32(vd + 0x18, root_size);
    memcpy(vd + 0x7EC, magic, sizeof magic);
    memset(sector_ptr(ROOT_SECTOR), 0xFF, SECTOR);
    memset(sector_ptr(SUB_SECTOR), 0xFF, SECTOR);
}

/* Writes one entry at byte `off` of the table in `table_sector`; returns the next 4-aligned offset.
 */
static size_t put_entry(uint32_t table_sector, size_t off, uint16_t left, uint16_t right,
                        uint32_t sector, uint32_t size, uint8_t attr, const char *name)
{
    uint8_t *e = sector_ptr(table_sector) + off;
    size_t n = strlen(name);
    put16(e, left);
    put16(e + 2, right);
    put32(e + 4, sector);
    put32(e + 8, size);
    e[12] = attr;
    e[13] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        e[14 + i] = (uint8_t)name[i]; /* names are stored without a terminator */
    }
    return (off + 14 + n + 3) & ~(size_t)3;
}

static xc_err list_img_with(size_t size, const xc_xdvdfs_limits *lim, xc_xdvdfs_listing *out)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, img, size);
    xc_xdvdfs_volume vol;
    xc_err err = xc_xdvdfs_open(&src, &vol);
    if (err != XC_OK) {
        memset(out, 0, sizeof *out);
        return err;
    }
    return xc_xdvdfs_list(&src, &vol, lim, out);
}

static xc_err list_img(xc_xdvdfs_listing *out)
{
    return list_img_with(sizeof img, NULL, out);
}

static void expect_list_err(xc_err expected)
{
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(expected, list_img(&l));
    TEST_ASSERT_NULL(l.files);
    TEST_ASSERT_EQUAL_size_t(0, l.file_count);
    xc_xdvdfs_listing_free(&l);
}

/* --- valid structures ------------------------------------------------------- */

static void test_minimal_tree(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    /* root "b" (file), left "a" (empty dir), right "c" (empty file) */
    size_t o1 = put_entry(ROOT_SECTOR, 0, 4, 0, DATA_SECTOR, 10, 0x20, "b");
    TEST_ASSERT_EQUAL_size_t(16, o1);
    put16(sector_ptr(ROOT_SECTOR) + 2, (uint16_t)(32 / 4));
    put_entry(ROOT_SECTOR, 16, 0, 0, SUB_SECTOR, SECTOR, 0x10, "a");
    put_entry(ROOT_SECTOR, 32, 0, 0, DATA_SECTOR + 1, 0, 0x20, "c");

    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list_img(&l));
    TEST_ASSERT_EQUAL_size_t(2, l.file_count);
    TEST_ASSERT_NOT_NULL(l.files);
    TEST_ASSERT_NOT_NULL(l.dirs);
    TEST_ASSERT_EQUAL_STRING("b", l.files[0].path);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)DATA_SECTOR * SECTOR, l.files[0].offset);
    TEST_ASSERT_EQUAL_UINT32(10, l.files[0].size);
    TEST_ASSERT_EQUAL_STRING("c", l.files[1].path);
    TEST_ASSERT_EQUAL_size_t(1, l.dir_count);
    TEST_ASSERT_EQUAL_STRING("a", l.dirs[0].path);
    TEST_ASSERT_EQUAL_UINT64(10, l.total_file_bytes);
    xc_xdvdfs_listing_free(&l);
    TEST_ASSERT_NULL(l.files);
}

static xc_err find(const char *name, xc_xdvdfs_entry *e, bool *found)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, img, sizeof img);
    xc_xdvdfs_volume vol;
    xc_err err = xc_xdvdfs_open(&src, &vol);
    return err != XC_OK ? err : xc_xdvdfs_find_root(&src, &vol, name, e, found);
}

/* §10: a name in the root only, any case, reading just the root's table. */
static void test_find_root(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    /* "default.xbe" takes 28 bytes: 14, its 11, padded to 4 */
    put_entry(ROOT_SECTOR, 0, 28 / 4, 44 / 4, DATA_SECTOR, 10, 0x20, "default.xbe");
    put_entry(ROOT_SECTOR, 28, 0, 0, SUB_SECTOR, SECTOR, 0x10, "a");
    put_entry(ROOT_SECTOR, 44, 0, 0, DATA_SECTOR + 1, 0, 0x20, "zz");
    put_entry(SUB_SECTOR, 0, 0, 0, DATA_SECTOR + 2, 3, 0x20, "deep");
    xc_xdvdfs_entry e;
    bool found = false;
    TEST_ASSERT_EQUAL_INT(XC_OK, find("DEFAULT.XBE", &e, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)DATA_SECTOR * SECTOR, e.offset);
    TEST_ASSERT_EQUAL_UINT32(10, e.size);
    TEST_ASSERT_NULL(e.path);
    TEST_ASSERT_EQUAL_INT(XC_OK, find("a", &e, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_HEX8(0x10, e.attributes);
    TEST_ASSERT_EQUAL_INT(XC_OK, find("zz", &e, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(XC_OK, find("deep", &e, &found)); /* not in the root */
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_INT(XC_OK, find("default.xb", &e, &found));
    TEST_ASSERT_FALSE(found);

    /* An empty root; a loop in it; a child outside it. */
    img_reset(ROOT_SECTOR, SECTOR);
    TEST_ASSERT_EQUAL_INT(XC_OK, find("a", &e, &found));
    TEST_ASSERT_FALSE(found);
    put_entry(ROOT_SECTOR, 0, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "a");
    put_entry(ROOT_SECTOR, 16, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "b");
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, find("c", &e, &found));
    TEST_ASSERT_FALSE(found);
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0x300, DATA_SECTOR, 1, 0x20, "a");
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, find("c", &e, &found));
    img_reset(0, 0); /* no files at all */
    TEST_ASSERT_EQUAL_INT(XC_OK, find("a", &e, &found));
    TEST_ASSERT_FALSE(found);
}

static void test_empty_volume(void)
{
    img_reset(0, 0);
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list_img(&l));
    TEST_ASSERT_EQUAL_size_t(0, l.file_count + l.dir_count);
    xc_xdvdfs_listing_free(&l);
}

static void test_empty_root_table(void)
{
    img_reset(ROOT_SECTOR, SECTOR); /* table is all 0xFF */
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list_img(&l));
    TEST_ASSERT_EQUAL_size_t(0, l.file_count + l.dir_count);
    xc_xdvdfs_listing_free(&l);
}

static void test_zero_size_file_sector_not_checked(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, 0xFFFFFFFFU, 0, 0x20, "empty");
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list_img(&l));
    TEST_ASSERT_NOT_NULL(l.files);
    /* 64-bit offset arithmetic: 0xFFFFFFFF * 2048 doesn't fit in 32 bits. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0xFFFFFFFFU * SECTOR, l.files[0].offset);
    xc_xdvdfs_listing_free(&l);
}

/* --- volume descriptor ------------------------------------------------------ */

static void test_no_magic_is_not_xdvdfs(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    sector_ptr(VD_SECTOR)[0] = 'X';
    expect_list_err(XC_ERR_NOT_XDVDFS);
}

static void test_too_small_is_not_xdvdfs(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_ERR_NOT_XDVDFS,
                          list_img_with((size_t)VD_SECTOR * SECTOR + 100, NULL, &l));
}

static void test_missing_tail_magic_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    sector_ptr(VD_SECTOR)[0x7EC] = 'X';
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_root_outside_image_is_corrupt(void)
{
    img_reset(1000, SECTOR);
    expect_list_err(XC_ERR_CORRUPT);
}

/* --- tree structure ---------------------------------------------------------- */

static void test_cycle_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "a");
    put_entry(ROOT_SECTOR, 16, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "b"); /* right -> itself */
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_shared_child_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 16 / 4, 32 / 4, DATA_SECTOR, 1, 0x20, "b");
    put_entry(ROOT_SECTOR, 16, 0, 32 / 4, DATA_SECTOR, 1, 0x20, "a"); /* also points at "c" */
    put_entry(ROOT_SECTOR, 32, 0, 0, DATA_SECTOR, 1, 0x20, "c");
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_child_pointing_at_padding_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 400 / 4, DATA_SECTOR, 1, 0x20, "a");
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_child_outside_table_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0xFFF0, DATA_SECTOR, 1, 0x20, "a");
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_entry_past_table_end_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 2028 / 4, DATA_SECTOR, 1, 0x20, "a");
    /* header fits (2028..2041) but the 10-byte name would end at 2052 > 2048 */
    put_entry(ROOT_SECTOR, 2028, 0, 0, DATA_SECTOR, 1, 0x20, "");
    sector_ptr(ROOT_SECTOR)[2028 + 13] = 10;
    expect_list_err(XC_ERR_CORRUPT);
}

static void test_table_past_image_end_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, IMG_SECTORS - 1, 4 * SECTOR, 0x10, "dir");
    expect_list_err(XC_ERR_CORRUPT);
}

/* --- names ------------------------------------------------------------------- */

static void test_invalid_names_are_corrupt(void)
{
    const char *bad[] = {"", ".", "..", "a/b", "a\\b", "tab\tx", "nl\nx"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        img_reset(ROOT_SECTOR, SECTOR);
        put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR, 1, 0x20, bad[i]);
        xc_xdvdfs_listing l;
        xc_err err = list_img(&l);
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_CORRUPT, err, bad[i]);
    }
}

static void test_non_ascii_names_pass_through(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR, 1, 0x20, "caf\xc3\xa9");
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, list_img(&l));
    TEST_ASSERT_NOT_NULL(l.files);
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", l.files[0].path);
    xc_xdvdfs_listing_free(&l);
}

static void test_case_insensitive_duplicates_are_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "readme");
    put_entry(ROOT_SECTOR, 16, 0, 0, DATA_SECTOR, 1, 0x20, "README");
    expect_list_err(XC_ERR_CORRUPT);
}

/* --- files ------------------------------------------------------------------- */

static void test_file_past_image_end_is_corrupt(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, IMG_SECTORS - 1, SECTOR + 1, 0x20, "truncated");
    expect_list_err(XC_ERR_CORRUPT);
}

/* --- limits ------------------------------------------------------------------ */

static void test_directory_cycle_hits_depth_limit(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, ROOT_SECTOR, SECTOR, 0x10, "loop"); /* table = root */
    expect_list_err(XC_ERR_LIMIT);
}

static void test_entry_limit(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 16 / 4, DATA_SECTOR, 1, 0x20, "a");
    put_entry(ROOT_SECTOR, 16, 0, 0, DATA_SECTOR, 1, 0x20, "b");
    xc_xdvdfs_limits lim;
    xc_xdvdfs_default_limits(&lim);
    lim.max_entries = 1;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, list_img_with(sizeof img, &lim, &l));
    TEST_ASSERT_NULL(l.files);
}

static void test_path_limit(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR, 1, 0x20, "abcd");
    xc_xdvdfs_limits lim;
    xc_xdvdfs_default_limits(&lim);
    lim.max_path_len = 3;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_ERR_LIMIT, list_img_with(sizeof img, &lim, &l));
}

/* --- source errors ----------------------------------------------------------- */

static xc_err failing_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    (void)ctx;
    (void)offset;
    (void)buf;
    (void)len;
    return XC_ERR_IO;
}

static void test_read_errors_propagate(void)
{
    xc_source src = {failing_read, NULL, sizeof img, NULL};
    xc_xdvdfs_volume vol;
    TEST_ASSERT_EQUAL_INT(XC_ERR_IO, xc_xdvdfs_open(&src, &vol));
}

static void test_invalid_arguments(void)
{
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_xdvdfs_open(NULL, &vol));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_xdvdfs_list(NULL, &vol, NULL, &l));
    xc_xdvdfs_listing_free(NULL);
    xc_xdvdfs_order o;
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_xdvdfs_stream_order(NULL, &l, &o));
}

/* --- stream order (§8) ------------------------------------------------------- */

/* Lists `img` and computes its stream order. */
static void order_of_img(xc_xdvdfs_order *o)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, img, sizeof img);
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_stream_order(&vol, &l, o));
    xc_xdvdfs_listing_free(&l);
}

static void test_stream_order_tables_first(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    /* root: "b" file (data at 35), left "a" (dir, table at 34) */
    put_entry(ROOT_SECTOR, 0, 16 / 4, 0, DATA_SECTOR, 10, 0x20, "b");
    put_entry(ROOT_SECTOR, 16, 0, 0, SUB_SECTOR, SECTOR, 0x10, "a");
    put_entry(SUB_SECTOR, 0, 0, 0, DATA_SECTOR + 1, 20, 0x20, "c");
    xc_xdvdfs_order o;
    order_of_img(&o);
    TEST_ASSERT_EQUAL_size_t(2, o.tables);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)ROOT_SECTOR * SECTOR, o.tables_start);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)DATA_SECTOR * SECTOR, o.tables_end);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)DATA_SECTOR * SECTOR, o.data_start);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(DATA_SECTOR + 1) * SECTOR + 20, o.data_end);
    TEST_ASSERT_EQUAL_size_t(0, o.late_files);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, o.first_late);
}

static void test_stream_order_table_after_its_file(void)
{
    enum { LATE_SECTOR = 38 };
    img_reset(ROOT_SECTOR, SECTOR);
    memset(sector_ptr(LATE_SECTOR), 0xFF, SECTOR);
    /* root: "early" (file at 36), right "late" (dir whose table is at 38, after its file) */
    size_t next = put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR + 1, 10, 0x20, "early");
    put16(sector_ptr(ROOT_SECTOR) + 2, (uint16_t)(next / 4)); /* right child -> "late" */
    put_entry(ROOT_SECTOR, next, 0, 0, LATE_SECTOR, SECTOR, 0x10, "late");
    put_entry(LATE_SECTOR, 0, 0, 0, DATA_SECTOR, 100, 0x20, "x"); /* data at 35 < 38 */
    xc_xdvdfs_order o;
    order_of_img(&o);
    TEST_ASSERT_EQUAL_size_t(1, o.late_files); /* "late/x"; "early" is after the root table */
    TEST_ASSERT_EQUAL_UINT64(100, o.late_bytes);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)DATA_SECTOR * SECTOR, o.first_late);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(LATE_SECTOR + 1) * SECTOR, o.last_table_end_needed);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(LATE_SECTOR + 1) * SECTOR, o.tables_end);
}

/* --- spool plan (§8.1) ------------------------------------------------------ */

/* The image of test_stream_order_table_after_its_file: root table at 33, "late/x" (100
   bytes at 35) listed in the table at 38, "early" (10 bytes at 36). */
static void late_img(void)
{
    enum { LATE_SECTOR = 38 };
    img_reset(ROOT_SECTOR, SECTOR);
    memset(sector_ptr(LATE_SECTOR), 0xFF, SECTOR);
    size_t next = put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR + 1, 10, 0x20, "early");
    put16(sector_ptr(ROOT_SECTOR) + 2, (uint16_t)(next / 4));
    put_entry(ROOT_SECTOR, next, 0, 0, LATE_SECTOR, SECTOR, 0x10, "late");
    put_entry(LATE_SECTOR, 0, 0, 0, DATA_SECTOR, 100, 0x20, "x");
}

static void spool_of_img(uint64_t budget, uint64_t checkpoint, xc_xdvdfs_spool *s)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, img, sizeof img);
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_spool_plan(&vol, &l, budget, checkpoint, s));
    xc_xdvdfs_listing_free(&l);
}

#define S ((uint64_t)SECTOR)

static void test_spool_plan_no_spool(void)
{
    late_img();
    xc_xdvdfs_spool s;
    spool_of_img(0, 0, &s);
    TEST_ASSERT_EQUAL_UINT64(39 * S, s.all_known);
    /* everything before 39 but the two tables and "early" */
    TEST_ASSERT_EQUAL_UINT64(37 * S - 10, s.unidentified);
    TEST_ASSERT_EQUAL_UINT64(100, s.late_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, s.spooled);
    TEST_ASSERT_EQUAL_UINT64(35 * S, s.reread_start);
    TEST_ASSERT_EQUAL_UINT64(35 * S, s.reread_from);
    TEST_ASSERT_EQUAL_UINT64(35 * S + 100, s.reread_end);

    spool_of_img(0, 32 * S, &s); /* pass 2 starts at the checkpoint before it */
    TEST_ASSERT_EQUAL_UINT64(35 * S, s.reread_start);
    TEST_ASSERT_EQUAL_UINT64(32 * S, s.reread_from);
}

static void test_spool_plan_partial_spool(void)
{
    late_img();
    xc_xdvdfs_spool s;
    /* holes: [0, 33), [34, 36) (with "late/x" at 35) and [36 + 10, 38). The spool keeps the
       latest bytes: the last hole alone drops all of "late/x". */
    spool_of_img(2 * S - 10, 0, &s);
    TEST_ASSERT_EQUAL_UINT64(2 * S - 10, s.spooled);
    TEST_ASSERT_EQUAL_UINT64(35 * S, s.reread_start);
    TEST_ASSERT_EQUAL_UINT64(35 * S + 100, s.reread_end);
    /* ...plus [35 + 50, 36) keeps all but its first 50 bytes */
    spool_of_img(3 * S - 60, 0, &s);
    TEST_ASSERT_EQUAL_UINT64(35 * S, s.reread_start);
    TEST_ASSERT_EQUAL_UINT64(35 * S + 50, s.reread_end);
}

static void test_spool_plan_full_spool(void)
{
    late_img();
    xc_xdvdfs_spool s;
    spool_of_img(4 * S - 10, 0, &s); /* everything from 34 on; [0, 33) is dropped */
    TEST_ASSERT_EQUAL_UINT64(4 * S - 10, s.spooled);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s.reread_start);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s.reread_from);
    spool_of_img(UINT64_MAX, 0, &s);
    TEST_ASSERT_EQUAL_UINT64(37 * S - 10, s.spooled);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s.reread_start);
}

static void test_spool_plan_tables_first(void)
{
    img_reset(ROOT_SECTOR, SECTOR);
    put_entry(ROOT_SECTOR, 0, 0, 0, DATA_SECTOR, 10, 0x20, "b");
    xc_xdvdfs_spool s;
    spool_of_img(0, 0, &s);
    TEST_ASSERT_EQUAL_UINT64(34 * S, s.all_known);
    TEST_ASSERT_EQUAL_UINT64(33 * S, s.unidentified); /* up to the root table */
    TEST_ASSERT_EQUAL_UINT64(0, s.late_bytes);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s.reread_start);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_xdvdfs_spool_plan(NULL, NULL, 0, 0, &s));
}

#undef S

/* --- fixtures ---------------------------------------------------------------- */

static const char *fixtures_dir;
static const char *fixture_name;

static int path_cmp(const void *a, const void *b)
{
    const xc_xdvdfs_entry *x = a;
    const xc_xdvdfs_entry *y = b;
    return strcmp(x->path, y->path);
}

static const char *layout_name(xc_xdvdfs_layout l)
{
    switch (l) {
    case XC_XDVDFS_XISO:
        return "xiso";
    case XC_XDVDFS_XGD1:
        return "xgd1";
    case XC_XDVDFS_XGD2:
        return "xgd2";
    case XC_XDVDFS_XGD3:
        return "xgd3";
    }
    return "?";
}

/*
 * Splits "kind\tA\tB\tC\tpath" in place. Returns the number of fields found
 * (max 5); fields not present are set to "".
 */
static int split_tsv(char *line, char *fields[5])
{
    static char empty[1];
    char *eol = strpbrk(line, "\r\n");
    if (eol != NULL) {
        *eol = '\0';
    }
    for (int i = 0; i < 5; i++) {
        fields[i] = empty;
    }
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p != '\0' && n < 5; p++) {
        if (*p == '\t') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void test_fixture(void)
{
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s.iso", fixtures_dir, fixture_name) > 0);
    xc_host_file fs = {NULL};
    xc_source src;
    TEST_ASSERT_EQUAL_INT_MESSAGE(XC_OK, xc_host_file_open(&src, &fs, path), path);

    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_open(&src, &vol));
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_list(&src, &vol, NULL, &l));
    xc_host_file_close(&fs);

    xc_xdvdfs_entry *dirs = NULL;
    if (l.dir_count > 0) {
        dirs = malloc(l.dir_count * sizeof *dirs);
        TEST_ASSERT_NOT_NULL(dirs);
        memcpy(dirs, l.dirs, l.dir_count * sizeof *dirs);
        qsort(dirs, l.dir_count, sizeof *dirs, path_cmp); /* the manifest sorts dirs by path */
    }

    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s.tsv", fixtures_dir, fixture_name) > 0);
    FILE *tsv = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(tsv, path);
    char line[2048];
    size_t nf = 0;
    size_t nd = 0;
    uint64_t total = 0;
    while (fgets(line, sizeof line, tsv) != NULL) {
        char *f[5];
        int n = split_tsv(line, f);
        if (line[0] == '#') {
            continue;
        }
        if (strcmp(f[0], "layout") == 0) {
            TEST_ASSERT_EQUAL_INT(3, n);
            TEST_ASSERT_EQUAL_STRING(f[1], layout_name(vol.layout));
            TEST_ASSERT_EQUAL_UINT64(strtoull(f[2], NULL, 10), vol.partition_offset);
        } else if (strcmp(f[0], "root") == 0) {
            TEST_ASSERT_EQUAL_INT(3, n);
            TEST_ASSERT_EQUAL_UINT32(strtoul(f[1], NULL, 10), vol.root_sector);
            TEST_ASSERT_EQUAL_UINT32(strtoul(f[2], NULL, 10), vol.root_size);
        } else {
            TEST_ASSERT_EQUAL_INT(5, n);
            bool is_file = strcmp(f[0], "file") == 0;
            TEST_ASSERT_TRUE_MESSAGE(is_file || strcmp(f[0], "dir") == 0, f[0]);
            const xc_xdvdfs_entry *e = NULL;
            if (is_file) {
                TEST_ASSERT_TRUE_MESSAGE(nf < l.file_count, "more files in manifest than listed");
                e = &l.files[nf++];
                total += strtoull(f[2], NULL, 10);
            } else {
                TEST_ASSERT_TRUE_MESSAGE(nd < l.dir_count, "more dirs in manifest than listed");
                e = &dirs[nd++];
            }
            TEST_ASSERT_EQUAL_STRING(f[4], e->path);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(strtoull(f[1], NULL, 10), e->offset, f[4]);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(strtoul(f[2], NULL, 10), e->size, f[4]);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(strtoul(f[3], NULL, 10), e->sector, f[4]);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(tsv));
    TEST_ASSERT_EQUAL_size_t(l.file_count, nf);
    TEST_ASSERT_EQUAL_size_t(l.dir_count, nd);
    TEST_ASSERT_EQUAL_UINT64(total, l.total_file_bytes);

    /* The generator writes every table before any file data, or with "late*" fixtures the
       root table after every file (tools/fixtures/README.md). */
    xc_xdvdfs_order o;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_xdvdfs_stream_order(&vol, &l, &o));
    TEST_ASSERT_EQUAL_size_t(l.dir_count + 1, o.tables);
    if (strncmp(fixture_name, "late", 4) == 0) {
        size_t nonempty = 0;
        for (size_t i = 0; i < l.file_count; i++) {
            nonempty += l.files[i].size > 0;
        }
        TEST_ASSERT_EQUAL_size_t(nonempty, o.late_files); /* the root table comes last */
        TEST_ASSERT_TRUE(o.data_end <= o.tables_end);
    } else {
        TEST_ASSERT_EQUAL_size_t(0, o.late_files);
        TEST_ASSERT_TRUE(o.tables_end <= o.data_start);
    }
    free(dirs);
    xc_xdvdfs_listing_free(&l);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_minimal_tree);
    RUN_TEST(test_find_root);
    RUN_TEST(test_empty_volume);
    RUN_TEST(test_empty_root_table);
    RUN_TEST(test_zero_size_file_sector_not_checked);
    RUN_TEST(test_no_magic_is_not_xdvdfs);
    RUN_TEST(test_too_small_is_not_xdvdfs);
    RUN_TEST(test_missing_tail_magic_is_corrupt);
    RUN_TEST(test_root_outside_image_is_corrupt);
    RUN_TEST(test_cycle_is_corrupt);
    RUN_TEST(test_shared_child_is_corrupt);
    RUN_TEST(test_child_pointing_at_padding_is_corrupt);
    RUN_TEST(test_child_outside_table_is_corrupt);
    RUN_TEST(test_entry_past_table_end_is_corrupt);
    RUN_TEST(test_table_past_image_end_is_corrupt);
    RUN_TEST(test_invalid_names_are_corrupt);
    RUN_TEST(test_non_ascii_names_pass_through);
    RUN_TEST(test_case_insensitive_duplicates_are_corrupt);
    RUN_TEST(test_file_past_image_end_is_corrupt);
    RUN_TEST(test_directory_cycle_hits_depth_limit);
    RUN_TEST(test_entry_limit);
    RUN_TEST(test_path_limit);
    RUN_TEST(test_read_errors_propagate);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_stream_order_tables_first);
    RUN_TEST(test_stream_order_table_after_its_file);
    RUN_TEST(test_spool_plan_no_spool);
    RUN_TEST(test_spool_plan_partial_spool);
    RUN_TEST(test_spool_plan_full_spool);
    RUN_TEST(test_spool_plan_tables_first);

    if (argc > 1) {
        fixtures_dir = argv[1];
        for (int i = 2; i < argc; i++) {
            fixture_name = argv[i];
            UnityDefaultTestRun(test_fixture, fixture_name, __LINE__);
        }
    }
    return UNITY_END();
}
