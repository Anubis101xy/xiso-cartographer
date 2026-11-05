/* NetBIOS node status request and answer (RFC 1002 §4.2.17-4.2.18; smb.md §5). */
#include "xcat/netbios.h"

#include <string.h>

void xc_nbstat_query(uint16_t id, uint8_t out[XC_NBSTAT_QUERY_LEN])
{
    memset(out, 0, XC_NBSTAT_QUERY_LEN);
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    /* Flags 0: a query, no recursion. One question. */
    out[5] = 1;
    /* The name "*" padded with NULs to 16 bytes, first-level encoded (RFC 1001 §14.1): each
       nibble as 'A' + nibble, 32 bytes after a length byte, then the empty label. */
    out[12] = 0x20;
    for (int i = 0; i < 16; i++) {
        uint8_t c = i == 0 ? (uint8_t)'*' : 0;
        out[13 + 2 * i] = (uint8_t)('A' + (c >> 4));
        out[14 + 2 * i] = (uint8_t)('A' + (c & 0x0FU));
    }
    out[45] = 0;
    out[47] = 0x21; /* NBSTAT */
    out[49] = 0x01; /* IN */
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* One NetBIOS name (15 bytes) as printable ASCII, trailing spaces trimmed. */
static void put_name(const uint8_t *p, char out[16])
{
    size_t n = 15;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) {
        n--;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = p[i] >= 0x20U && p[i] < 0x7FU ? (char)p[i] : '?';
    }
    out[n] = '\0';
}

xc_err xc_nbstat_parse(const uint8_t *data, size_t len, uint16_t id, xc_nbstat *out)
{
    if (data == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (len < 12 || be16(data) != id) {
        return XC_ERR_CORRUPT;
    }
    uint16_t flags = be16(data + 2);
    /* A response (R), opcode 0 (query), no error. */
    if ((flags & 0x8000U) == 0 || ((flags >> 11) & 0x0FU) != 0 || (flags & 0x000FU) != 0 ||
        be16(data + 6) == 0) {
        return XC_ERR_CORRUPT;
    }
    size_t at = 12;
    /* Skip any questions a server repeats, then the answer's name: labels, or a pointer. */
    for (uint16_t q = be16(data + 4); q > 0; q--) {
        while (at < len && data[at] != 0 && (data[at] & 0xC0U) == 0) {
            at += 1U + data[at];
        }
        at += at < len && (data[at] & 0xC0U) == 0xC0U ? 2U : 1U;
        at += 4; /* type, class */
    }
    while (at < len && data[at] != 0 && (data[at] & 0xC0U) == 0) {
        at += 1U + data[at];
    }
    if (at >= len) {
        return XC_ERR_CORRUPT;
    }
    at += (data[at] & 0xC0U) == 0xC0U ? 2U : 1U;
    /* Type NBSTAT, class IN, TTL, RDLENGTH. */
    if (at + 10 > len || be16(data + at) != 0x21 || be16(data + at + 2) != 0x01) {
        return XC_ERR_CORRUPT;
    }
    size_t rdlen = be16(data + at + 8);
    at += 10;
    if (rdlen == 0 || at + rdlen > len) {
        return XC_ERR_CORRUPT;
    }
    const uint8_t *rd = data + at;
    size_t names = rd[0];
    if (1 + names * 18 > rdlen) {
        return XC_ERR_CORRUPT;
    }
    const uint8_t *fallback = NULL;
    for (size_t i = 0; i < names; i++) {
        const uint8_t *e = rd + 1 + i * 18;
        bool group = (e[16] & 0x80U) != 0;
        uint8_t suffix = e[15];
        if (!group && suffix == 0x20 && out->name[0] == '\0') {
            put_name(e, out->name);
        } else if (!group && suffix == 0x00 && fallback == NULL) {
            fallback = e;
        } else if (group && suffix == 0x00 && out->group[0] == '\0') {
            put_name(e, out->group);
        }
    }
    if (out->name[0] == '\0' && fallback != NULL) {
        put_name(fallback, out->name);
    }
    size_t stats = 1 + names * 18;
    if (stats + 6 <= rdlen) {
        memcpy(out->mac, rd + stats, 6);
        out->has_mac = true;
    }
    return XC_OK;
}
