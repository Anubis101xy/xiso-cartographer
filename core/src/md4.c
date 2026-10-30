/* MD4 (RFC 1320 §3), one-shot. Only the NT hash of an SMB password uses it (smb.md §4). */
#include "md4.h"

#include <string.h>

static uint32_t rotl(uint32_t x, unsigned s)
{
    return (x << s) | (x >> (32U - s));
}

static uint32_t f(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (~x & z);
}

static uint32_t g(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (x & z) | (y & z);
}

static uint32_t h(uint32_t x, uint32_t y, uint32_t z)
{
    return x ^ y ^ z;
}

/* §3.4: one 64-byte block. */
static void block(uint32_t st[4], const uint8_t *p)
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 |
               (uint32_t)p[4 * i + 3] << 24;
    }
    uint32_t v[4] = {st[0], st[1], st[2], st[3]};
    static const unsigned s1[4] = {3, 7, 11, 19};
    static const unsigned s2[4] = {3, 5, 9, 13};
    static const unsigned s3[4] = {3, 9, 11, 15};
    static const int k2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
    static const int k3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    /* Each step updates a, d, c, b in turn: v[(4 - i) % 4]. */
    for (int i = 0; i < 16; i++) {
        uint32_t *a = &v[(4 - i % 4) % 4];
        uint32_t b = v[(5 - i % 4) % 4], c = v[(6 - i % 4) % 4], d = v[(7 - i % 4) % 4];
        *a = rotl(*a + f(b, c, d) + x[i], s1[i % 4]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t *a = &v[(4 - i % 4) % 4];
        uint32_t b = v[(5 - i % 4) % 4], c = v[(6 - i % 4) % 4], d = v[(7 - i % 4) % 4];
        *a = rotl(*a + g(b, c, d) + x[k2[i]] + 0x5A827999U, s2[i % 4]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t *a = &v[(4 - i % 4) % 4];
        uint32_t b = v[(5 - i % 4) % 4], c = v[(6 - i % 4) % 4], d = v[(7 - i % 4) % 4];
        *a = rotl(*a + h(b, c, d) + x[k3[i]] + 0x6ED9EBA1U, s3[i % 4]);
    }
    for (int i = 0; i < 4; i++) {
        st[i] += v[i];
    }
}

void xc_md4(const void *data, size_t len, uint8_t out[XC_MD4_BYTES])
{
    uint32_t st[4] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U};
    const uint8_t *p = data;
    size_t left = len;
    while (left >= 64) {
        block(st, p);
        p += 64;
        left -= 64;
    }
    /* §3.1-3.2: 0x80, zeros to 56 mod 64, then the length in bits, little-endian. */
    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    if (left > 0) {
        memcpy(tail, p, left);
    }
    tail[left] = 0x80;
    size_t n = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8U;
    for (int i = 0; i < 8; i++) {
        tail[n - 8 + (size_t)i] = (uint8_t)(bits >> (8 * i));
    }
    block(st, tail);
    if (n == 128) {
        block(st, tail + 64);
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[4 * i + j] = (uint8_t)(st[i] >> (8 * j));
        }
    }
}
