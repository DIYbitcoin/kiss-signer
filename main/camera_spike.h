// Step 2 of the wallet build order: camera spike (OV02C10 over MIPI-CSI).
// Goal: prove a live video stream once, then park it. Device-only (no sim).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_lcd_types.h"

// Give the spike the DPI panel handle + BOTH framebuffers (num_fbs=2). The live
// preview is rendered by the PPA directly into the off-screen framebuffer and
// flipped — LVGL is bypassed while streaming (no tear).
void camera_spike_set_panel(esp_lcd_panel_handle_t panel, void *fb0, void *fb1);

// Toggle the live camera view on/off. Lazily initializes the pipeline on first
// use. parent is unused (kept for call-site stability).
// Returns true if now streaming.
bool camera_spike_toggle(lv_obj_t *parent, i2c_master_bus_handle_t i2c_bus);

// One-line human-readable status of the last toggle/init attempt.
const char *camera_spike_status(void);

// Confine the live preview to a rect given in LANDSCAPE UI coordinates, the same
// 800x480 space every wallet screen is laid out in. Everything outside it keeps
// whatever LVGL drew there, so a screen can put a preview in one column and real
// widgets in the other (ADDENDUM-01). Pass w or h as 0 to restore the full-panel
// default, which is what the dev preview and every pre-two-column mode use.
//
// Two consequences the caller has to know about. The camera stops FLIPPING
// framebuffers while a rect is set, because a flip swaps in the buffer LVGL did
// not just paint; it writes into the live buffer instead, so tearing is possible
// inside the preview rect and nowhere else. And LVGL keeps repainting the WHOLE
// panel while streaming, preview rect included: that is what makes a live column
// beside the video work, and a repaint that lands on the preview is corrected by
// the next video frame rather than being held off.
void camera_spike_set_preview_rect(int x, int y, int w, int h);

// True only while the live preview covers the WHOLE panel, which is the one
// state in which LVGL must not paint. With a preview rect set, LVGL paints
// everywhere and the video reclaims its rectangle on the next frame.
bool camera_spike_owns_panel(void);

// Freeze the preview WITHOUT tearing the pipeline down: the stream task keeps
// dequeuing frames but stops blitting and stops decoding, so LVGL owns the whole
// panel and no QR can land behind an overlay. Resuming costs one frame, where a
// stop-and-restart costs a second of "STARTING".
//
// Any screen that opens something over a live preview must call this. The video
// writes past LVGL straight into the scanned-out framebuffer, so an overlay alone
// is a picture with a hole in it and a decoder still running underneath.
void camera_spike_pause(bool on);

// True while the preview is live.
bool camera_spike_is_on(void);

// Poll each UI tick: true ONCE if the stream died on its own; caller repaints.
bool camera_spike_check_died(void);

// Dev orientation finder: cycle rot0/90/180/270 x mirror while live.
const char *camera_spike_cycle_orientation(void);

// Zoom in (+1) / out (-1) one step on the 5-level ladder. Indicator is drawn
// into the video frame along the landscape-right edge.
const char *camera_spike_zoom(int dir);

// One-time: remember the shared I2C bus so camera modes that start from deep
// UI code (entropy page) don't need it threaded through.
void camera_spike_set_bus(void *i2c_bus);

// ---- step 7: entropy mode — live Shannon meter drawn into the video, tap
// freezes the frame fold and reads the chip TRNG once the 6.0-bit gate passes.
// Poll _sources from an LVGL timer (the capture happens on the camera task).
bool camera_entropy_start(void);
void camera_entropy_tap(void);
// Sources 1 and 2, SEPARATELY: chain_out = the fold over sampled frames,
// trng_out = the chip read taken at capture. NEITHER is a seed. kiss_setup
// folds both with the tap chain (kiss_entropy_mix3) before any mnemonic
// exists, so the call that makes a wallet names all three inputs. Both buffers
// are wiped here as they are handed over, which is why this is one call and
// not two.
bool camera_entropy_sources(uint8_t chain_out[32], uint8_t trng_out[32]);
void camera_entropy_stop(void);
// How full source one is, 0..100, for the LVGL column beside the preview to
// draw. Reads a volatile the camera task owns, so it is a snapshot and needs no
// lock: the only consumer is an LVGL timer that redraws a bar.
int camera_entropy_progress(void);

// Why the bar is doing what it is doing, so the screen can give an instruction
// instead of leaving a stalled bar unexplained. A frame has to clear BOTH gates
// to be credited, and each failure has its own fix, so they are separate codes.
//   ENT_R_DARK   the view is too flat or too dark to hold detail: aim elsewhere
//   ENT_R_STILL  detailed, but barely changed since the last frame: move
//   ENT_R_OK     the last frame was credited
enum { ENT_R_OK = 0, ENT_R_DARK, ENT_R_STILL };
int camera_entropy_reason(void);


// ---- step 6: QR scan mode (same pipeline + k_quirc decode every few frames) ----
// on_decode runs in the CAMERA TASK context — copy the payload out, return fast.
// i2c_bus is really i2c_master_bus_handle_t (void* keeps sim includes clean).
bool camera_scan_start(void *i2c_bus, void (*on_decode)(const char *data, size_t len));
void camera_scan_stop(void);
// Assembly progress, drawn as a bar into the video (LVGL is covered while live).
void camera_scan_progress(int seen, int total);
