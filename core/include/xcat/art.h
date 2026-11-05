#ifndef XCAT_ART_H
#define XCAT_ART_H

/*
 * Where a game's picture and details are looked for (docs/formats/art.md §2): which files,
 * in which order, relative to the folder the game is in or is. The platform reads them (from a
 * NAS or the console's drive); xcat/picture.h, xcat/synopsis.h and xcat/xbe.h read what's in
 * them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_ART_MAX_PATH 192U /* bytes of a path below, with its NUL */
#define XC_ART_MAX_COVERS 12U

typedef struct xc_art_plan {
    /* §2.1: pictures, best first; the first that decodes is shown. '/'-separated. */
    char covers[XC_ART_MAX_COVERS][XC_ART_MAX_PATH];
    size_t cover_count;
    char synopsis[XC_ART_MAX_PATH]; /* §2.2: the synopsis, or "" if none is looked for */
    char xbe[XC_ART_MAX_PATH];      /* §2.3: a default.xbe beside, or "" (an image's is in it) */
} xc_art_plan;

/*
 * §2: for a game folder (`image` NULL), paths inside it; for an image or archive, paths in the
 * folder it's in, `image` its file name. `own_folder`: that folder holds only this game (it's
 * one folder down from the folder listed, smb.md §3), so its folder-wide files count as the
 * game's too.
 */
void xc_art_plan_for(const char *image, bool own_folder, xc_art_plan *out);

/* §2: the name of `image` without its image or archive extension (".xiso.iso", ".iso",
   ".xiso", ".zip", ".7z", any case; else its last extension): "Halo (USA)". */
void xc_art_base_name(const char *image, char *out, size_t cap);

/* §6: the region in the console's words: "PAL", "NTSC-U", "NTSC-J", "NTSC-U/J", "Region free"
   (all three), or "" if no region bit is set. Debug bit ignored. */
const char *xc_art_region(uint32_t region);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ART_H */
