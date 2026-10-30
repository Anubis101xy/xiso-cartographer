/*
 * libFuzzer target for game folders as images (docs/formats/folder-image.md).
 *
 * The input is a listing, one item a line: "d <path>" for a folder, "f <size> <path>" for a
 * file. Building must give XC_OK, XC_ERR_GAME_FOLDER or XC_ERR_INVALID_ARG. A built image must
 * list, with the XDVDFS reader (docs/formats/xdvdfs.md), exactly the items given: the same
 * paths and sizes, and each file's first byte must be that file's. A violation aborts. Seeds
 * are in core/tests/fuzz-seeds/folder-image/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/folder_image.h"
#include "xcat/xdvdfs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_ITEMS 512

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static xc_err read_item(void *ctx, size_t item, uint64_t offset, void *buf, size_t len)
{
    (void)ctx;
    memset(buf, (int)(uint8_t)(item + offset), len);
    return XC_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static xc_fimg_item items[MAX_ITEMS];
    char *text = malloc(size + 1U);
    if (text == NULL) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';
    size_t n = 0;
    for (char *line = text; line != NULL && n < MAX_ITEMS;) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        if (line[0] == 'd' && line[1] == ' ') {
            items[n++] = (xc_fimg_item){line + 2, 0, 0, true};
        } else if (line[0] == 'f' && line[1] == ' ') {
            char *rest = NULL;
            unsigned long long sz = strtoull(line + 2, &rest, 10);
            if (rest != NULL && *rest == ' ') {
                items[n++] = (xc_fimg_item){rest + 1, sz % (1ULL << 33), (uint64_t)n, false};
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    xc_fimg *img = NULL;
    xc_source src;
    xc_fimg_problem why;
    xc_err err = xc_fimg_build(items, n, read_item, NULL, &img, &src, &why);
    check(err == XC_OK || err == XC_ERR_GAME_FOLDER || err == XC_ERR_INVALID_ARG);
    if (err != XC_OK) {
        check(img == NULL && why.why != NULL && (why.item < n || why.item == SIZE_MAX));
        free(text);
        return 0;
    }
    check(strlen(xc_fimg_validator(img)) == XC_FIMG_VALIDATOR_HEX);
    xc_xdvdfs_volume vol;
    xc_xdvdfs_listing l;
    memset(&l, 0, sizeof l);
    check(xc_xdvdfs_open(&src, &vol) == XC_OK);
    check(xc_xdvdfs_list(&src, &vol, NULL, &l) == XC_OK);
    check(l.file_count + l.dir_count == n);
    for (size_t i = 0; i < l.file_count + l.dir_count; i++) {
        const xc_xdvdfs_entry *e = i < l.file_count ? &l.files[i] : &l.dirs[i - l.file_count];
        size_t it = 0;
        while (it < n && strcmp(items[it].path, e->path) != 0) {
            it++;
        }
        check(it < n && items[it].dir == (i >= l.file_count));
        if (!items[it].dir) {
            check(e->size == items[it].size);
            if (e->size > 0) {
                uint8_t b = 0;
                check(xc_source_read(&src, e->offset, &b, 1) == XC_OK);
                check(b == (uint8_t)it);
            }
        }
    }
    xc_xdvdfs_listing_free(&l);
    xc_fimg_free(img);
    free(text);
    return 0;
}
