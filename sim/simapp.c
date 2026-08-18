// The simulator a person can actually touch.
//
// sim/sim_main.c drives the same UI, but it is a GATE: a fixed script of ~250
// stops that renders, photographs and exits. Nobody can open it. This file is
// the other frontend -- a window, a clock and a finger -- so the signer can be
// tried without the board, in a browser tab or on a desktop.
//
// It links the REAL crypto (main/kiss_crypto.c and friends, over the vendored
// libwally), not the fakes sim_main.c carries. A tap through where the
// fingerprint is a hash of the passphrase and the address is a canned string
// teaches the shape of the product and nothing else; here the words are BIP39,
// the fingerprint is the fingerprint, and the signature is a signature.
//
// The four seams this fills are the four the device fills:
//   platform_read_touch()  <- the pointer, below
//   an lv_display flush_cb <- into g_fb
//   lv_tick_inc()          <- wall clock, not a frame counter
//   SD_BASE                <- main/platform_sd.c's POSIX branch
//
// Two frontends, one body. The browser is the product and owns its own loop in
// JS; the desktop window is the development loop and uses SDL2. The browser
// deliberately links NO SDL: LVGL has already produced every pixel, so SDL
// there would be a blitter wrapped around a blitter, and it cost an afternoon
// of black canvases before it was taken out. docs/sim/index.html reads the
// framebuffer straight out of wasm memory instead.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <SDL.h>
#endif
#include "lvgl.h"
#include "i18n.h"

#define HRES 800
#define VRES 480

void build_game(void);            // main/main.c -- the device calls this too
void kiss_trng_start(void);       // main/kiss_crypto.c
void kiss_scan_inject(const char *data, size_t len);   // main/kiss_scan.c

static uint16_t g_fb[HRES * VRES];

// The pointer. Two consumers, and the second is why this cannot simply be an
// LVGL indev: main/main.c's game_tick() samples platform_read_touch() directly,
// so the fruit game and the KISS unlock stroke never reach LVGL at all. Feed
// the seam, not the widget layer, and both halves see the same finger.
static int  g_tx, g_ty;
static bool g_pressed;

bool platform_read_touch(int *x, int *y) {
    if (!g_pressed) return false;
    *x = g_tx; *y = g_ty;
    return true;
}

static void set_touch(int x, int y, int down) {
    if (x < 0) x = 0; if (x >= HRES) x = HRES - 1;
    if (y < 0) y = 0; if (y >= VRES) y = VRES - 1;
    g_tx = x; g_ty = y; g_pressed = down ? true : false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    uint16_t *p = (uint16_t *)px;
    for (int y = area->y1; y <= area->y2; y++)
        for (int x = area->x1; x <= area->x2; x++, p++)
            if (x >= 0 && x < HRES && y >= 0 && y < VRES) g_fb[y * HRES + x] = *p;
    lv_display_flush_ready(disp);
}

static void lvgl_start(void) {
    lv_init();
    lv_display_t *d = lv_display_create(HRES, VRES);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    // Aligned on purpose. lv_display_set_buffers asserts the buffer meets
    // LV_DRAW_BUF_ALIGN, LV_ASSERT_HANDLER is "while(1);" and LV_USE_LOG is 0,
    // so an unaligned buffer is not an error message -- it is a silent hang
    // inside LVGL with no output at all. A bare uint8_t array has alignment 1
    // and lands wherever the linker puts it; this one landed on an odd address
    // and cost an hour.
    static uint8_t buf[HRES * 60 * 2] __attribute__((aligned(64)));
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    kiss_trng_start();
    build_game();
    lv_refr_now(NULL);
}

static bool write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", HRES, VRES);
    for (long i = 0; i < (long)HRES * VRES; i++) {
        uint16_t c = g_fb[i];
        unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                                 (unsigned char)(((c >>  5) & 0x3F) * 255 / 63),
                                 (unsigned char)(( c        & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

// ============================ the browser ==================================
#ifdef __EMSCRIPTEN__

// RGBA copy of the frame, because putImageData wants RGBA8888 and a canvas
// cannot read RGB565. Widened here rather than in JS: this is one pass of
// straight-line C over 384000 pixels, and the page then wraps it with no copy.
static uint8_t g_rgba[HRES * VRES * 4];

EMSCRIPTEN_KEEPALIVE void kiss_sim_boot(void) { lvgl_start(); }

// One frame, dt milliseconds of device time. The page owns the animation loop
// so that a throttled or hidden tab stalls honestly instead of fast-forwarding
// the signer's idle timers when it comes back.
EMSCRIPTEN_KEEPALIVE const uint8_t *kiss_sim_tick(int dt) {
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;
    lv_tick_inc((uint32_t)dt);
    lv_timer_handler();
    for (long i = 0, o = 0; i < (long)HRES * VRES; i++, o += 4) {
        uint16_t c = g_fb[i];
        g_rgba[o]     = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        g_rgba[o + 1] = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
        g_rgba[o + 2] = (uint8_t)(( c        & 0x1F) * 255 / 31);
        g_rgba[o + 3] = 255;
    }
    return g_rgba;
}

EMSCRIPTEN_KEEPALIVE void kiss_sim_touch(int x, int y, int down) { set_touch(x, y, down); }
EMSCRIPTEN_KEEPALIVE void kiss_sim_feed_qr(const char *d) { if (d) kiss_scan_inject(d, strlen(d)); }

EMSCRIPTEN_KEEPALIVE void kiss_sim_set_lang(const char *code) {
    for (int i = 0; i < I18N_LANG_N; i++)
        if (!strcmp(i18n_langs[i].code, code)) { i18n_set_lang(i); return; }
}
// The picker order the device itself uses, handed out so the page's <select> is
// the same list in the same order rather than a second opinion about it.
EMSCRIPTEN_KEEPALIVE int         kiss_sim_lang_count(void)   { return I18N_LANG_N; }
EMSCRIPTEN_KEEPALIVE const char *kiss_sim_lang_code(int i)   { return i18n_langs[i18n_pick_order[i]].code; }
EMSCRIPTEN_KEEPALIVE const char *kiss_sim_lang_native(int i) { return i18n_langs[i18n_pick_order[i]].native; }

int main(void) { return 0; }   // the page calls kiss_sim_boot when the user does

// ============================ the desktop ==================================
#else

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static bool g_running = true;

// Window coordinates are whatever size the user dragged the window to; every
// screen in this repo is laid out in one fixed 800x480 space. Scale here so
// nothing below ever learns the window exists.
static void map_pointer(int wx, int wy, int down) {
    int w = HRES, h = VRES;
    SDL_GetWindowSize(g_win, &w, &h);
    set_touch((int)((long)wx * HRES / (w > 0 ? w : 1)),
              (int)((long)wy * VRES / (h > 0 ? h : 1)), down);
}

static void frame(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_running = false;
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
            map_pointer(e.button.x, e.button.y, 1);
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT)
            g_pressed = false;
        else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK))
            map_pointer(e.motion.x, e.motion.y, 1);
    }
    static uint32_t last;
    uint32_t now = SDL_GetTicks(), dt = now - last;
    if (dt > 100) dt = 100;
    if (dt) { lv_tick_inc(dt); last = now; }
    lv_timer_handler();
    SDL_UpdateTexture(g_tex, NULL, g_fb, HRES * (int)sizeof(uint16_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

int main(int argc, char **argv) {
    int scale = 1, frames = 0; const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale")  && i + 1 < argc) scale  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot")   && i + 1 < argc) shot   = argv[++i];
    }
    if (scale < 1) scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    g_win = SDL_CreateWindow("KISS Signer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             HRES * scale, VRES * scale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    g_tex = g_ren ? SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING, HRES, VRES) : NULL;
    if (!g_win || !g_ren || !g_tex) { fprintf(stderr, "SDL setup: %s\n", SDL_GetError()); return 1; }

    lvgl_start();
    if (frames > 0) {                      // headless smoke, for CI
        for (int i = 0; i < frames; i++) { lv_tick_inc(16); lv_timer_handler(); }
        lv_refr_now(NULL);
        if (shot && write_ppm(shot)) printf("wrote %s\n", shot);
        SDL_Quit();
        return 0;
    }
    while (g_running) frame();
    SDL_Quit();
    return 0;
}
#endif
