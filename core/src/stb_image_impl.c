/*
 * stb_image, built for core/src/picture.c (third_party/stb/README.md, docs/formats/art.md §4):
 * its JPEG and PNG decoders only, from memory, with picture.c's allocator, which caps what a
 * picture may allocate. Compiled with stb's own warnings off (cmake/stb.cmake, app/art.mk).
 */
#include <stddef.h>

void *xc_stb_malloc(size_t n);
void *xc_stb_realloc(void *p, size_t old_n, size_t n);
void xc_stb_free(void *p);
int xc_stb_info(const unsigned char *data, int len, int *w, int *h);
unsigned char *xc_stb_load(const unsigned char *data, int len, int *w, int *h);
void xc_stb_image_free(void *p);

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 16384 /* picture.c refuses more than 2048 itself, as XC_ERR_LIMIT */
#define STBI_MALLOC(n) xc_stb_malloc(n)
#define STBI_REALLOC_SIZED(p, old_n, n) xc_stb_realloc(p, old_n, n)
#define STBI_FREE(p) xc_stb_free(p)
#ifndef XCAT_STB_ASSERT
#define STBI_ASSERT(x) ((void)0) /* the XBE: a picture that trips one is refused, not fatal */
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int xc_stb_info(const unsigned char *data, int len, int *w, int *h)
{
    int comp = 0;
    return stbi_info_from_memory(data, len, w, h, &comp);
}

unsigned char *xc_stb_load(const unsigned char *data, int len, int *w, int *h)
{
    int comp = 0;
    return stbi_load_from_memory(data, len, w, h, &comp, 4);
}

void xc_stb_image_free(void *p)
{
    stbi_image_free(p);
}
