/* Tests for XBE certificates (xcat/xbe.h, docs/formats/xbe.md). */
#include "unity.h"
#include "xcat/xbe.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define BASE 0x10000U
#define CERT_OFF 0x180U

static uint8_t xbe[0x1000];

static void put32(size_t off, uint32_t v)
{
    xbe[off] = (uint8_t)v;
    xbe[off + 1] = (uint8_t)(v >> 8U);
    xbe[off + 2] = (uint8_t)(v >> 16U);
    xbe[off + 3] = (uint8_t)(v >> 24U);
}

/* A minimal valid XBE: headers of 0x1000 bytes, the certificate at CERT_OFF. */
static void make(const char *title)
{
    memset(xbe, 0, sizeof xbe);
    xbe[0] = 'X';
    xbe[1] = 'B';
    xbe[2] = 'E';
    xbe[3] = 'H';
    put32(0x104, BASE);
    put32(0x108, sizeof xbe);
    put32(0x118, BASE + CERT_OFF);
    put32(CERT_OFF, 0x1D0);
    put32(CERT_OFF + 8, 0x4D530004U);
    for (size_t i = 0; title[i] != '\0' && i < XC_XBE_TITLE_UNITS; i++) {
        xbe[CERT_OFF + 0x0C + 2 * i] = (uint8_t)title[i];
    }
}

static xc_err read_xbe(size_t len, xc_xbe_cert *cert)
{
    xc_source src;
    xc_mem_source mem;
    xc_source_memory(&src, &mem, xbe, len);
    return xc_xbe_read_cert(&src, cert);
}

static void test_valid(void)
{
    xc_xbe_cert c;
    make("Game Title  ");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_xbe(sizeof xbe, &c));
    TEST_ASSERT_EQUAL_HEX32(0x4D530004U, c.title_id);
    TEST_ASSERT_EQUAL_STRING("Game Title", c.title); /* trailing spaces trimmed */

    /* All 40 units used, no terminator. */
    make("0123456789012345678901234567890123456789");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_xbe(sizeof xbe, &c));
    TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890123456789", c.title);

    /* Non-ASCII, a surrogate, control characters; an empty name. */
    make("");
    static const uint16_t units[] = {0x00E9, 0x20AC, 0xD83D, 0x0007, 0x0085, 'x', 0};
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        xbe[CERT_OFF + 0x0C + 2 * i] = (uint8_t)units[i];
        xbe[CERT_OFF + 0x0D + 2 * i] = (uint8_t)(units[i] >> 8U);
    }
    TEST_ASSERT_EQUAL_INT(XC_OK, read_xbe(sizeof xbe, &c));
    TEST_ASSERT_EQUAL_STRING("\xc3\xa9\xe2\x82\xac???x", c.title);
    make("");
    TEST_ASSERT_EQUAL_INT(XC_OK, read_xbe(sizeof xbe, &c));
    TEST_ASSERT_EQUAL_STRING("", c.title);

    /* The certificate may end exactly at the end of the headers. */
    make("T");
    put32(0x108, CERT_OFF + 0x5C);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_xbe(sizeof xbe, &c));
}

static void test_invalid(void)
{
    xc_xbe_cert c;
    make("T");
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_xbe(0x177, &c)); /* too short */
    xbe[0] = 'Y';
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_xbe(sizeof xbe, &c));

    static const struct {
        size_t off;
        uint32_t value;
    } breaks[] = {
        {0x108, 0x177},                    /* headers smaller than the image header */
        {0x108, sizeof xbe + 1},           /* headers beyond the file */
        {0x118, BASE - 1},                 /* certificate below the base */
        {0x118, BASE + 0x100},             /* inside the image header */
        {0x118, BASE + sizeof xbe - 0x5B}, /* doesn't fit in the headers */
        {0x118, 0xFFFFFFFFU},              /* far beyond */
        {CERT_OFF, 0x5B},                  /* certificate too small */
    };
    for (size_t i = 0; i < sizeof breaks / sizeof breaks[0]; i++) {
        make("T");
        put32(breaks[i].off, breaks[i].value);
        TEST_ASSERT_EQUAL_INT_MESSAGE(XC_ERR_CORRUPT, read_xbe(sizeof xbe, &c), "break");
        TEST_ASSERT_EQUAL_HEX32(0, c.title_id);
    }
}

/* ---- §4, §5: region, disc, version and the title image's section */

static uint8_t big[0x3000]; /* the headers of make(), then the sections' bytes */

static void section(size_t i, uint32_t raw, uint32_t raw_size, uint32_t name_off)
{
    size_t at = 0x400 + 56 * i;
    memset(xbe + at, 0, 56);
    put32(at + 0x0C, raw);
    put32(at + 0x10, raw_size);
    put32(at + 0x14, BASE + name_off);
}

static xc_err read_info(size_t len, xc_xbe_info *info)
{
    memcpy(big, xbe, sizeof xbe);
    xc_source src;
    xc_mem_source mem;
    xc_source_memory(&src, &mem, big, len);
    return xc_xbe_read_info(&src, info);
}

static void test_info(void)
{
    xc_xbe_info info;
    make("Game");
    put32(CERT_OFF + 0xA0, XC_XBE_REGION_NA | XC_XBE_REGION_JAPAN);
    put32(CERT_OFF + 0xA8, 2);
    put32(CERT_OFF + 0xAC, 0x7);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_STRING("Game", info.cert.title);
    TEST_ASSERT_TRUE(info.has_region);
    TEST_ASSERT_EQUAL_HEX32(3, info.region);
    TEST_ASSERT_EQUAL_UINT32(2, info.disc);
    TEST_ASSERT_EQUAL_UINT32(7, info.version);
    TEST_ASSERT_EQUAL_UINT32(0, info.image_size); /* no sections */

    /* A certificate too short for them: still an XBE, without them. */
    put32(CERT_OFF, 0xAC);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_FALSE(info.has_region);
    TEST_ASSERT_EQUAL_UINT32(0, info.disc);
    put32(CERT_OFF, 0x1D0);
    put32(0x108, CERT_OFF + 0xAF); /* the headers end inside the fields */
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_FALSE(info.has_region);
    put32(0x108, sizeof xbe);

    /* $$XSIMAGE, then $$XTIMAGE: the title image wins, wherever it is. */
    memcpy(xbe + 0x800, "$$XSIMAGE\0$$XTIMAGE\0.text\0$$XTIMAGEX", 37);
    put32(0x11C, 3);
    put32(0x120, BASE + 0x400);
    section(0, 0x1000, 0x100, 0x800);
    section(1, 0x2000, 0x800, 0x80A);
    section(2, 0x1000, 0x1000, 0x814);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0x2000, info.image_offset);
    TEST_ASSERT_EQUAL_UINT32(0x800, info.image_size);

    /* The title image's bytes past the file's end: the save image instead. */
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(0x27FF, &info));
    TEST_ASSERT_EQUAL_UINT32(0x1000, info.image_offset);
    TEST_ASSERT_EQUAL_UINT32(0x100, info.image_size);
    /* Too large, or too small to be an XPR0: ignored too. */
    section(1, 0x2000, XC_XBE_MAX_IMAGE + 1U, 0x80A);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0x1000, info.image_offset);
    section(1, 0x2000, 0x1F, 0x80A);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0x1000, info.image_offset);

    /* Names: "$$XTIMAGEX" isn't one; nor is a name before the base, or past the headers. */
    section(0, 0x1000, 0x100, 0x81A);
    section(1, 0x2000, 0x800, 0xFFA);
    put32(0x400 + 56 * 2 + 0x14, BASE - 4U);
    memcpy(xbe + 0xFFA, "$$XTIM", 6); /* runs into the end of the headers */
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.image_size);

    /* The section headers outside the headers, before the base, or too many. */
    section(1, 0x2000, 0x800, 0x80A);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0x800, info.image_size);
    put32(0x120, BASE + 0x1000 - 56 * 3 + 1);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.image_size);
    put32(0x120, BASE - 56);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.image_size);
    put32(0x120, BASE + 0x400);
    put32(0x11C, XC_XBE_MAX_SECTIONS + 1U);
    TEST_ASSERT_EQUAL_INT(XC_OK, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.image_size);

    /* Not an XBE: as xc_xbe_read_cert, everything zero. */
    xbe[0] = 'Y';
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, read_info(sizeof big, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.cert.title_id);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid);
    RUN_TEST(test_invalid);
    RUN_TEST(test_info);
    return UNITY_END();
}
