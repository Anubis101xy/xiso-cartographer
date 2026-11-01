/*
 * A game or app folder in a zip (xcat/zip.h, docs/formats/folder-image.md §7): its members,
 * read as the install asks, through miniz's tinfl for Deflate (as zip_read.c does).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "xcat/arc_folder.h"
#include "xcat/crc32.h"
#include "xcat/zip.h"

#define DICT XC_ZIP_DICT_SIZE
#define IN_SIZE ((size_t)64 * 1024)

struct xc_zip_folder {
    const xc_source *src;
    const xc_zip_listing *zl;
    xc_arc_folder *arc;
    /* the member being read */
    size_t cur;      /* SIZE_MAX: none */
    xc_zip_member m; /* its data's place, from its local header */
    uint64_t pos;    /* uncompressed bytes passed (delivered or skipped) */
    uint64_t cpos;   /* compressed bytes given to tinfl */
    uint32_t crc;    /* of [0, pos), when crc_from_start */
    bool crc_from_start;
    bool done; /* tinfl finished the member */
    /* inflated bytes not yet passed: dict[pend_ofs, pend_ofs + pend_len) */
    size_t dict_ofs;
    size_t pend_ofs;
    size_t pend_len;
    size_t in_at;
    size_t in_len;
    tinfl_decompressor inf;
    uint8_t dict[DICT];
    uint8_t in[IN_SIZE];
};

static xc_err start_member(xc_zip_folder *z, size_t member)
{
    xc_err err = xc_zip_open_member(z->src, z->zl, member, &z->m);
    if (err != XC_OK) {
        z->cur = SIZE_MAX;
        return err;
    }
    z->cur = member;
    z->pos = 0;
    z->cpos = 0;
    z->crc = 0;
    z->crc_from_start = true;
    z->done = false;
    z->dict_ofs = 0;
    z->pend_len = 0;
    z->in_at = 0;
    z->in_len = 0;
    tinfl_init(&z->inf);
    /* As zip_read.c: distances before the first byte read zeros; the CRC-32 catches them. */
    memset(z->dict, 0, sizeof z->dict);
    return XC_OK;
}

/* The member is read to its end: its CRC-32 must be the listing's (zip.md §9). */
static xc_err check_end(xc_zip_folder *z)
{
    if (z->pos == z->m.size && z->crc_from_start && z->crc != z->m.crc32) {
        return XC_ERR_ZIP_CRC;
    }
    return XC_OK;
}

static xc_err read_stored(xc_zip_folder *z, uint64_t offset, uint8_t *buf, size_t len)
{
    xc_err err = xc_source_read(z->src, z->m.data_offset + offset, buf, len);
    if (err != XC_OK) {
        return err;
    }
    if (offset != z->pos) {
        z->crc_from_start = false; /* resumed in the middle: the CRC can't be checked */
    } else {
        z->crc = xc_crc32(z->crc, buf, len);
    }
    z->pos = offset + len;
    return check_end(z);
}

/* More inflated bytes into the pending window. */
static xc_err inflate_more(xc_zip_folder *z)
{
    for (;;) {
        if (z->done) {
            return XC_ERR_CORRUPT; /* the member's data ended before its size */
        }
        if (z->in_at == z->in_len && z->cpos < z->m.compressed_size) {
            uint64_t left = z->m.compressed_size - z->cpos;
            size_t n = left < IN_SIZE ? (size_t)left : IN_SIZE;
            xc_err err = xc_source_read(z->src, z->m.data_offset + z->cpos, z->in, n);
            if (err != XC_OK) {
                return err;
            }
            z->in_at = 0;
            z->in_len = n;
        }
        size_t in_sz = z->in_len - z->in_at;
        size_t out_sz = DICT - z->dict_ofs;
        bool more = z->cpos + in_sz < z->m.compressed_size;
        tinfl_status st =
            tinfl_decompress(&z->inf, z->in + z->in_at, &in_sz, z->dict, z->dict + z->dict_ofs,
                             &out_sz, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        z->in_at += in_sz;
        z->cpos += in_sz;
        if (st < TINFL_STATUS_DONE) {
            return XC_ERR_CORRUPT;
        }
        if (st == TINFL_STATUS_DONE) {
            z->done = true;
        }
        if (out_sz > 0) {
            z->pend_ofs = z->dict_ofs;
            z->pend_len = out_sz;
            z->dict_ofs = (z->dict_ofs + out_sz) & (DICT - 1U);
            return XC_OK;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && z->in_at == z->in_len &&
            z->cpos == z->m.compressed_size) {
            return XC_ERR_CORRUPT; /* it wants more than the member has */
        }
    }
}

static xc_err read_deflate(xc_zip_folder *z, uint64_t offset, uint8_t *buf, size_t len)
{
    uint64_t end = offset + len;
    while (z->pos < end) {
        if (z->pend_len == 0) {
            xc_err err = inflate_more(z);
            if (err != XC_OK) {
                return err;
            }
        }
        const uint8_t *p = z->dict + z->pend_ofs;
        size_t n = z->pend_len;
        if (z->pos < offset) {
            uint64_t skip = offset - z->pos;
            n = skip < n ? (size_t)skip : n; /* before the read: passed, not copied */
        } else {
            uint64_t want = end - z->pos;
            n = want < n ? (size_t)want : n;
            memcpy(buf + (z->pos - offset), p, n);
        }
        z->crc = xc_crc32(z->crc, p, n);
        z->pos += n;
        z->pend_ofs += n;
        z->pend_len -= n;
    }
    return check_end(z);
}

static xc_err member_read(void *ctx, size_t member, uint64_t offset, void *buf, size_t len)
{
    xc_zip_folder *z = ctx;
    if (member >= z->zl->count || offset > z->zl->entries[member].size ||
        len > z->zl->entries[member].size - offset) {
        return XC_ERR_RANGE;
    }
    /* Another member, or back in this one: from its first byte again (Deflate can't seek). */
    bool restart = member != z->cur || (z->m.method != XC_ZIP_STORED && offset < z->pos);
    if (restart) {
        xc_err err = start_member(z, member);
        if (err != XC_OK) {
            return err;
        }
    }
    if (len == 0) {
        return XC_OK;
    }
    return z->m.method == XC_ZIP_STORED ? read_stored(z, offset, buf, len)
                                        : read_deflate(z, offset, buf, len);
}

xc_err xc_zip_folder_open(const xc_source *src, const xc_zip_listing *listing, xc_zip_folder **out,
                          xc_source *image, char *why, size_t why_cap)
{
    if (why != NULL && why_cap > 0) {
        why[0] = '\0';
    }
    if (src == NULL || listing == NULL || out == NULL || image == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    xc_arc_member *members = malloc((listing->count + 1U) * sizeof *members);
    xc_zip_folder *z = calloc(1, sizeof *z);
    if (members == NULL || z == NULL) {
        free(members);
        free(z);
        return XC_ERR_NOMEM;
    }
    for (size_t i = 0; i < listing->count; i++) {
        const xc_zip_entry *e = &listing->entries[i];
        size_t n = strlen(e->name);
        bool dir = n > 0 && (e->name[n - 1U] == '/' || e->name[n - 1U] == '\\');
        /* Data order is the local headers' order; the CRC-32 stands for its content (§5). */
        members[i] = (xc_arc_member){e->name, e->size, dir, e->local_offset, e->crc32};
    }
    z->src = src;
    z->zl = listing;
    z->cur = SIZE_MAX;
    xc_err err =
        xc_arc_folder_build(members, listing->count, member_read, z, &z->arc, image, why, why_cap);
    free(members);
    /* Every member the image reads must be one this reader can: stored or Deflate, not
       encrypted (zip.md §6). Checked now, not half way through the install. */
    for (size_t i = 0; err == XC_OK && i < listing->count; i++) {
        const xc_zip_entry *e = &listing->entries[i];
        size_t n = strlen(e->name);
        bool dir = n > 0 && (e->name[n - 1U] == '/' || e->name[n - 1U] == '\\');
        if (dir || e->size == 0 || !xc_arc_folder_uses(z->arc, i)) {
            continue;
        }
        if ((e->flags & 1U) != 0 || (e->method != XC_ZIP_STORED && e->method != XC_ZIP_DEFLATE)) {
            const char *how = (e->flags & 1U) != 0 ? "encrypted" : xc_zip_method_name(e->method);
            if (why != NULL && why_cap > 0) {
                int w = snprintf(why, why_cap, "%s: %s (only stored and Deflate can be read)",
                                 e->name, how != NULL ? how : "an unknown method");
                if (w < 0) {
                    why[0] = '\0';
                }
            }
            err = XC_ERR_ZIP_UNSUPPORTED;
        }
    }
    if (err != XC_OK) {
        xc_zip_folder_close(z);
        memset(image, 0, sizeof *image);
        return err;
    }
    *out = z;
    return XC_OK;
}

void xc_zip_folder_close(xc_zip_folder *z)
{
    if (z == NULL) {
        return;
    }
    xc_arc_folder_free(z->arc);
    free(z);
}

const struct xc_arc_folder *xc_zip_folder_arc(const xc_zip_folder *z)
{
    return z != NULL ? z->arc : NULL;
}
