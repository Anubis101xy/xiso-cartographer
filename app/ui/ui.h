#ifndef XCAT_UI_H
#define XCAT_UI_H

/*
 * The UI's drawing interface and the text helpers built on it (ADR 0015). Screens draw
 * through a ui_draw only, so this header and everything that uses it include no SDL or
 * nxdk header and build on the PC too (decision 4): draw_sdl.c draws on the Xbox, and the
 * host tests use a fake with fixed glyph widths.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SCREEN_W 640
#define UI_SCREEN_H 480
#define UI_ELLIPSIS "\xE2\x80\xA6" /* U+2026, in the bundled font */

typedef enum ui_font {
    UI_FONT_BODY,  /* Noto Sans Regular, list lines and text */
    UI_FONT_BOLD,  /* Noto Sans Bold, the highlighted line and labels */
    UI_FONT_TITLE, /* Michroma: screen titles; the app's own words only (no Greek, Cyrillic) */
    UI_FONT_SMALL, /* Noto Sans Regular, smaller: hints and details */
    UI_FONT_LABEL, /* Chakra Petch Bold: section names, labels; the app's own words only */
    UI_FONT_COUNT_
} ui_font;

/* Images the backend loads from the XBE's ui\ folder (app/ui/assets, docs/ui-style.md), and
   one it makes from them. */
typedef enum ui_image {
    UI_IMAGE_BACKGROUND, /* background.bmp: the panorama behind every screen */
    /* The panorama as seen through a panel: ui_col_panel blended over it once, at start, so a
       panel is drawn as a copy of it rather than blended pixel by pixel (theme.c). */
    UI_IMAGE_BACKGROUND_PANEL,
    UI_IMAGE_COUNT_
} ui_image;

typedef struct ui_color {
    uint8_t r, g, b, a;
} ui_color;

typedef struct ui_rect {
    int x, y, w, h;
} ui_rect;

/* What a screen can draw with. Every function gets `ctx`. Text is UTF-8. */
typedef struct ui_draw {
    void *ctx;
    void (*fill)(void *ctx, ui_rect r, ui_color c);
    /* Draws the first `n` bytes of `s` with the top of its line at y; returns the width. */
    int (*text)(void *ctx, ui_font f, int x, int y, ui_color c, const char *s, size_t n);
    /* The width in pixels of the first `n` bytes of `s`. */
    int (*text_width)(void *ctx, ui_font f, const char *s, size_t n);
    /* The height of one line of the font, including its spacing. */
    int (*line_height)(void *ctx, ui_font f);
    /* Limits drawing to `r`; NULL draws everywhere again. */
    void (*clip)(void *ctx, const ui_rect *r);
    /* Draws the part `src` of an image, unscaled, with its top left at (x, y). Draws nothing
       if the image isn't loaded. */
    void (*image)(void *ctx, ui_image img, ui_rect src, int x, int y);
    /* The image's size, or 0 x 0 if it isn't loaded. */
    void (*image_size)(void *ctx, ui_image img, int *w, int *h);
    /* Keeps a copy of the frame drawn so far, with `shade` blended over it, for backdrop. */
    void (*save_backdrop)(void *ctx, ui_color shade);
    /* Draws the copy save_backdrop kept over the whole screen; nothing if there's none. */
    void (*backdrop)(void *ctx);
    /* Draws w x h pixels (0xFFRRGGBB, row by row; opaque, their top byte ignored) with their
       top left at (x, y), inside the clip. */
    void (*pixels)(void *ctx, const uint32_t *px, int w, int h, int x, int y);
} ui_draw;

/* The title-safe area: the screen less 5 % on each side, which TVs may hide (ADR 0015). */
ui_rect ui_safe_area(void);

/* The index of the code point after the one at s[i] (i < n). A byte that doesn't start a
   valid UTF-8 sequence counts as one code point, so text never stalls. */
size_t ui_utf8_next(const char *s, size_t n, size_t i);

/* How many bytes of s[0, n) fit in max_w pixels, ending on a code point boundary. */
size_t ui_fit(const ui_draw *d, ui_font f, const char *s, size_t n, int max_w);

/* Draws s in at most max_w pixels: whole if it fits, else cut and ended with an ellipsis.
   Returns the width drawn. */
int ui_text_fit(const ui_draw *d, ui_font f, int x, int y, int max_w, ui_color c, const char *s);

/*
 * The next line when s[0, n) wraps at max_w pixels: the number of bytes on it, and in *next
 * where the line after starts (spaces at the break, and a '\n' ending the line, are skipped).
 * Lines break at the last space that fits; a word wider than the line is cut between code
 * points. A line holds at least one code point, so wrapping always moves on.
 */
size_t ui_wrap(const ui_draw *d, ui_font f, const char *s, size_t n, int max_w, size_t *next);

/* Draws s wrapped into r, at most as many lines as fit; returns the number of lines drawn.
   Text left over is not drawn, and the last line drawn ends with an ellipsis then. */
int ui_text_wrap(const ui_draw *d, ui_font f, ui_rect r, ui_color c, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_UI_H */
