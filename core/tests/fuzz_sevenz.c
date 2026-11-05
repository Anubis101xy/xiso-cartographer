/*
 * libFuzzer target for 7z archives (docs/formats/sevenz.md): the database (the LZMA SDK's
 * reader under our allocation bound), the decoders, and the game folder built from it
 * (folder-image.md §7).
 *
 * The input is an archive. Opening must give XC_OK or one of the errors sevenz.md names;
 * every file is then checked, and at most 256 KiB of each is read; a game folder, if there is
 * one, must list with the XDVDFS reader. Work is bounded: a 1 MiB dictionary at most. A
 * crash, leak or violation aborts. Seeds are core/tests/data/7z's archives.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/sevenz.h"
#include "xcat/xdvdfs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static bool known(xc_err e)
{
    return e == XC_OK || e == XC_ERR_NOT_7Z || e == XC_ERR_7Z_UNSUPPORTED ||
           e == XC_ERR_7Z_MEMORY || e == XC_ERR_LIMIT || e == XC_ERR_CORRUPT ||
           e == XC_ERR_ZIP_CRC || e == XC_ERR_RANGE || e == XC_ERR_GAME_FOLDER ||
           e == XC_ERR_ZIP_MEMBER || e == XC_ERR_NOMEM;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xc_mem_source mem;
    xc_source src;
    xc_source_memory(&src, &mem, data, size);
    xc_7z *a = NULL;
    char why[256];
    xc_err err = xc_7z_open(&src, 1U << 20, &a, why, sizeof why);
    check(known(err));
    if (err != XC_OK) {
        check(a == NULL);
        return 0;
    }
    static uint8_t buf[64 * 1024];
    for (size_t i = 0; i < xc_7z_count(a) && i < 64; i++) {
        check(xc_7z_path(a, i) != NULL);
        err = xc_7z_check(a, i, why, sizeof why);
        check(known(err));
        uint64_t n = xc_7z_size(a, i);
        if (err != XC_OK || xc_7z_is_dir(a, i) || n == 0) {
            continue;
        }
        uint64_t limit = n < 256U * 1024U ? n : 256U * 1024U;
        for (uint64_t off = 0; off < limit; off += sizeof buf) {
            size_t len = limit - off < sizeof buf ? (size_t)(limit - off) : sizeof buf;
            err = xc_7z_read(a, i, off, buf, len);
            check(known(err));
            if (err != XC_OK) {
                break;
            }
        }
    }
    xc_arc_folder *f = NULL;
    xc_source image;
    err = xc_7z_folder_open(a, &f, &image, why, sizeof why);
    check(known(err));
    if (err == XC_OK) {
        xc_xdvdfs_volume vol;
        xc_xdvdfs_listing l;
        memset(&l, 0, sizeof l);
        check(xc_xdvdfs_open(&image, &vol) == XC_OK);
        check(xc_xdvdfs_list(&image, &vol, NULL, &l) == XC_OK);
        check(l.file_count == xc_arc_folder_files(f));
        xc_xdvdfs_listing_free(&l);
        xc_arc_folder_free(f);
    }
    xc_7z_close(a);
    return 0;
}
