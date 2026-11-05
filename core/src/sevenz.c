/*
 * 7z archives (xcat/sevenz.h). Implements docs/formats/sevenz.md; "§n" below refers to it.
 * The database is read by the LZMA SDK's 7zArcIn.c (third_party/lzma); each solid block
 * ("folder") is then decoded here a buffer at a time, through LzmaDec, Lzma2Dec and the x86
 * branch converter, so nothing larger than a dictionary is ever in memory.
 */
#include "xcat/sevenz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zCrc.h"
#include "Bra.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"
#include "xcat/crc32.h"
#include "xcat/zip.h"

/* --- the SDK's CRC-32, over ours (third_party/lzma/README.md) ---------------------------- */

void Z7_FASTCALL CrcGenerateTable(void) {}

UInt32 Z7_FASTCALL CrcUpdate(UInt32 v, const void *data, size_t size)
{
    return ~xc_crc32(~v, data, size); /* v is the running register, not a finished CRC */
}

UInt32 Z7_FASTCALL CrcCalc(const void *data, size_t size)
{
    return xc_crc32(0, data, size);
}

/* --- §4: methods -------------------------------------------------------------------------- */

#define M_COPY 0x00U
#define M_LZMA 0x030101U
#define M_LZMA2 0x21U
#define M_BCJ 0x03030103U
#define M_BCJ2 0x0303011BU
#define M_PPMD 0x030401U
#define M_AES 0x06F10701U
#define M_DEFLATE 0x040108U
#define M_BZIP2 0x040202U
#define M_DELTA 0x03U

#define IN_BUF ((size_t)64 * 1024)
#define OUT_BUF ((size_t)256 * 1024)
#define LOOK_BUF ((size_t)64 * 1024)
#define DIC_MIN 4096U

static const char *method_name(UInt32 id)
{
    switch (id) {
    case M_COPY:
        return "Copy";
    case M_LZMA:
        return "LZMA";
    case M_LZMA2:
        return "LZMA2";
    case M_BCJ:
        return "BCJ";
    case M_BCJ2:
        return "BCJ2";
    case M_PPMD:
        return "PPMd";
    case M_AES:
        return "AES (a password)";
    case M_DEFLATE:
        return "Deflate";
    case M_BZIP2:
        return "BZip2";
    case M_DELTA:
        return "Delta";
    default:
        return "an unknown method";
    }
}

/* --- reading the archive through the SDK ---------------------------------------------------- */

typedef struct seek_stream {
    ISeekInStream vt; /* first: the SDK's pointer is ours */
    const xc_source *src;
    uint64_t pos;
    xc_err err;
} seek_stream;

static SRes ss_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
    seek_stream *s = (seek_stream *)(void *)pp;
    uint64_t left = s->pos < s->src->size ? s->src->size - s->pos : 0;
    size_t n = (uint64_t)*size < left ? *size : (size_t)left;
    if (n > 0) {
        xc_err err = xc_source_read(s->src, s->pos, buf, n);
        if (err != XC_OK) {
            s->err = err;
            *size = 0;
            return SZ_ERROR_READ;
        }
    }
    s->pos += n;
    *size = n;
    return SZ_OK;
}

static SRes ss_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    seek_stream *s = (seek_stream *)(void *)pp;
    int64_t base = origin == SZ_SEEK_SET   ? 0
                   : origin == SZ_SEEK_CUR ? (int64_t)s->pos
                                           : (int64_t)s->src->size;
    int64_t to = base + *pos;
    if (to < 0) {
        return SZ_ERROR_PARAM;
    }
    s->pos = (uint64_t)to;
    *pos = to;
    return SZ_OK;
}

/* The SDK's allocations, refused past a bound (§2), so a hostile database can't take every
   byte of the console's memory. */
typedef struct limited_alloc {
    ISzAlloc vt;
    size_t most;
    bool refused;
} limited_alloc;

static void *la_alloc(ISzAllocPtr p, size_t size)
{
    limited_alloc *a = (limited_alloc *)(void *)p;
    if (size > a->most) {
        a->refused = true;
        return NULL;
    }
    return malloc(size != 0 ? size : 1U);
}

static void la_free(ISzAllocPtr p, void *address)
{
    (void)p;
    free(address);
}

/* A decoder over one solid block, a buffer at a time (§5). */
typedef struct dec {
    bool open;
    UInt32 folder;
    UInt32 method; /* coder 0's: M_COPY, M_LZMA, M_LZMA2 */
    bool bcj;
    UInt32 bcj_state;
    UInt32 bcj_pc;
    uint64_t pack_at; /* the next packed byte's offset in the archive */
    uint64_t pack_left;
    uint64_t main_left; /* coder 0's output still to come */
    uint64_t pos;       /* the block's output bytes passed so far */
    CLzmaDec lz;
    CLzma2Dec lz2;
    bool probs;
    Byte *dic;
    Byte in[IN_BUF];
    size_t in_pos, in_len;
    Byte buf[OUT_BUF];
    size_t buf_len; /* decoded */
    size_t ready;   /* of those, final (past the branch converter) */
    size_t taken;   /* of those, passed */
} dec;

struct xc_7z {
    const xc_source *src;
    size_t max_dict;
    seek_stream ss;
    CLookToRead2 look;
    Byte *look_buf;
    limited_alloc alloc;
    CSzArEx db;
    bool db_open;
    char **paths; /* UTF-8, '/'-separated */
    dec *d;
    /* §5: the CRC-32 of the file being read through */
    size_t crc_file; /* SIZE_MAX: none */
    uint64_t crc_next, crc_end;
    uint32_t crc;
};

/* The decoders' probabilities: small, and bounded by the format. */
static void *plain_malloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return malloc(size != 0 ? size : 1U);
}

static const ISzAlloc plain_alloc = {plain_malloc, la_free};

static void say(char *why, size_t cap, const char *fmt, const char *a, const char *b)
{
    if (why != NULL && cap > 0) {
        int n = snprintf(why, cap, fmt, a, b);
        if (n < 0) {
            why[0] = '\0';
        }
    }
}

/* UTF-16LE to UTF-8, '\' as '/'; unpaired surrogates as '?'. */
static char *utf8_name(const UInt16 *w, size_t n)
{
    char *out = malloc(n * 3U + 1U);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && w[i] != 0; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800U && c <= 0xDBFFU && i + 1U < n && w[i + 1U] >= 0xDC00U &&
            w[i + 1U] <= 0xDFFFU) {
            c = 0x10000U + ((c - 0xD800U) << 10U) + (w[i + 1U] - 0xDC00U);
            i++;
        } else if (c >= 0xD800U && c <= 0xDFFFU) {
            c = '?';
        }
        if (c == '\\') {
            c = '/';
        }
        if (c < 0x80U) {
            out[o++] = (char)c;
        } else if (c < 0x800U) {
            out[o++] = (char)(0xC0U | (c >> 6));
            out[o++] = (char)(0x80U | (c & 0x3FU));
        } else if (c < 0x10000U) {
            out[o++] = (char)(0xE0U | (c >> 12));
            out[o++] = (char)(0x80U | ((c >> 6) & 0x3FU));
            out[o++] = (char)(0x80U | (c & 0x3FU));
        } else {
            out[o++] = (char)(0xF0U | (c >> 18));
            out[o++] = (char)(0x80U | ((c >> 12) & 0x3FU));
            out[o++] = (char)(0x80U | ((c >> 6) & 0x3FU));
            out[o++] = (char)(0x80U | (c & 0x3FU));
        }
    }
    out[o] = '\0';
    return out;
}

static xc_err from_sres(const xc_7z *a, SRes r)
{
    switch (r) {
    case SZ_OK:
        return XC_OK;
    case SZ_ERROR_MEM:
        return a->alloc.refused ? XC_ERR_LIMIT : XC_ERR_NOMEM;
    case SZ_ERROR_READ:
        return a->ss.err != XC_OK ? a->ss.err : XC_ERR_IO;
    case SZ_ERROR_UNSUPPORTED:
        return XC_ERR_7Z_UNSUPPORTED;
    default:
        return XC_ERR_NOT_7Z;
    }
}

xc_err xc_7z_open(const xc_source *src, size_t max_dict, xc_7z **out, char *why, size_t why_cap)
{
    say(why, why_cap, "%s%s", "", "");
    if (src == NULL || out == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *out = NULL;
    xc_7z *a = calloc(1, sizeof *a);
    if (a == NULL) {
        return XC_ERR_NOMEM;
    }
    a->src = src;
    a->max_dict = max_dict;
    a->crc_file = SIZE_MAX;
    a->ss.vt.Read = ss_read;
    a->ss.vt.Seek = ss_seek;
    a->ss.src = src;
    a->alloc.vt.Alloc = la_alloc;
    a->alloc.vt.Free = la_free;
    a->alloc.most = XC_7Z_MAX_HEADER;
    a->look_buf = malloc(LOOK_BUF);
    if (a->look_buf == NULL) {
        free(a);
        return XC_ERR_NOMEM;
    }
    LookToRead2_CreateVTable(&a->look, False);
    a->look.buf = a->look_buf;
    a->look.bufSize = LOOK_BUF;
    a->look.realStream = &a->ss.vt;
    LookToRead2_INIT(&a->look);
    SzArEx_Init(&a->db);
    SRes r = SzArEx_Open(&a->db, &a->look.vt, &a->alloc.vt, &a->alloc.vt);
    xc_err err = from_sres(a, r);
    a->db_open = true;
    if (err == XC_OK && a->db.NumFiles > XC_7Z_MAX_FILES) {
        err = XC_ERR_LIMIT;
    }
    if (err == XC_OK) {
        a->paths = calloc((size_t)a->db.NumFiles + 1U, sizeof *a->paths);
        err = a->paths != NULL ? XC_OK : XC_ERR_NOMEM;
    }
    UInt16 *wide = NULL;
    for (UInt32 i = 0; err == XC_OK && i < a->db.NumFiles; i++) {
        size_t n = SzArEx_GetFileNameUtf16(&a->db, i, NULL);
        if (n > XC_7Z_MAX_NAME) {
            err = XC_ERR_LIMIT;
            break;
        }
        UInt16 *w = realloc(wide, (n + 1U) * sizeof *w);
        if (w == NULL) {
            err = XC_ERR_NOMEM;
            break;
        }
        wide = w;
        (void)SzArEx_GetFileNameUtf16(&a->db, i, wide);
        a->paths[i] = utf8_name(wide, n);
        if (a->paths[i] == NULL) {
            err = XC_ERR_NOMEM;
        }
    }
    free(wide);
    if (err != XC_OK) {
        say(why, why_cap, "%s%s",
            err == XC_ERR_NOT_7Z           ? "no 7z signature, or a damaged database"
            : err == XC_ERR_7Z_UNSUPPORTED ? "its list of files is encrypted (a password)"
            : err == XC_ERR_LIMIT          ? "its list of files is too large"
                                           : "",
            "");
        xc_7z_close(a);
        return err;
    }
    *out = a;
    return XC_OK;
}

static void dec_free(xc_7z *a)
{
    dec *d = a->d;
    if (d == NULL) {
        return;
    }
    if (d->probs) {
        if (d->method == M_LZMA) {
            LzmaDec_FreeProbs(&d->lz, &plain_alloc);
        } else {
            Lzma2Dec_FreeProbs(&d->lz2, &plain_alloc);
        }
    }
    free(d->dic);
    d->dic = NULL;
    d->probs = false;
    d->open = false;
}

void xc_7z_close(xc_7z *a)
{
    if (a == NULL) {
        return;
    }
    dec_free(a);
    free(a->d);
    if (a->paths != NULL) {
        for (UInt32 i = 0; i < a->db.NumFiles; i++) {
            free(a->paths[i]);
        }
        free(a->paths);
    }
    if (a->db_open) {
        SzArEx_Free(&a->db, &a->alloc.vt);
    }
    free(a->look_buf);
    free(a);
}

size_t xc_7z_count(const xc_7z *a)
{
    return a != NULL ? a->db.NumFiles : 0;
}

const char *xc_7z_path(const xc_7z *a, size_t i)
{
    return a != NULL && i < a->db.NumFiles ? a->paths[i] : "";
}

uint64_t xc_7z_size(const xc_7z *a, size_t i)
{
    return a != NULL && i < a->db.NumFiles ? SzArEx_GetFileSize(&a->db, i) : 0;
}

bool xc_7z_is_dir(const xc_7z *a, size_t i)
{
    return a != NULL && i < a->db.NumFiles && SzArEx_IsDir(&a->db, i);
}

bool xc_7z_crc(const xc_7z *a, size_t i, uint32_t *crc)
{
    if (a == NULL || i >= a->db.NumFiles || !SzBitWithVals_Check(&a->db.CRCs, i)) {
        return false;
    }
    *crc = a->db.CRCs.Vals[i];
    return true;
}

bool xc_7z_is_7z_name(const char *path_or_url)
{
    if (path_or_url == NULL) {
        return false;
    }
    size_t n = strcspn(path_or_url, "?#");
    return n >= 3 && path_or_url[n - 3] == '.' && path_or_url[n - 2] == '7' &&
           (path_or_url[n - 1] | 0x20) == 'z';
}

xc_err xc_7z_pick_image(const xc_7z *a, size_t *index)
{
    size_t found = 0;
    for (size_t i = 0; a != NULL && i < a->db.NumFiles; i++) {
        if (!SzArEx_IsDir(&a->db, i) && xc_zip_is_image_name(a->paths[i])) {
            if (found == 0 && index != NULL) {
                *index = i;
            }
            found++;
        }
    }
    return found == 1 ? XC_OK : XC_ERR_ZIP_MEMBER;
}

/* --- §4: what a solid block needs ------------------------------------------------------------ */

typedef struct block {
    CSzFolder f;
    const Byte *props; /* the coders' data; a coder's properties at props + PropsOffset */
    UInt32 main;       /* coder 0's method */
    bool bcj;
    uint64_t main_size; /* coder 0's output */
    uint64_t size;      /* the block's output */
    uint64_t pack_at;
    uint64_t pack_size;
    uint64_t dict; /* the dictionary the decoder needs: never more than main_size */
} block;

static xc_err block_of(const xc_7z *a, UInt32 fi, block *b, char *why, size_t why_cap)
{
    const CSzAr *ar = &a->db.db;
    if (fi >= ar->NumFolders) {
        return XC_ERR_CORRUPT;
    }
    CSzData sd;
    sd.Data = ar->CodersData + ar->FoCodersOffsets[fi];
    sd.Size = ar->FoCodersOffsets[fi + 1] - ar->FoCodersOffsets[fi];
    memset(b, 0, sizeof *b);
    if (SzGetNextFolderItem(&b->f, &sd) != SZ_OK) {
        say(why, why_cap, "%s%s", "a solid block's coders can't be read", "");
        return XC_ERR_7Z_UNSUPPORTED;
    }
    b->props = ar->CodersData + ar->FoCodersOffsets[fi];
    const CSzFolder *f = &b->f;
    const UInt64 *unpack = ar->CoderUnpackSizes + ar->FoToCoderUnpackSizes[fi];
    bool shape1 =
        f->NumCoders == 1 && f->NumPackStreams == 1 && f->PackStreams[0] == 0 && f->NumBonds == 0;
    bool shape2 = f->NumCoders == 2 && f->Coders[1].NumStreams == 1 && f->NumPackStreams == 1 &&
                  f->PackStreams[0] == 0 && f->NumBonds == 1 && f->Bonds[0].InIndex == 1 &&
                  f->Bonds[0].OutIndex == 0 && f->Coders[1].MethodID == M_BCJ;
    UInt32 m0 = f->NumCoders > 0 ? f->Coders[0].MethodID : 0;
    bool main_ok = f->NumCoders > 0 && f->Coders[0].NumStreams == 1 &&
                   (m0 == M_COPY || m0 == M_LZMA || m0 == M_LZMA2);
    if (!(shape1 || shape2) || !main_ok) {
        /* Name what it uses: the first coder that isn't one we decode, else the shape. */
        const char *what = "a coder layout";
        for (UInt32 c = 0; c < f->NumCoders; c++) {
            UInt32 id = f->Coders[c].MethodID;
            if (id != M_COPY && id != M_LZMA && id != M_LZMA2 && id != M_BCJ) {
                what = method_name(id);
                break;
            }
        }
        say(why, why_cap, "it uses %s; this console decodes LZMA and LZMA2 (with BCJ), and %s",
            what, "uncompressed files");
        return XC_ERR_7Z_UNSUPPORTED;
    }
    b->main = m0;
    b->bcj = shape2;
    b->main_size = unpack[0];
    b->size = SzAr_GetFolderUnpackSize(ar, fi);
    UInt32 ps = ar->FoStartPackStreamIndex[fi];
    b->pack_at = a->db.dataPos + ar->PackPositions[ps];
    b->pack_size = ar->PackPositions[ps + 1] - ar->PackPositions[ps];
    const CSzCoderInfo *c = &f->Coders[0];
    const Byte *p = b->props + c->PropsOffset;
    uint64_t dict = 0;
    if (m0 == M_LZMA) {
        if (c->PropsSize < 5) {
            return XC_ERR_CORRUPT;
        }
        dict = (uint64_t)p[1] | (uint64_t)p[2] << 8 | (uint64_t)p[3] << 16 | (uint64_t)p[4] << 24;
    } else if (m0 == M_LZMA2) {
        if (c->PropsSize < 1 || p[0] > 40) {
            return XC_ERR_CORRUPT;
        }
        dict = p[0] == 40 ? 0xFFFFFFFFULL : (uint64_t)(2U | (p[0] & 1U)) << (p[0] / 2U + 11U);
    }
    /* Matches can't reach before the block's first byte: a block smaller than its dictionary
       needs only its own size (§4). */
    if (m0 != M_COPY) {
        dict = dict < b->main_size ? dict : b->main_size;
        dict = dict < DIC_MIN ? DIC_MIN : dict;
    }
    b->dict = dict;
    if (a->max_dict != 0 && dict > a->max_dict) {
        char need[48];
        char have[48];
        (void)snprintf(need, sizeof need, "%llu MiB",
                       (unsigned long long)((dict + 0xFFFFFU) >> 20));
        (void)snprintf(have, sizeof have, "%llu MiB", (unsigned long long)(a->max_dict >> 20));
        say(why, why_cap, "unpacking it needs a %s dictionary; this console has room for %s", need,
            have);
        return XC_ERR_7Z_MEMORY;
    }
    return XC_OK;
}

xc_err xc_7z_check(const xc_7z *a, size_t i, char *why, size_t why_cap)
{
    if (a == NULL || i >= a->db.NumFiles) {
        return XC_ERR_INVALID_ARG;
    }
    UInt32 fi = a->db.FileToFolder[i];
    if (fi == (UInt32)-1 || SzArEx_GetFileSize(&a->db, i) == 0) {
        return XC_OK; /* no data */
    }
    block b;
    return block_of(a, fi, &b, why, why_cap);
}

bool xc_7z_stored_at(const xc_7z *a, size_t i, uint64_t *offset)
{
    if (a == NULL || i >= a->db.NumFiles) {
        return false;
    }
    UInt32 fi = a->db.FileToFolder[i];
    block b;
    if (fi == (UInt32)-1 || block_of(a, fi, &b, NULL, 0) != XC_OK || b.main != M_COPY || b.bcj) {
        return false;
    }
    *offset =
        b.pack_at + (a->db.UnpackPositions[i] - a->db.UnpackPositions[a->db.FolderToFile[fi]]);
    return true;
}

uint64_t xc_7z_packed_size(const xc_7z *a, size_t i)
{
    if (a == NULL || i >= a->db.NumFiles || a->db.FileToFolder[i] == (UInt32)-1) {
        return 0;
    }
    block b;
    return block_of(a, a->db.FileToFolder[i], &b, NULL, 0) == XC_OK ? b.pack_size : 0;
}

/* --- §5: decoding a block a buffer at a time ---------------------------------------------- */

static xc_err dec_start(xc_7z *a, UInt32 fi)
{
    if (a->d == NULL) {
        a->d = calloc(1, sizeof *a->d);
        if (a->d == NULL) {
            return XC_ERR_NOMEM;
        }
    }
    dec_free(a);
    dec *d = a->d;
    block b;
    xc_err err = block_of(a, fi, &b, NULL, 0);
    if (err != XC_OK) {
        return err;
    }
    d->folder = fi;
    d->method = b.main;
    d->bcj = b.bcj;
    d->bcj_state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    d->bcj_pc = 0;
    d->pack_at = b.pack_at;
    d->pack_left = b.pack_size;
    d->main_left = b.main_size;
    d->pos = 0;
    d->in_pos = d->in_len = 0;
    d->buf_len = d->ready = d->taken = 0;
    if (b.main != M_COPY) {
        d->dic = malloc((size_t)b.dict);
        if (d->dic == NULL) {
            return XC_ERR_7Z_MEMORY; /* the check passed; the memory isn't there now */
        }
        const CSzCoderInfo *c = &b.f.Coders[0];
        const Byte *props = b.props + c->PropsOffset;
        SRes r;
        if (b.main == M_LZMA) {
            LzmaDec_CONSTRUCT(&d->lz);
            r = LzmaDec_AllocateProbs(&d->lz, props, c->PropsSize, &plain_alloc);
            if (r == SZ_OK) {
                d->lz.dic = d->dic;
                d->lz.dicBufSize = (SizeT)b.dict;
                LzmaDec_Init(&d->lz);
            }
        } else {
            Lzma2Dec_CONSTRUCT(&d->lz2);
            r = Lzma2Dec_AllocateProbs(&d->lz2, props[0], &plain_alloc);
            if (r == SZ_OK) {
                d->lz2.decoder.dic = d->dic;
                d->lz2.decoder.dicBufSize = (SizeT)b.dict;
                Lzma2Dec_Init(&d->lz2);
            }
        }
        if (r != SZ_OK) {
            free(d->dic);
            d->dic = NULL;
            return r == SZ_ERROR_MEM ? XC_ERR_7Z_MEMORY : XC_ERR_CORRUPT;
        }
        d->probs = true;
    }
    d->open = true;
    return XC_OK;
}

static xc_err refill(xc_7z *a)
{
    dec *d = a->d;
    if (d->pack_left == 0) {
        return XC_ERR_CORRUPT; /* the packed data ends before the block does */
    }
    size_t n = d->pack_left < IN_BUF ? (size_t)d->pack_left : IN_BUF;
    xc_err err = xc_source_read(a->src, d->pack_at, d->in, n);
    if (err != XC_OK) {
        return err;
    }
    d->pack_at += n;
    d->pack_left -= n;
    d->in_pos = 0;
    d->in_len = n;
    return XC_OK;
}

/* Coder 0's next bytes into buf[buf_len, ...). */
static xc_err decode_more(xc_7z *a)
{
    dec *d = a->d;
    size_t room = sizeof d->buf - d->buf_len;
    size_t want = d->main_left < room ? (size_t)d->main_left : room;
    while (want > 0) {
        if (d->method == M_COPY) {
            if (d->pack_left < want) {
                return XC_ERR_CORRUPT;
            }
            xc_err err = xc_source_read(a->src, d->pack_at, d->buf + d->buf_len, want);
            if (err != XC_OK) {
                return err;
            }
            d->pack_at += want;
            d->pack_left -= want;
            d->buf_len += want;
            d->main_left -= want;
            return XC_OK;
        }
        if (d->in_pos == d->in_len) {
            xc_err err = refill(a);
            if (err != XC_OK) {
                return err;
            }
        }
        SizeT out_len = want;
        SizeT in_len = d->in_len - d->in_pos;
        ELzmaStatus st;
        SRes r = d->method == M_LZMA
                     ? LzmaDec_DecodeToBuf(&d->lz, d->buf + d->buf_len, &out_len, d->in + d->in_pos,
                                           &in_len, LZMA_FINISH_ANY, &st)
                     : Lzma2Dec_DecodeToBuf(&d->lz2, d->buf + d->buf_len, &out_len,
                                            d->in + d->in_pos, &in_len, LZMA_FINISH_ANY, &st);
        if (r != SZ_OK) {
            return XC_ERR_CORRUPT;
        }
        d->in_pos += in_len;
        d->buf_len += out_len;
        d->main_left -= out_len;
        if (out_len > 0) {
            return XC_OK;
        }
        if (st == LZMA_STATUS_FINISHED_WITH_MARK) {
            return XC_ERR_CORRUPT; /* it ended before its size */
        }
        if (in_len == 0 && d->in_pos < d->in_len) {
            return XC_ERR_CORRUPT; /* no progress with input left */
        }
    }
    return XC_OK;
}

/* Makes some of the block's output ready to pass. */
static xc_err produce(xc_7z *a)
{
    dec *d = a->d;
    while (d->taken == d->ready) {
        size_t tail = d->buf_len - d->ready;
        memmove(d->buf, d->buf + d->ready, tail);
        d->buf_len = tail;
        d->ready = d->taken = 0;
        if (d->main_left == 0 && d->buf_len == 0) {
            return XC_ERR_CORRUPT; /* asked for more than the block has */
        }
        if (d->main_left > 0) {
            xc_err err = decode_more(a);
            if (err != XC_OK) {
                return err;
            }
        }
        if (!d->bcj) {
            d->ready = d->buf_len;
        } else {
            Byte *end = z7_BranchConvSt_X86_Dec(d->buf, d->buf_len, d->bcj_pc, &d->bcj_state);
            size_t done = (size_t)(end - d->buf);
            d->bcj_pc += (UInt32)done;
            /* At the block's end, the last bytes the converter keeps back stay as they are. */
            d->ready = d->main_left == 0 ? d->buf_len : done;
        }
    }
    return XC_OK;
}

/* Bytes [d->pos, d->pos + n) of the block pass: the file being read through counts them. */
static xc_err count_crc(xc_7z *a, const Byte *p, size_t n)
{
    if (a->crc_file == SIZE_MAX) {
        return XC_OK;
    }
    uint64_t lo = a->d->pos > a->crc_next ? a->d->pos : a->crc_next;
    uint64_t hi = a->d->pos + n < a->crc_end ? a->d->pos + n : a->crc_end;
    if (lo < hi && lo == a->crc_next) {
        a->crc = xc_crc32(a->crc, p + (lo - a->d->pos), (size_t)(hi - lo));
        a->crc_next = hi;
    }
    if (a->crc_next == a->crc_end) {
        uint32_t want = 0;
        size_t f = a->crc_file;
        a->crc_file = SIZE_MAX;
        if (xc_7z_crc(a, f, &want) && want != a->crc) {
            return XC_ERR_ZIP_CRC;
        }
    }
    return XC_OK;
}

/*
 * Passes the block's bytes up to `until`: those at or after `from` go into `out` (at their
 * distance from `from`) or to `fn` (with offsets from `base`).
 */
static xc_err pass(xc_7z *a, uint64_t until, uint64_t from, Byte *out, xc_source_data_fn fn,
                   void *fn_ctx, uint64_t base)
{
    dec *d = a->d;
    while (d->pos < until) {
        xc_err err = produce(a);
        if (err != XC_OK) {
            return err;
        }
        size_t n = d->ready - d->taken;
        if ((uint64_t)n > until - d->pos) {
            n = (size_t)(until - d->pos);
        }
        const Byte *p = d->buf + d->taken;
        err = count_crc(a, p, n);
        if (err != XC_OK) {
            return err;
        }
        if (d->pos + n > from) {
            size_t skip = d->pos < from ? (size_t)(from - d->pos) : 0;
            if (out != NULL) {
                memcpy(out + (d->pos + skip - from), p + skip, n - skip);
            } else {
                err = fn(fn_ctx, d->pos + skip - base, p + skip, n - skip);
                if (err != XC_OK) {
                    return err;
                }
            }
        }
        d->pos += n;
        d->taken += n;
    }
    return XC_OK;
}

/* Where file i is in its block: [*start, *end). */
static xc_err place_of(const xc_7z *a, size_t i, UInt32 *fi, uint64_t *start, uint64_t *end)
{
    *fi = a->db.FileToFolder[i];
    if (*fi == (UInt32)-1) {
        return XC_ERR_RANGE;
    }
    uint64_t first = a->db.UnpackPositions[a->db.FolderToFile[*fi]];
    *start = a->db.UnpackPositions[i] - first;
    *end = a->db.UnpackPositions[i + 1U] - first;
    return XC_OK;
}

/* The decoder at or before block position `target` of file i's block, from the block's
   start if it has to go back; the file's CRC-32 counted if it will be read through. */
static xc_err seek_to(xc_7z *a, size_t i, UInt32 fi, uint64_t start, uint64_t end, uint64_t target)
{
    dec *d = a->d;
    if (d == NULL || !d->open || d->folder != fi || d->pos > target) {
        xc_err err = dec_start(a, fi);
        if (err != XC_OK) {
            return err;
        }
        a->crc_file = SIZE_MAX;
        d = a->d;
    }
    if (a->crc_file != i && d->pos <= start) {
        a->crc_file = i;
        a->crc = 0;
        a->crc_next = start;
        a->crc_end = end;
    }
    return XC_OK;
}

xc_err xc_7z_read(xc_7z *a, size_t i, uint64_t offset, void *buf, size_t len)
{
    if (a == NULL || i >= a->db.NumFiles) {
        return XC_ERR_INVALID_ARG;
    }
    uint64_t size = SzArEx_GetFileSize(&a->db, i);
    if (offset > size || len > size - offset) {
        return XC_ERR_RANGE;
    }
    if (len == 0) {
        return XC_OK;
    }
    UInt32 fi;
    uint64_t start, end;
    xc_err err = place_of(a, i, &fi, &start, &end);
    if (err == XC_OK) {
        err = seek_to(a, i, fi, start, end, start + offset);
    }
    if (err == XC_OK) {
        err = pass(a, start + offset + len, start + offset, buf, NULL, NULL, 0);
    }
    if (err != XC_OK && a->d != NULL) {
        a->d->open = false; /* start again next time */
    }
    return err;
}

xc_err xc_7z_stream(xc_7z *a, size_t i, uint64_t from, xc_source_data_fn fn, void *fn_ctx)
{
    if (a == NULL || fn == NULL || i >= a->db.NumFiles) {
        return XC_ERR_INVALID_ARG;
    }
    uint64_t size = SzArEx_GetFileSize(&a->db, i);
    if (from >= size) {
        return from == size ? XC_OK : XC_ERR_RANGE;
    }
    UInt32 fi;
    uint64_t start, end;
    xc_err err = place_of(a, i, &fi, &start, &end);
    if (err == XC_OK && a->d != NULL) {
        a->d->open = false; /* a stream always decodes its block from the start */
    }
    if (err == XC_OK) {
        err = seek_to(a, i, fi, start, end, start + from);
    }
    if (err == XC_OK) {
        err = pass(a, end, start + from, NULL, fn, fn_ctx, start);
    }
    if (a->d != NULL) {
        a->d->open = false;
    }
    return err;
}

/* --- folder-image.md §7 ----------------------------------------------------------------- */

static xc_err folder_member_read(void *ctx, size_t member, uint64_t offset, void *buf, size_t len)
{
    return xc_7z_read(ctx, member, offset, buf, len);
}

xc_err xc_7z_folder_open(xc_7z *a, xc_arc_folder **out, xc_source *image, char *why, size_t why_cap)
{
    if (a == NULL || out == NULL || image == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    size_t n = a->db.NumFiles;
    xc_arc_member *members = malloc((n + 1U) * sizeof *members);
    if (members == NULL) {
        return XC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t crc = 0;
        bool has_crc = xc_7z_crc(a, i, &crc);
        /* The archive's order (UnpackPositions grow through the blocks, in order), so each
           block is decoded once from its start to its end (§5). */
        members[i] = (xc_arc_member){a->paths[i], SzArEx_GetFileSize(&a->db, i),
                                     SzArEx_IsDir(&a->db, i) != 0, a->db.UnpackPositions[i],
                                     has_crc ? crc : (uint64_t)i};
    }
    xc_err err = xc_arc_folder_build(members, n, folder_member_read, a, out, image, why, why_cap);
    free(members);
    /* Every file the image reads must be one this console can decode: said now, not half
       way through the install. */
    for (size_t i = 0; err == XC_OK && i < n; i++) {
        if (!xc_arc_folder_uses(*out, i)) {
            continue;
        }
        char what[160];
        err = xc_7z_check(a, i, what, sizeof what);
        if (err != XC_OK) {
            say(why, why_cap, "%s: %s", a->paths[i], what);
        }
    }
    if (err != XC_OK && *out != NULL) {
        xc_arc_folder_free(*out);
        *out = NULL;
        memset(image, 0, sizeof *image);
    }
    return err;
}
