// Step 2 of the wallet build order: camera spike (OV02C10 over MIPI-CSI).
// Goal: prove a live video stream once, then park it. Device-only (no sim).
#pragma once
#include <stdbool.h>
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
// captures SHA256(SHA256(frame) || hardware TRNG) once the 6.0-bit gate passes
// (neither source alone can weaken the seed). Poll _result from an LVGL timer
// (the capture happens on the camera task).
bool camera_entropy_start(void);
void camera_entropy_tap(void);
bool camera_entropy_result(uint8_t out[32]);
void camera_entropy_stop(void);

// ---- step 6: QR scan mode (same pipeline + k_quirc decode every few frames) ----
// on_decode runs in the CAMERA TASK context — copy the payload out, return fast.
// i2c_bus is really i2c_master_bus_handle_t (void* keeps sim includes clean).
bool camera_scan_start(void *i2c_bus, void (*on_decode)(const char *data, size_t len));
void camera_scan_stop(void);
// Assembly progress, drawn as a bar into the video (LVGL is covered while live).
void camera_scan_progress(int seen, int total);
