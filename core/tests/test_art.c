/* Tests for where a game's art is looked for (xcat/art.h, docs/formats/art.md §2, §6). */
#include "unity.h"
#include "xcat/art.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_base_name(void)
{
    char b[32];
    xc_art_base_name("Halo (USA).iso", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Halo (USA)", b);
    xc_art_base_name("Halo.XISO.ISO", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Halo", b);
    xc_art_base_name("Halo.xiso", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Halo", b);
    xc_art_base_name("Fable.7z", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Fable", b);
    xc_art_base_name("Fable.v1.zip", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Fable.v1", b);
    xc_art_base_name("Game.rar", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Game", b);
    xc_art_base_name(".iso", b, sizeof b); /* nothing before it: kept */
    TEST_ASSERT_EQUAL_STRING(".iso", b);
    xc_art_base_name("NoExtension", b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("NoExtension", b);
    xc_art_base_name("A long name for a small buffer.iso", b, 8);
    TEST_ASSERT_EQUAL_STRING("A long ", b);
}

static void test_plans(void)
{
    xc_art_plan p;
    /* A game folder: its own files, its synopsis, its default.xbe. */
    xc_art_plan_for(NULL, false, &p);
    TEST_ASSERT_EQUAL_size_t(6, p.cover_count);
    TEST_ASSERT_EQUAL_STRING("_resources/artwork/poster.jpg", p.covers[0]);
    TEST_ASSERT_EQUAL_STRING("default.tbn", p.covers[1]);
    TEST_ASSERT_EQUAL_STRING("folder.png", p.covers[5]);
    TEST_ASSERT_EQUAL_STRING("_resources/default.xml", p.synopsis);
    TEST_ASSERT_EQUAL_STRING("default.xbe", p.xbe);

    /* An image among others: only the pictures named after it. */
    xc_art_plan_for("Halo (USA).iso", false, &p);
    TEST_ASSERT_EQUAL_size_t(3, p.cover_count);
    TEST_ASSERT_EQUAL_STRING("Halo (USA).jpg", p.covers[0]);
    TEST_ASSERT_EQUAL_STRING("Halo (USA).png", p.covers[1]);
    TEST_ASSERT_EQUAL_STRING("Halo (USA).tbn", p.covers[2]);
    TEST_ASSERT_EQUAL_STRING("", p.synopsis);
    TEST_ASSERT_EQUAL_STRING("", p.xbe);

    /* An image in a folder of its own: its pictures first, then the folder's. */
    xc_art_plan_for("Halo (USA).7z", true, &p);
    TEST_ASSERT_EQUAL_size_t(9, p.cover_count);
    TEST_ASSERT_EQUAL_STRING("Halo (USA).jpg", p.covers[0]);
    TEST_ASSERT_EQUAL_STRING("_resources/artwork/poster.jpg", p.covers[3]);
    TEST_ASSERT_EQUAL_STRING("_resources/default.xml", p.synopsis);
    TEST_ASSERT_EQUAL_STRING("", p.xbe);

    /* A name too long for a path: those pictures are left out, not cut. */
    char longname[XC_ART_MAX_PATH + 8];
    memset(longname, 'a', sizeof longname - 5);
    strcpy(longname + sizeof longname - 5, ".iso");
    xc_art_plan_for(longname, true, &p);
    TEST_ASSERT_EQUAL_size_t(6, p.cover_count);
    TEST_ASSERT_EQUAL_STRING("_resources/artwork/poster.jpg", p.covers[0]);
}

static void test_region(void)
{
    TEST_ASSERT_EQUAL_STRING("NTSC-U", xc_art_region(XC_XBE_REGION_NA));
    TEST_ASSERT_EQUAL_STRING("NTSC-J", xc_art_region(XC_XBE_REGION_JAPAN));
    TEST_ASSERT_EQUAL_STRING("PAL", xc_art_region(XC_XBE_REGION_WORLD | XC_XBE_REGION_DEBUG));
    TEST_ASSERT_EQUAL_STRING("NTSC-U/J", xc_art_region(3));
    TEST_ASSERT_EQUAL_STRING("NTSC-U, PAL", xc_art_region(5));
    TEST_ASSERT_EQUAL_STRING("NTSC-J, PAL", xc_art_region(6));
    TEST_ASSERT_EQUAL_STRING("Region free", xc_art_region(7));
    TEST_ASSERT_EQUAL_STRING("", xc_art_region(0));
    TEST_ASSERT_EQUAL_STRING("", xc_art_region(XC_XBE_REGION_DEBUG));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_base_name);
    RUN_TEST(test_plans);
    RUN_TEST(test_region);
    return UNITY_END();
}
