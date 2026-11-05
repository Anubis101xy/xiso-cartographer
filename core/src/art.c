/* Where a game's art and details are (xcat/art.h, docs/formats/art.md). */
#include "xcat/art.h"

#include <stdio.h>
#include <string.h>

static char fold(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool ends_ci(const char *s, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    if (n < k) {
        return false;
    }
    for (size_t i = 0; i < k; i++) {
        if (fold(s[n - k + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

void xc_art_base_name(const char *image, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t n = strlen(image);
    static const char *const exts[] = {".xiso.iso", ".iso", ".xiso", ".zip", ".7z"};
    size_t cut = n;
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        if (ends_ci(image, n, exts[i]) && n > strlen(exts[i])) {
            cut = n - strlen(exts[i]);
            break;
        }
    }
    if (cut == n) {
        const char *dot = strrchr(image, '.');
        if (dot != NULL && dot != image) {
            cut = (size_t)(dot - image);
        }
    }
    if (cut >= cap) {
        cut = cap - 1;
    }
    memcpy(out, image, cut);
    out[cut] = '\0';
}

/* §2.1: pictures that belong to a whole folder. */
static const char *const folder_covers[] = {
    "_resources/artwork/poster.jpg",
    "default.tbn",
    "cover.jpg",
    "cover.png",
    "folder.jpg",
    "folder.png",
};

static void add(xc_art_plan *p, const char *a, const char *b)
{
    if (p->cover_count >= XC_ART_MAX_COVERS) {
        return;
    }
    int n = snprintf(p->covers[p->cover_count], XC_ART_MAX_PATH, "%s%s", a, b);
    if (n > 0 && (size_t)n < XC_ART_MAX_PATH) {
        p->cover_count++;
    }
}

void xc_art_plan_for(const char *image, bool own_folder, xc_art_plan *out)
{
    memset(out, 0, sizeof *out);
    if (image != NULL) {
        char base[XC_ART_MAX_PATH];
        xc_art_base_name(image, base, sizeof base);
        add(out, base, ".jpg");
        add(out, base, ".png");
        add(out, base, ".tbn");
    }
    if (image == NULL || own_folder) {
        for (size_t i = 0; i < sizeof folder_covers / sizeof folder_covers[0]; i++) {
            add(out, folder_covers[i], "");
        }
        (void)snprintf(out->synopsis, sizeof out->synopsis, "%s", "_resources/default.xml");
    }
    if (image == NULL) {
        (void)snprintf(out->xbe, sizeof out->xbe, "%s", "default.xbe");
    }
}

const char *xc_art_region(uint32_t region)
{
    uint32_t r = region & (XC_XBE_REGION_NA | XC_XBE_REGION_JAPAN | XC_XBE_REGION_WORLD);
    switch (r) {
    case XC_XBE_REGION_NA:
        return "NTSC-U";
    case XC_XBE_REGION_JAPAN:
        return "NTSC-J";
    case XC_XBE_REGION_WORLD:
        return "PAL";
    case XC_XBE_REGION_NA | XC_XBE_REGION_JAPAN:
        return "NTSC-U/J";
    case XC_XBE_REGION_NA | XC_XBE_REGION_WORLD:
        return "NTSC-U, PAL";
    case XC_XBE_REGION_JAPAN | XC_XBE_REGION_WORLD:
        return "NTSC-J, PAL";
    case XC_XBE_REGION_NA | XC_XBE_REGION_JAPAN | XC_XBE_REGION_WORLD:
        return "Region free";
    default:
        return "";
    }
}
