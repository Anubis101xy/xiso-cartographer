/* The ui_draw backend on SDL2 and SDL_ttf (draw_sdl.h, ADR 0015). */
#include "draw_sdl.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "theme.h"

#define CACHE_SLOTS 128    /* distinct strings kept as textures: a screen's worth, twice */
#define CACHE_TEXT_MAX 256 /* longer strings are rendered every time, not cached */
/* The NV2A register holding the physical address the screen shows (nxdk's hal/video.c keeps
   it private). Not XVideoSetFB: it takes a physical address and makes XVideoGetFB return it,
   which the CPU can't write through. */
#define PCRTC_START 0x00600800

/* Point sizes per font (ui_font); the line height comes from the font itself. */
static const struct {
    const char *file;
    int size;
} font_spec[UI_FONT_COUNT_] = {
    [UI_FONT_BODY] = {"NotoSans-Regular.ttf", 20},  /* list lines, text */
    [UI_FONT_BOLD] = {"NotoSans-Bold.ttf", 20},     /* the selected line */
    [UI_FONT_TITLE] = {"Michroma-Regular.ttf", 22}, /* titles; wide, so keep them short */
    [UI_FONT_SMALL] = {"NotoSans-Regular.ttf", 16}, /* details, hints */
    [UI_FONT_LABEL] = {"ChakraPetch-Bold.ttf", 21}, /* section names, labels */
};

typedef struct slot {
    SDL_Texture *tex; /* NULL: empty */
    int w, h;
    ui_font font;
    ui_color color;
    unsigned long used; /* the frame it was last drawn in, for eviction */
    char text[CACHE_TEXT_MAX + 1];
} slot;

#define WIDTH_SLOTS 512    /* measured widths kept */
#define WIDTH_TEXT_MAX 128 /* longer strings are measured every time */

typedef struct width_slot {
    ui_font font;
    size_t n;
    int w; /* zeroed: the empty string in font 0, which is 0 wide */
    char text[WIDTH_TEXT_MAX];
} width_slot;

static SDL_Window *window;
static width_slot widths[WIDTH_SLOTS];
static SDL_Renderer *renderer;
static SDL_Surface *screen; /* the window's surface, which the software renderer draws into */
static TTF_Font *fonts[UI_FONT_COUNT_];
static slot cache[CACHE_SLOTS];
static unsigned long frame;
static char scratch[1024]; /* the n bytes of a string, NUL-terminated for SDL_ttf */
/* [0] XVideoSetMode's framebuffer (write-combined), [1] and [2] ours, cached, which frames
   are drawn into directly (ui_sdl_begin); NULL if they couldn't be had. */
static unsigned char *fb[3];
static int shown;            /* the one on screen */
static void *window_pixels;  /* the window surface's own pixels, put back at close */
static SDL_Rect clip;        /* what sdl_clip limits drawing to: the screen if nothing */
static SDL_Surface *saved;   /* save_backdrop's copy, NULL until the first */
static ui_sdl_times drawing; /* the frame being drawn's */
static ui_sdl_times last;    /* the last presented frame's, for ui_sdl_last_times */

/* The files of ui_image, in the XBE's ui\ folder (app/ui/ui-assets.mk); NULL: made here. */
static const char *const image_file[UI_IMAGE_COUNT_] = {
    [UI_IMAGE_BACKGROUND] = "background.bmp",
    [UI_IMAGE_BACKGROUND_PANEL] = NULL,
};

/* In the screen's pixel format, so drawing one is a copy. NULL: not loaded. */
static SDL_Surface *images[UI_IMAGE_COUNT_];

/*
 * The pixel work is done here rather than by SDL wherever it costs: nxdk builds its C library
 * at -O0, and its memcpy copies a byte at a time. On the console a pass over the whole screen
 * costs milliseconds, bound by memory rather than the CPU (docs/ui-style.md#cost), so the
 * large copies go around the cache. Everything below works on the screen's 32-bit XRGB pixels.
 */

static uint32_t now_us(void)
{
    static ULONGLONG per_us;
    if (per_us == 0) {
        per_us = KeQueryPerformanceFrequency() / 1000000U;
    }
    return (uint32_t)(KeQueryPerformanceCounter() / per_us);
}

/* Copies n 32-bit words with the CPU's string copy, fast into write-combined memory too. */
static void copy32(void *dst, const void *src, size_t n)
{
    int d0;
    int d1;
    int d2;
    __asm__ __volatile__("cld\n\trep movsl"
                         : "=&D"(d0), "=&S"(d1), "=&c"(d2)
                         : "0"(dst), "1"(src), "2"(n)
                         : "memory");
}

/* Copies n pixels for the large copies: 64 bytes at a time through the MMX registers, reading
   ahead with prefetchnta and writing with movntq, which goes straight to memory instead of
   reading each destination line into the cache first. The Pentium III has both (SSE). Not
   inlined: the caller's x87 stack is empty at a call, so the MMX registers are free. */
__attribute__((noinline)) static void copy_px(uint32_t *dst, const uint32_t *src, size_t n)
{
    size_t blocks = n / 16U;
    if (blocks > 0) {
        __asm__ __volatile__("1:\n\t"
                             "prefetchnta 320(%1)\n\t"
                             "movq (%1), %%mm0\n\t"
                             "movq 8(%1), %%mm1\n\t"
                             "movq 16(%1), %%mm2\n\t"
                             "movq 24(%1), %%mm3\n\t"
                             "movq 32(%1), %%mm4\n\t"
                             "movq 40(%1), %%mm5\n\t"
                             "movq 48(%1), %%mm6\n\t"
                             "movq 56(%1), %%mm7\n\t"
                             "movntq %%mm0, (%0)\n\t"
                             "movntq %%mm1, 8(%0)\n\t"
                             "movntq %%mm2, 16(%0)\n\t"
                             "movntq %%mm3, 24(%0)\n\t"
                             "movntq %%mm4, 32(%0)\n\t"
                             "movntq %%mm5, 40(%0)\n\t"
                             "movntq %%mm6, 48(%0)\n\t"
                             "movntq %%mm7, 56(%0)\n\t"
                             "addl $64, %1\n\t"
                             "addl $64, %0\n\t"
                             "decl %2\n\t"
                             "jnz 1b\n\t"
                             "sfence\n\t"
                             "emms"
                             : "+r"(dst), "+r"(src), "+r"(blocks)
                             :
                             : "memory", "cc");
    }
    if (n % 16U != 0) {
        copy32(dst, src, n % 16U);
    }
}

static uint32_t *pixel(SDL_Surface *s, int x, int y)
{
    return (uint32_t *)((uint8_t *)s->pixels + (ptrdiff_t)y * s->pitch) + x;
}

/* Blends c over r of s (r inside s), red and blue in one multiply, green in another. */
static void blend(SDL_Surface *s, SDL_Rect r, ui_color c)
{
    uint32_t a = c.a + (c.a >> 7U); /* 0..256 */
    uint32_t na = 256U - a;
    uint32_t rb = (((uint32_t)c.r << 16U) | c.b) * a;
    uint32_t g = ((uint32_t)c.g << 8U) * a;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = pixel(s, r.x, r.y + y);
        for (int x = 0; x < r.w; x++) {
            uint32_t d = p[x];
            uint32_t drb = (((d & 0xFF00FFU) * na + rb) >> 8U) & 0xFF00FFU;
            uint32_t dg = (((d & 0x00FF00U) * na + g) >> 8U) & 0x00FF00U;
            p[x] = 0xFF000000U | drb | dg;
        }
    }
}

/* r within the clip and the screen; false if nothing is left. */
static bool clipped(SDL_Rect *r)
{
    SDL_Rect out;
    if (!SDL_IntersectRect(r, &clip, &out)) {
        return false;
    }
    *r = out;
    return true;
}

/* The screen with every queued SDL command drawn, for drawing into it directly. */
static SDL_Surface *drawn(void)
{
    uint32_t start = now_us();
    SDL_RenderFlush(renderer);
    drawing.sdl_us += now_us() - start;
    return screen;
}

/* s[0, n) as a C string in `scratch`, cut at its size. */
static const char *terminated(const char *s, size_t n)
{
    if (n >= sizeof scratch) {
        n = sizeof scratch - 1;
    }
    memcpy(scratch, s, n);
    scratch[n] = '\0';
    return scratch;
}

static void sdl_fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    SDL_Rect rect = {r.x, r.y, r.w, r.h};
    if (c.a == 255) {
        /* SDL fills with the CPU's string store: fast at any optimisation. */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }
    if (clipped(&rect)) {
        SDL_Surface *s = drawn();
        uint32_t start = now_us();
        blend(s, rect, c);
        drawing.blend_us += now_us() - start;
    }
}

/* The cached texture of `s` in font f and color c, rendering it on a miss (into the least
   recently drawn slot). NULL if it can't be rendered, or for an empty string. */
static slot *texture_for(ui_font f, ui_color c, const char *s)
{
    slot *victim = &cache[0];
    for (int i = 0; i < CACHE_SLOTS; i++) {
        slot *e = &cache[i];
        if (e->tex != NULL && e->font == f && memcmp(&e->color, &c, sizeof c) == 0 &&
            strcmp(e->text, s) == 0) {
            e->used = frame;
            return e;
        }
        if (e->tex == NULL || (victim->tex != NULL && e->used < victim->used)) {
            victim = e;
        }
    }
    uint32_t start = now_us();
    SDL_Color color = {c.r, c.g, c.b, c.a};
    SDL_Surface *surface = TTF_RenderUTF8_Blended(fonts[f], s, color);
    if (surface == NULL) {
        drawing.text_us += now_us() - start;
        return NULL;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    int w = surface->w;
    int h = surface->h;
    SDL_FreeSurface(surface);
    drawing.text_us += now_us() - start;
    if (tex == NULL) {
        return NULL;
    }
    if (victim->tex != NULL) {
        SDL_DestroyTexture(victim->tex);
    }
    victim->tex = tex;
    victim->w = w;
    victim->h = h;
    victim->font = f;
    victim->color = c;
    victim->used = frame;
    snprintf(victim->text, sizeof victim->text, "%s", s);
    return victim;
}

static int sdl_text(void *ctx, ui_font f, int x, int y, ui_color c, const char *s, size_t n)
{
    (void)ctx;
    if (n == 0) {
        return 0;
    }
    const char *t = terminated(s, n);
    if (n > CACHE_TEXT_MAX) {
        /* Too long to cache: render, draw and drop it. */
        SDL_Color color = {c.r, c.g, c.b, c.a};
        SDL_Surface *surface = TTF_RenderUTF8_Blended(fonts[f], t, color);
        if (surface == NULL) {
            return 0;
        }
        int w = surface->w;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
        if (tex != NULL) {
            SDL_Rect dst = {x, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, tex, NULL, &dst);
            SDL_DestroyTexture(tex);
        }
        SDL_FreeSurface(surface);
        return w;
    }
    slot *e = texture_for(f, c, t);
    if (e == NULL) {
        return 0;
    }
    SDL_Rect dst = {x, y, e->w, e->h};
    SDL_RenderCopy(renderer, e->tex, NULL, &dst);
    return e->w;
}

static int sdl_text_width(void *ctx, ui_font f, const char *s, size_t n)
{
    (void)ctx;
    uint32_t start = now_us();
    /* Fitting and wrapping measure the same prefixes every frame: FNV-1a over the font and
       the bytes picks the one slot a string can be in. */
    uint32_t hash = 2166136261U ^ (uint32_t)f;
    for (size_t i = 0; i < n; i++) {
        hash = (hash ^ (unsigned char)s[i]) * 16777619U;
    }
    width_slot *e = &widths[hash % WIDTH_SLOTS];
    if (n <= WIDTH_TEXT_MAX && e->n == n && e->font == f && memcmp(e->text, s, n) == 0) {
        drawing.size_us += now_us() - start;
        return e->w;
    }
    int w = 0;
    if (n > 0 && TTF_SizeUTF8(fonts[f], terminated(s, n), &w, NULL) != 0) {
        w = 0;
    }
    if (n <= WIDTH_TEXT_MAX) {
        e->font = f;
        e->n = n;
        e->w = w;
        memcpy(e->text, s, n);
    }
    drawing.size_us += now_us() - start;
    return w;
}

static int sdl_line_height(void *ctx, ui_font f)
{
    (void)ctx;
    return TTF_FontLineSkip(fonts[f]);
}

/* Limits drawing to rect (NULL: the screen), for SDL and for the drawing done here.
   SDL_RenderSetClipRect goes with the viewport queued first: nxdk's SDL (2.0.10's code)
   forgets the queued viewport at every flush, and a batch that starts with a clip rect then
   fails an SDL_assert in the software renderer, whose handler exits: a reboot. */
static void set_clip(const SDL_Rect *rect)
{
    SDL_Rect all = {0, 0, UI_SCREEN_W, UI_SCREEN_H};
    if (rect == NULL || !SDL_IntersectRect(rect, &all, &clip)) {
        clip = rect == NULL ? all : (SDL_Rect){0, 0, 0, 0};
    }
    SDL_RenderSetViewport(renderer, NULL);
    SDL_RenderSetClipRect(renderer, rect);
}

static void sdl_clip(void *ctx, const ui_rect *r)
{
    (void)ctx;
    if (r == NULL) {
        set_clip(NULL);
        return;
    }
    SDL_Rect rect = {r->x, r->y, r->w, r->h};
    set_clip(&rect);
}

static void sdl_image(void *ctx, ui_image img, ui_rect src, int x, int y)
{
    (void)ctx;
    SDL_Surface *from = images[img];
    if (from == NULL) {
        return;
    }
    /* Keep src inside the image, then the destination inside the clip, moving src along. */
    SDL_Rect s = {src.x, src.y, src.w, src.h};
    SDL_Rect whole = {0, 0, from->w, from->h};
    SDL_Rect in;
    if (!SDL_IntersectRect(&s, &whole, &in)) {
        return;
    }
    SDL_Rect to = {x + in.x - s.x, y + in.y - s.y, in.w, in.h};
    SDL_Rect dst = to;
    if (!clipped(&dst)) {
        return;
    }
    in.x += dst.x - to.x;
    in.y += dst.y - to.y;
    SDL_Surface *out = drawn();
    uint32_t start = now_us();
    for (int row = 0; row < dst.h; row++) {
        copy_px(pixel(out, dst.x, dst.y + row), pixel(from, in.x, in.y + row), (size_t)dst.w);
    }
    drawing.copy_us += now_us() - start;
}

static void sdl_pixels(void *ctx, const uint32_t *px, int w, int h, int x, int y)
{
    (void)ctx;
    SDL_Rect to = {x, y, w, h};
    SDL_Rect dst = to;
    if (px == NULL || w <= 0 || h <= 0 || !clipped(&dst)) {
        return;
    }
    SDL_Surface *out = drawn();
    uint32_t start = now_us();
    for (int row = 0; row < dst.h; row++) {
        const uint32_t *from = px + (size_t)(dst.y - y + row) * (size_t)w + (dst.x - x);
        copy_px(pixel(out, dst.x, dst.y + row), from, (size_t)dst.w);
    }
    drawing.copy_us += now_us() - start;
}

static void sdl_image_size(void *ctx, ui_image img, int *w, int *h)
{
    (void)ctx;
    *w = images[img] != NULL ? images[img]->w : 0;
    *h = images[img] != NULL ? images[img]->h : 0;
}

static void sdl_save_backdrop(void *ctx, ui_color shade)
{
    (void)ctx;
    SDL_Surface *s = drawn();
    if (saved == NULL) {
        saved = SDL_CreateRGBSurfaceWithFormat(0, s->w, s->h, 32, s->format->format);
        if (saved == NULL) {
            return;
        }
    }
    uint32_t start = now_us();
    for (int row = 0; row < s->h; row++) {
        copy_px(pixel(saved, 0, row), pixel(s, 0, row), (size_t)s->w);
    }
    blend(saved, (SDL_Rect){0, 0, saved->w, saved->h}, shade);
    drawing.copy_us += now_us() - start;
}

static void sdl_backdrop(void *ctx)
{
    (void)ctx;
    if (saved == NULL) {
        return;
    }
    SDL_Surface *s = drawn();
    uint32_t start = now_us();
    for (int row = 0; row < s->h; row++) {
        copy_px(pixel(s, 0, row), pixel(saved, 0, row), (size_t)s->w);
    }
    drawing.copy_us += now_us() - start;
}

/* Loads the images that exist, in the screen's format, and makes the panel-tinted
   background; a missing one is drawn without (theme.c falls back). */
static void load_images(const char *asset_dir)
{
    for (int i = 0; i < UI_IMAGE_COUNT_; i++) {
        if (image_file[i] == NULL) {
            continue;
        }
        char path[260];
        snprintf(path, sizeof path, "%s\\ui\\%s", asset_dir, image_file[i]);
        SDL_Surface *surface = SDL_LoadBMP(path);
        if (surface == NULL) {
            continue;
        }
        images[i] = SDL_ConvertSurface(surface, screen->format, 0);
        SDL_FreeSurface(surface);
    }
    SDL_Surface *bg = images[UI_IMAGE_BACKGROUND];
    if (bg != NULL) {
        SDL_Surface *tinted = SDL_ConvertSurface(bg, screen->format, 0);
        if (tinted != NULL) {
            blend(tinted, (SDL_Rect){0, 0, tinted->w, tinted->h}, ui_col_panel);
        }
        images[UI_IMAGE_BACKGROUND_PANEL] = tinted;
    }
}

/* A failed SDL_assert: SDL's handler would exit, which reboots without a word. Show it on
   the debug console instead, and stop. */
static SDL_AssertState on_assert(const SDL_AssertData *a, void *userdata)
{
    (void)userdata;
    XVideoWaitForVBlank();
    VIDEOREG(PCRTC_START) = (unsigned int)XVideoGetFB() & 0x7FFFFFFF;
    debugPrint("SDL assertion failed: %s\n%s:%d (%s)\n", a->condition, a->filename, a->linenum,
               a->function);
    for (;;) {
        Sleep(1000);
    }
}

bool ui_sdl_open(ui_draw *d, const char *asset_dir, const char **why)
{
    static char message[160];
    SDL_SetAssertionHandler(on_assert, NULL);
    if (SDL_VideoInit(NULL) != 0 || TTF_Init() != 0) {
        snprintf(message, sizeof message, "SDL video or TTF init failed: %s", SDL_GetError());
        *why = message;
        return false;
    }
    window = SDL_CreateWindow("xiso-cartographer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              UI_SCREEN_W, UI_SCREEN_H, SDL_WINDOW_SHOWN);
    renderer = window != NULL ? SDL_CreateRenderer(window, -1, 0) : NULL;
    screen = window != NULL ? SDL_GetWindowSurface(window) : NULL;
    if (renderer == NULL || screen == NULL) {
        snprintf(message, sizeof message, "SDL window or renderer failed: %s", SDL_GetError());
        *why = message;
        return false;
    }
    /* The drawing done here assumes 32-bit pixels with red, green and blue in that order. */
    const SDL_PixelFormat *pf = screen->format;
    if (pf->BytesPerPixel != 4 || pf->Rmask != 0xFF0000U || pf->Gmask != 0x00FF00U ||
        pf->Bmask != 0x0000FFU || screen->w != UI_SCREEN_W || screen->h != UI_SCREEN_H) {
        snprintf(message, sizeof message, "Unexpected screen format %s, %dx%d",
                 SDL_GetPixelFormatName(pf->format), screen->w, screen->h);
        *why = message;
        return false;
    }
    /* Two framebuffers of our own, cached, that frames are drawn into directly: the screen
       shows one while the next is drawn into the other (ui_sdl_present). XVideoSetMode's is
       write-combined, which is fast to write but very slow to read, and blending reads. Without
       them, frames are drawn into the window surface and SDL copies them to the screen. */
    VIDEO_MODE vm = XVideoGetMode();
    fb[0] = XVideoGetFB();
    window_pixels = screen->pixels;
    if (screen->pitch == vm.width * 4) {
        for (int i = 1; i <= 2; i++) {
            fb[i] = MmAllocateContiguousMemoryEx((SIZE_T)screen->pitch * vm.height, 0, 0x7FFFFFFF,
                                                 0x1000, PAGE_READWRITE);
        }
        for (int i = 1; i <= 2 && (fb[1] == NULL || fb[2] == NULL); i++) {
            if (fb[i] != NULL) {
                MmFreeContiguousMemory(fb[i]);
                fb[i] = NULL;
            }
        }
    }
    shown = 0;
    /* Only the bundled font is ever opened: nxdk's FreeType is old (ADR 0015). */
    for (int f = 0; f < UI_FONT_COUNT_; f++) {
        char path[260];
        snprintf(path, sizeof path, "%s\\fonts\\%s", asset_dir, font_spec[f].file);
        fonts[f] = TTF_OpenFont(path, font_spec[f].size);
        if (fonts[f] == NULL) {
            snprintf(message, sizeof message, "Can't open the font %s: %s", path, TTF_GetError());
            *why = message;
            return false;
        }
    }
    load_images(asset_dir);
    set_clip(NULL);
    d->ctx = NULL;
    d->fill = sdl_fill;
    d->text = sdl_text;
    d->text_width = sdl_text_width;
    d->line_height = sdl_line_height;
    d->clip = sdl_clip;
    d->image = sdl_image;
    d->image_size = sdl_image_size;
    d->save_backdrop = sdl_save_backdrop;
    d->backdrop = sdl_backdrop;
    d->pixels = sdl_pixels;
    return true;
}

/* Our framebuffer that isn't on screen, or NULL without them. */
static unsigned char *back_buffer(void)
{
    return fb[1] == NULL ? NULL : fb[shown == 1 ? 2 : 1];
}

void ui_sdl_begin(ui_color bg)
{
    frame++;
    drawing = (ui_sdl_times){0};
    /* The software renderer draws into the window surface, wherever its pixels are. */
    unsigned char *back = back_buffer();
    if (back != NULL) {
        screen->pixels = back;
    }
    set_clip(NULL);
    if (images[UI_IMAGE_BACKGROUND] == NULL) {
        SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(renderer);
    }
}

void ui_sdl_present(void)
{
    /* nxdk's SDL would copy the frame into the framebuffer on screen (SDL_RenderPresent),
       and the TV could show half of two frames. So the frame is drawn into the buffer that
       isn't shown, and the screen switches to it at the vertical blank. The software renderer
       batches its commands (nxdk's SDL is 2.0.10's code, though SDL_version.h says 2.0.9):
       run them first. */
    uint32_t start = now_us();
    SDL_RenderFlush(renderer);
    uint32_t rendered = now_us();
    drawing.finish_us = rendered - start;
    unsigned char *back = back_buffer();
    if (back == NULL) {
        SDL_RenderPresent(renderer);
        drawing.flip_us = now_us() - rendered;
        last = drawing;
        return;
    }
    /* The GPU reads memory, not the CPU's cache: write the frame's lines still in it back. */
    __asm__ __volatile__("sfence\n\twbinvd" ::: "memory");
    drawing.flip_us = now_us() - rendered;
    last = drawing;
    XVideoWaitForVBlank();
    VIDEOREG(PCRTC_START) = (unsigned int)MmGetPhysicalAddress(back);
    shown = back == fb[1] ? 1 : 2;
}

void ui_sdl_last_times(ui_sdl_times *t)
{
    *t = last;
}

uint32_t ui_sdl_us(void)
{
    return now_us();
}

void ui_sdl_close(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].tex != NULL) {
            SDL_DestroyTexture(cache[i].tex);
            cache[i].tex = NULL;
        }
    }
    for (int i = 0; i < UI_IMAGE_COUNT_; i++) {
        SDL_FreeSurface(images[i]);
        images[i] = NULL;
    }
    SDL_FreeSurface(saved);
    saved = NULL;
    for (int f = 0; f < UI_FONT_COUNT_; f++) {
        if (fonts[f] != NULL) {
            TTF_CloseFont(fonts[f]);
            fonts[f] = NULL;
        }
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (screen != NULL) {
        screen->pixels = window_pixels; /* SDL frees its own */
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    renderer = NULL;
    window = NULL;
    screen = NULL;
    if (shown != 0) {
        XVideoWaitForVBlank();
        VIDEOREG(PCRTC_START) = (unsigned int)MmGetPhysicalAddress(fb[0]);
        shown = 0;
    }
    for (int i = 1; i <= 2; i++) {
        if (fb[i] != NULL) {
            MmFreeContiguousMemory(fb[i]);
            fb[i] = NULL;
        }
    }
    TTF_Quit();
}
