/* Tests for NetBIOS node status (xcat/netbios.h, RFC 1002 §4.2.17-4.2.18, smb.md §5). */
#include "unity.h"
#include "xcat/netbios.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_query(void)
{
    uint8_t q[XC_NBSTAT_QUERY_LEN];
    xc_nbstat_query(0x1234, q);
    static const uint8_t head[] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x01, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x20};
    TEST_ASSERT_EQUAL_MEMORY(head, q, sizeof head);
    /* "*" and 15 NULs, first-level encoded (as nbtstat -A and Impacket send it). */
    TEST_ASSERT_EQUAL_MEMORY("CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", q + 13, 32);
    static const uint8_t tail[] = {0x00, 0x00, 0x21, 0x00, 0x01};
    TEST_ASSERT_EQUAL_MEMORY(tail, q + 45, sizeof tail);
}

/* A node status answer the way Samba's nmbd sends one: the name repeated, three names, then
   the statistics with the MAC first. */
static size_t answer(uint8_t *out, uint16_t id, const char names[][16], const uint8_t *suffix,
                     const uint16_t *flags, int count, bool compressed, size_t stats_len)
{
    size_t n = 0;
    out[n++] = (uint8_t)(id >> 8);
    out[n++] = (uint8_t)id;
    out[n++] = 0x84; /* response, authoritative */
    out[n++] = 0x00;
    out[n++] = 0;
    out[n++] = 0; /* no questions */
    out[n++] = 0;
    out[n++] = 1; /* one answer */
    memset(out + n, 0, 4);
    n += 4;
    if (compressed) {
        out[n++] = 0xC0;
        out[n++] = 0x0C;
    } else {
        out[n++] = 0x20;
        memcpy(out + n, "CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 32);
        n += 32;
        out[n++] = 0;
    }
    out[n++] = 0;
    out[n++] = 0x21;
    out[n++] = 0;
    out[n++] = 0x01;
    memset(out + n, 0, 4); /* TTL */
    n += 4;
    size_t rdlen = 1 + (size_t)count * 18 + stats_len;
    out[n++] = (uint8_t)(rdlen >> 8);
    out[n++] = (uint8_t)rdlen;
    out[n++] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        memset(out + n, ' ', 15);
        memcpy(out + n, names[i], strlen(names[i]));
        out[n + 15] = suffix[i];
        out[n + 16] = (uint8_t)(flags[i] >> 8);
        out[n + 17] = (uint8_t)flags[i];
        n += 18;
    }
    static const uint8_t mac[6] = {0x6C, 0xBF, 0xB5, 0x01, 0x02, 0x03};
    memset(out + n, 0, stats_len);
    memcpy(out + n, mac, stats_len < 6 ? stats_len : 6);
    n += stats_len;
    return n;
}

static void test_samba_answer(void)
{
    static const char names[][16] = {"TNAS-F4", "TNAS-F4", "TNAS-F4", "WORKGROUP", "WORKGROUP"};
    static const uint8_t suffix[] = {0x00, 0x03, 0x20, 0x00, 0x1E};
    static const uint16_t flags[] = {0x0400, 0x0400, 0x0400, 0x8400, 0x8400};
    uint8_t a[400];
    size_t n = answer(a, 0x4242, names, suffix, flags, 5, false, 46);
    xc_nbstat st;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_nbstat_parse(a, n, 0x4242, &st));
    TEST_ASSERT_EQUAL_STRING("TNAS-F4", st.name);
    TEST_ASSERT_EQUAL_STRING("WORKGROUP", st.group);
    TEST_ASSERT_TRUE(st.has_mac);
    TEST_ASSERT_EQUAL_HEX8(0x6C, st.mac[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, st.mac[5]);

    /* Another transaction's answer, or a cut one, isn't taken. */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(a, n, 0x4243, &st));
    for (size_t cut = 0; cut < n - 46; cut++) {
        TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(a, cut, 0x4242, &st));
    }
}

static void test_name_choice(void)
{
    /* No <20>: the unique <00> name. Compressed answer name. No statistics. */
    static const char names[][16] = {"GROUPY", "MYPC"};
    static const uint8_t suffix[] = {0x00, 0x00};
    static const uint16_t flags[] = {0x8400, 0x0400};
    uint8_t a[200];
    size_t n = answer(a, 7, names, suffix, flags, 2, true, 0);
    xc_nbstat st;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_nbstat_parse(a, n, 7, &st));
    TEST_ASSERT_EQUAL_STRING("MYPC", st.name);
    TEST_ASSERT_EQUAL_STRING("GROUPY", st.group);
    TEST_ASSERT_FALSE(st.has_mac);

    /* Unprintable bytes become '?'. */
    static const char odd[][16] = {"A\x01"
                                   "B\xE9"};
    static const uint8_t s20[] = {0x20};
    static const uint16_t f0[] = {0x0400};
    n = answer(a, 8, odd, s20, f0, 1, false, 6);
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_nbstat_parse(a, n, 8, &st));
    TEST_ASSERT_EQUAL_STRING("A?B?", st.name);
    TEST_ASSERT_EQUAL_STRING("", st.group);
}

static void test_not_answers(void)
{
    uint8_t q[XC_NBSTAT_QUERY_LEN];
    xc_nbstat_query(9, q);
    xc_nbstat st;
    /* Our own query isn't an answer (no R bit). */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(q, sizeof q, 9, &st));
    static const char names[][16] = {"X"};
    static const uint8_t suffix[] = {0x20};
    static const uint16_t flags[] = {0x0400};
    uint8_t a[200];
    size_t n = answer(a, 9, names, suffix, flags, 1, false, 6);
    a[3] = 0x03; /* RCODE: name error */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(a, n, 9, &st));
    a[3] = 0x00;
    /* Header 12 bytes, name 34: the type is at 46-47, the name count at 56. */
    a[47] = 0x20; /* not NBSTAT: NB */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(a, n, 9, &st));
    a[47] = 0x21;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_nbstat_parse(a, n, 9, &st));
    a[56] = 200; /* more names than the data holds */
    TEST_ASSERT_EQUAL_INT(XC_ERR_CORRUPT, xc_nbstat_parse(a, n, 9, &st));
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_nbstat_parse(NULL, 0, 9, &st));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_query);
    RUN_TEST(test_samba_answer);
    RUN_TEST(test_name_choice);
    RUN_TEST(test_not_answers);
    return UNITY_END();
}
