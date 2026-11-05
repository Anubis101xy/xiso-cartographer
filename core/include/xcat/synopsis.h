#ifndef XCAT_SYNOPSIS_H
#define XCAT_SYNOPSIS_H

/*
 * A game's synopsis as XBMC4Gamers keeps it, _resources/default.xml in the game's folder
 * (docs/formats/synopsis.md): who made it, its genre, year and rating, and what it's about.
 * Read for the game's details; the file comes from a NAS and is untrusted.
 */

#include <stddef.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SYNOPSIS_MAX_FILE (64U << 10) /* §2: larger files are refused */

/* §3: each field's text, UTF-8, whitespace collapsed, cut to fit; "" if the file hasn't it. */
typedef struct xc_synopsis {
    char title[160];
    char developer[96];
    char publisher[96];
    char genre[160];
    char release_date[40];
    char year[16];
    char esrb[16];
    char rating[16];
    char features[160]; /* features_general */
    char online[160];   /* features_online */
    char title_id[16];  /* titleid */
    char overview[2048];
} xc_synopsis;

/* §2-§3. XC_ERR_LIMIT if `len` is over XC_SYNOPSIS_MAX_FILE; XC_ERR_CORRUPT if the root
 * element isn't <synopsis>. Everything else is taken as well as it can be. */
xc_err xc_synopsis_parse(const char *data, size_t len, xc_synopsis *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SYNOPSIS_H */
