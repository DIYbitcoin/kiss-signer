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
#ifdef __EMSCRIPTEN__
#include "k_quirc.h"
#include "kiss_scan.h"
#endif

#define HRES 800
#define VRES 480

void build_game(void);            // main/main.c -- the device calls this too
void kiss_trng_start(void);       // main/kiss_crypto.c
void kiss_scan_inject(const char *data, size_t len);   // main/kiss_scan.c
void sim_open_signer(const char *mnemonic, const char *passphrase);   // main/main.c

// The BIP39 vector every wallet tests against. Recognisable enough that nobody
// mistakes it for keys worth keeping, and it makes the simulator checkable:
// fingerprint 73C5DA0A, and m/84h/1h/0h/0/0 is a value pinned in test_crypto.c.
#define SIM_TEST_WORDS "abandon abandon abandon abandon abandon abandon " \
                       "abandon abandon abandon abandon abandon about"

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

// The known-answer wallet. abandon x11 + about is the BIP39 vector every wallet
// on earth tests against, fingerprint 73C5DA0A -- so nobody mistakes it for
// keys worth keeping, and every address the simulator shows can be checked
// against a published table, or against the owner's own signer.
EMSCRIPTEN_KEEPALIVE void kiss_sim_test_signer(void) {
    sim_open_signer(SIM_TEST_WORDS, NULL);
}

// The viewfinder rect, read from the screen that draws it. The page used to
// carry these four numbers as hand-computed CSS percentages.
EMSCRIPTEN_KEEPALIVE int kiss_sim_view_rect(int i) {
    int r[4];
    kiss_scan_view_rect(&r[0], &r[1], &r[2], &r[3]);
    return (i >= 0 && i < 4) ? r[i] : 0;
}

// ---- the lens -------------------------------------------------------------
// The page owns the camera, because only a browser can ask for one. It owns
// nothing else: the frame goes through the SAME k_quirc the device decodes
// with, and a payload goes to the SAME kiss_scan_inject() the device's decode
// callback calls, so BC-UR fountain, pMofN and static all travel the real
// assembly path in main/qr_transport.c. A QR that this simulator can read is a
// QR the signer can read.
static k_quirc_t *g_q;
static int g_qw, g_qh;

// Hand the page the grayscale buffer to fill. Sized on demand: the video's
// resolution is whatever camera the visitor has, not something to guess at.
EMSCRIPTEN_KEEPALIVE uint8_t *kiss_sim_cam_frame(int w, int h) {
    if (!g_q && !(g_q = k_quirc_new())) return NULL;
    if (w != g_qw || h != g_qh) {
        if (k_quirc_resize(g_q, w, h) != 0) return NULL;
        g_qw = w; g_qh = h;
    }
    int bw, bh;
    return k_quirc_begin(g_q, &bw, &bh);
}

// Decode whatever the page just wrote there. Returns how many payloads reached
// the signer, so the page can show that a code was read without inventing its
// own idea of progress -- the screen's own progress bar is the real one.
EMSCRIPTEN_KEEPALIVE int kiss_sim_cam_decode(void) {
    if (!g_q) return 0;
    k_quirc_end(g_q, false);
    int n = k_quirc_count(g_q), fed = 0;
    for (int i = 0; i < n; i++) {
        static k_quirc_result_t res;      // 2.6KB, kept off the stack
        if (k_quirc_decode(g_q, i, &res) == K_QUIRC_SUCCESS && res.data.payload_len > 0) {
            kiss_scan_inject((const char *)res.data.payload, (size_t)res.data.payload_len);
            fed++;
        }
        // Static, so it outlives the scan in .bss. A SeedQR restore puts a
        // whole mnemonic in there and a passphrase QR puts the passphrase; the
        // device wipes it for that reason and so does this.
        memset(&res, 0, sizeof res);
    }
    return fed;
}

// Whether the signer is on its scan screen, so the page shows the lens exactly
// when the device would be streaming and hides it the rest of the time.
EMSCRIPTEN_KEEPALIVE int kiss_sim_scanning(void) { return kiss_scan_active() ? 1 : 0; }
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

// Drawing KISS with a mouse is genuinely awkward, and it is the first thing
// between a person and the signer. So: a key that draws it for them.
//
// Replayed, not bypassed. These are the exact points sim/sim_main.c's
// draw_cover_word() feeds the gate walk, so the stroke goes through the real
// recogniser in kiss_gword.c and opens whichever door the real ink opens. A
// hook that skipped detection would let the recogniser rot without any of the
// three harnesses noticing.
//
// {-1,-1} lifts the finger, {-2,-2} ends the script. One point per frame,
// matching the walk's pump(1) -- the game's sampler reads every tick.
#define KP_UP  -1
#define KP_END -2
static int16_t g_kiss[220][2];
static int g_kiss_n, g_kiss_at = -1, g_kiss_hold;

static void kiss_script_build(void)
{
    int n = 0, i;
    for (i = 0; i <= 9; i++) { g_kiss[n][0] = 140;          g_kiss[n][1] = (int16_t)(120 + i * 20); n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    for (i = 0; i <= 6; i++) { g_kiss[n][0] = (int16_t)(140 + i * 15); g_kiss[n][1] = (int16_t)(210 - i * 13); n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    for (i = 0; i <= 6; i++) { g_kiss[n][0] = (int16_t)(140 + i * 15); g_kiss[n][1] = (int16_t)(210 + i * 15); n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    for (i = 0; i <= 8; i++) { g_kiss[n][0] = 285;          g_kiss[n][1] = (int16_t)(130 + i * 21); n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    static const int16_t s5[7][2] = {{420,140},{360,152},{345,188},{400,212},{422,250},{362,286},{342,272}};
    static const int16_t s6[7][2] = {{540,140},{480,152},{465,188},{520,212},{542,250},{482,286},{462,272}};
    for (i = 0; i < 7; i++) { g_kiss[n][0] = s5[i][0]; g_kiss[n][1] = s5[i][1]; n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    for (i = 0; i < 7; i++) { g_kiss[n][0] = s6[i][0]; g_kiss[n][1] = s6[i][1]; n++; }
    g_kiss[n][0] = KP_UP; g_kiss[n++][1] = KP_UP;
    g_kiss[n][0] = KP_END; g_kiss[n++][1] = KP_END;
    g_kiss_n = n;
}

// One step per frame. Returns true while the script still owns the pointer, so
// the mouse cannot fight it half way through a letter.
static bool kiss_script_step(void)
{
    if (g_kiss_at < 0) return false;
    if (g_kiss_hold > 0) { g_kiss_hold--; return true; }
    int16_t x = g_kiss[g_kiss_at][0], y = g_kiss[g_kiss_at][1];
    g_kiss_at++;
    if (x == KP_END) { g_kiss_at = -1; g_pressed = false; return false; }
    if (x == KP_UP)  { g_pressed = false; g_kiss_hold = 2; return true; }
    set_touch(x, y, 1);
    return true;
}

// ---- the sim's own chrome, below the panel ---------------------------------
//
// The device screen is 800x480 and nothing that is not the device belongs
// inside it. So the window is taller than the panel and the extra strip carries
// the controls, the same arrangement the browser page uses with its side panel.
//
// It needs labels, and SDL2 without SDL_ttf cannot draw text, so here is a 5x7
// font covering the characters these three strings use. Cheaper than a
// dependency and it never has to grow.
#define STRIP_H 64
static const struct { char c; uint8_t r[7]; } FONT5x7[] = {
    {' ',{0,0,0,0,0,0,0}},        {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x1E,0x11,0x11,0x11,0x1E}}, {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, {'E',{0x1F,0x10,0x1E,0x10,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x1E,0x10,0x10,0x10,0x10}}, {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H',{0x11,0x11,0x1F,0x11,0x11,0x11,0x11}}, {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'.',{0,0,0,0,0,0x06,0x06}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'\'',{0x04,0x04,0,0,0,0,0}},
};
static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof FONT5x7 / sizeof *FONT5x7; i++)
        if (FONT5x7[i].c == c) return FONT5x7[i].r;
    return NULL;
}

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static bool g_running = true;
static int  g_scale = 1;
// The two controls, in window coordinates before scaling.
// One control. Two was a menu, and a menu is a question asked before the
// visitor knows enough to answer it.
static const SDL_Rect BTN_OPEN = { 16, 494, 246, 30 };

static void draw_text(const char *t, int x, int y, int px, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
    for (; *t; t++, x += 6 * px) {
        const uint8_t *gl = glyph(*t);
        if (!gl) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (gl[row] & (0x10 >> col)) {
                    SDL_Rect d = { (x + col * px) * g_scale, (y + row * px) * g_scale,
                                   px * g_scale, px * g_scale };
                    SDL_RenderFillRect(g_ren, &d);
                }
    }
}

static void draw_button(SDL_Rect b, const char *label) {
    SDL_Rect s = { b.x * g_scale, b.y * g_scale, b.w * g_scale, b.h * g_scale };
    SDL_SetRenderDrawColor(g_ren, 42, 51, 70, 255);
    SDL_RenderFillRect(g_ren, &s);
    SDL_SetRenderDrawColor(g_ren, 90, 104, 130, 255);
    SDL_RenderDrawRect(g_ren, &s);
    int w = (int)strlen(label) * 6 * 2;
    draw_text(label, b.x + (b.w - w) / 2, b.y + (b.h - 14) / 2, 2, 232, 238, 247);
}

static bool in_rect(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Window coordinates are whatever size the user dragged the window to; every
// screen in this repo is laid out in one fixed 800x480 space. Scale here so
// nothing below ever learns the window exists.
// ---- making a mouse behave like a finger -----------------------------------
//
// Two things make drawing with a mouse harder than it should be, and neither is
// the recogniser's fault.
//
// SDL coalesces motion. Every event that arrives in one frame overwrites the
// last, and the seam is sampled ONCE per frame by two readers -- main.c's
// game_tick and kiss_ui.c's indev -- so a quick flick reaches kiss_gword.c as a
// handful of scattered points where a finger, moving slowly against glass,
// would have left a dense path. So motion is interpolated into a queue at a
// fixed spacing and played out one point per frame, and the button coming up
// does NOT lift the finger until that queue has drained. The whole stroke gets
// seen, however fast it was drawn.
//
// And holding a button down for the length of six letters is just tiring.
// Space, or the right button, latches the pen: move freely, press again to
// lift. Nothing below the seam can tell the difference.
#define PATH_MAX_PTS 512
#define PATH_STEP    18          // px between sampled points, in the 800x480 space
static int16_t g_path[PATH_MAX_PTS][2];
static int g_path_head, g_path_tail;
static bool g_btn_down;          // the physical button (or the latch)
static bool g_latched;           // space / right click: pen stays down
static int  g_last_qx = -1, g_last_qy = -1;

static void path_push(int x, int y) {
    int nxt = (g_path_head + 1) % PATH_MAX_PTS;
    if (nxt == g_path_tail) return;            // full: keep the earlier shape
    g_path[g_path_head][0] = (int16_t)x;
    g_path[g_path_head][1] = (int16_t)y;
    g_path_head = nxt;
}

// Walk from the last queued point to this one, dropping a point every
// PATH_STEP, so a fast drag is as dense as a slow one.
static void path_extend(int x, int y) {
    if (g_last_qx < 0) { path_push(x, y); g_last_qx = x; g_last_qy = y; return; }
    int dx = x - g_last_qx, dy = y - g_last_qy;
    int dist = (int)(0.5 + __builtin_sqrt((double)(dx * dx + dy * dy)));
    int steps = dist / PATH_STEP;
    for (int i = 1; i <= steps; i++)
        path_push(g_last_qx + dx * i / steps, g_last_qy + dy * i / steps);
    if (steps > 0) { g_last_qx = x; g_last_qy = y; }
}

static void path_clear(void) { g_path_head = g_path_tail = 0; g_last_qx = g_last_qy = -1; }

// One queued point per frame, so both readers of the seam see the same one.
// Returns false only once the queue is empty AND the button is up.
static bool path_step(void) {
    if (g_path_tail != g_path_head) {
        set_touch(g_path[g_path_tail][0], g_path[g_path_tail][1], 1);
        g_path_tail = (g_path_tail + 1) % PATH_MAX_PTS;
        return true;
    }
    if (g_btn_down) return true;               // held still: keep reporting it
    g_pressed = false;
    return false;
}

// Window coordinates -> the 800x480 panel. The strip below it is not the panel,
// so a press down there never reaches the signer.
static void map_pointer(int wx, int wy) {
    path_extend(wx / g_scale, wy / g_scale);
}
static bool in_panel(int wx, int wy) { return wy / g_scale < VRES; }
static void render(void);

static void pen_down(int wx, int wy) {
    path_clear();
    g_btn_down = true;
    map_pointer(wx, wy);
}

// One press, one destination: a signer with keys in it, so every tile works.
//
// It does NOT play the cover word first. That was tried: the stroke's own door
// opens a frame or two AFTER the script drains -- game_tick classifies on the
// release -- so the setup wizard landed on top of this and the button appeared
// to do nothing. Chasing that with a delay would be timing guesswork against
// the game loop. The gesture is still there to be found: lock the signer from
// the top left corner and draw it, or run --kiss, which is what the headless
// check uses to prove the recogniser still works.
static void open_the_signer(void) { sim_open_signer(SIM_TEST_WORDS, NULL); }

static void frame(void) {
    bool scripted = kiss_script_step();   // the script owns the pointer while it runs
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_running = false;
        else if (e.type == SDL_MOUSEBUTTONDOWN) {
            int bx = e.button.x / g_scale, by = e.button.y / g_scale;
            if (!in_panel(e.button.x, e.button.y)) {              // the strip: controls
                if (in_rect(BTN_OPEN, bx, by)) open_the_signer();
            }
            else if (e.button.button == SDL_BUTTON_LEFT && !g_latched) pen_down(e.button.x, e.button.y);
            else if (e.button.button == SDL_BUTTON_RIGHT) {       // latch / unlatch
                g_latched = !g_latched;
                if (g_latched) pen_down(e.button.x, e.button.y); else g_btn_down = false;
            }
        }
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            if (!g_latched) g_btn_down = false;                   // drains, then lifts
        }
        else if (e.type == SDL_MOUSEMOTION) {
            if (g_btn_down) map_pointer(e.motion.x, e.motion.y);
        }
        else if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_RETURN) open_the_signer();
            else if (e.key.keysym.sym == SDLK_TAB) {              // same latch, on a key
                int mx, my; SDL_GetMouseState(&mx, &my);
                g_latched = !g_latched;
                if (g_latched) pen_down(mx, my); else g_btn_down = false;
            }
            else if (e.key.keysym.sym == SDLK_ESCAPE) g_running = false;
        }
    }
    if (!scripted) path_step();
    static uint32_t last;
    uint32_t now = SDL_GetTicks(), dt = now - last;
    if (dt > 100) dt = 100;
    if (dt) { lv_tick_inc(dt); last = now; }
    lv_timer_handler();
    render();
}

// The panel, then the chrome under it. Split out so a headless run can capture
// the whole window and not just LVGL's half of it.
static void render(void) {
    SDL_UpdateTexture(g_tex, NULL, g_fb, HRES * (int)sizeof(uint16_t));
    SDL_SetRenderDrawColor(g_ren, 7, 10, 16, 255);
    SDL_RenderClear(g_ren);
    SDL_Rect panel = { 0, 0, HRES * g_scale, VRES * g_scale };
    SDL_RenderCopy(g_ren, g_tex, NULL, &panel);

    draw_button(BTN_OPEN, "OPEN THE SIGNER");
    // The one thing the simulator cannot do, said where it is asked rather than
    // left to be discovered: a mouse can trace the default word from a script,
    // but it cannot teach the recogniser a word of your own.
    draw_text("CUSTOM DRAWING NEEDS A FINGER. USE A DEVICE.",
              16, 534, 1, 122, 134, 156);
    SDL_RenderPresent(g_ren);
}

int main(int argc, char **argv) {
    int frames = 0; const char *shot = NULL; bool draw_kiss = false, open_now = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale")  && i + 1 < argc) g_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot")   && i + 1 < argc) shot   = argv[++i];
        else if (!strcmp(argv[i], "--kiss")) draw_kiss = true;   // headless: the recogniser
        else if (!strcmp(argv[i], "--open")) open_now = true;     // headless: the button
    }
    if (g_scale < 1) g_scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    g_win = SDL_CreateWindow("KISS Signer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             HRES * g_scale, (VRES + STRIP_H) * g_scale, SDL_WINDOW_SHOWN);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    g_tex = g_ren ? SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING, HRES, VRES) : NULL;
    if (!g_win || !g_ren || !g_tex) { fprintf(stderr, "SDL setup: %s\n", SDL_GetError()); return 1; }

    kiss_script_build();
    lvgl_start();
    printf("Click OPEN THE SIGNER under the panel, or press ENTER. ESC quits.\n"
           "Drawing your own word needs a finger and is not usable here.\n");
    if (draw_kiss) g_kiss_at = 0;   // headless: exercise the real recogniser
    if (open_now) open_the_signer();
    if (frames > 0) {                      // headless smoke, for CI
        for (int i = 0; i < frames; i++) {
            kiss_script_step();            // same replay the k key runs
            lv_tick_inc(16); lv_timer_handler();
        }
        lv_refr_now(NULL);
        render();
        if (shot) {
            int W = HRES * g_scale, H = (VRES + STRIP_H) * g_scale;
            uint8_t *px = malloc((size_t)W * H * 4);
            FILE *f = px ? fopen(shot, "wb") : NULL;
            if (f && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                                          px, W * 4) == 0) {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (long i = 0; i < (long)W * H; i++) {
                    uint8_t rgb[3] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0] };
                    fwrite(rgb, 1, 3, f);
                }
                printf("wrote %s\n", shot);
            }
            if (f) fclose(f);
            free(px);
        }
        SDL_Quit();
        return 0;
    }
    while (g_running) frame();
    SDL_Quit();
    return 0;
}
#endif
