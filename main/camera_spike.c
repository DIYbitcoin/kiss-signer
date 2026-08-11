// Camera spike: OV02C10 -> MIPI-CSI -> ISP(RGB565) -> V4L2 -> PPA -> panel framebuffer.
// V4L2 plumbing adapted from Kern's components/video (same chip, ESP32-P4).
//
// Display path is DIRECT: the PPA writes each camera frame (crop+scale+rotate+mirror
// in one hardware op) into the DPI panel framebuffer that is NOT being scanned, then
// esp_lcd_panel_draw_bitmap() with that framebuffer pointer performs a zero-copy
// scanout FLIP (see esp_lcd_panel_dpi.c). The beam only ever reads complete frames,
// so the horizontal shear that plagued the LVGL-partial-flush path cannot happen.
// LVGL is bypassed while live; on stop the whole screen is invalidated so LVGL
// repaints the wallet UI back into the current framebuffer.
//
// This is throwaway proof-of-stream code; the real QR pipeline is step 6.
#ifndef SIMULATOR

#include "camera_spike.h"

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include <math.h>
#include <wally_crypto.h>

#include "esp_random.h"
#include "kiss_crypto.h"   // kiss_entropy_mix: camera hash + TRNG -> seed
#include "kiss_proof.h"    // WPROOF_FRAME_BYTES: the one size a proof names

#include "k_quirc.h"
#include "i18n.h"
#include "osd_strips.h"
#include "kiss_theme.h"   // wt_lock_565: the reticle's acquire colour

static const char *TAG = "camspike";

#define OV02C10_SCCB_ADDR 0x36   // the sensor Guition ships on this board's ribbon
#define CAM_BUF_NUM 2
// Native panel geometry (portrait); the PPA renders camera frames directly in
// panel orientation, so no LVGL/rot_flush work happens per frame.
#define PANEL_W 480
#define PANEL_H 800

typedef struct {
  int fd;
  uint8_t *buf[CAM_BUF_NUM];
  size_t buf_len;
  uint32_t w, h;
  bool inited;
  bool streaming;
  volatile bool stop;
  TaskHandle_t task;
} cam_t;

static cam_t s_cam = {.fd = -1};
static char s_status[96] = "CAM: not started";
static volatile bool s_task_err;     // stream task died unexpectedly (not via stop)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];            // both DPI framebuffers (flip targets)
static int s_fb_wr;                  // framebuffer the PPA writes next
static ppa_client_handle_t s_ppa;
static uint32_t s_frames;
static int64_t s_t0;

// Orientation finder (dev): tap the top-right corner while live cycles
// rot{0,90,180,270} x mirror. 0..7: (idx%4)*90 degrees, idx>=4 = mirrored.
// The old LVGL-path winner ("8/8" = rot270+mirror in logical space) composes with
// the logical->panel rotation to rot0+mirror in panel space = index 4.
static int s_orient = 4;
// Zoom ladder: explicit crop + exact N/16 scale per level. Level 0 shows (nearly)
// the FULL sensor letterboxed; deeper levels fill the screen with smaller crops.
#define ZOOM_LEVELS 6
typedef struct { uint16_t bw, bh; uint8_t n16; } zoom_lvl_t;
static const zoom_lvl_t s_zoom_tab[2][ZOOM_LEVELS] = {
    // rot 0/180 (the camera module is mounted 90deg to the landscape screen, so
    // fullscreen fill can only use ~30% of the sensor width — physics of the
    // mounting; remounting the module 90deg would double the fullscreen FOV).
    // The whole-sensor "strip" level is deliberately GONE (user: never show it).
    //   L0 = DEFAULT = widest usable view (full height, thin side bars) . L1
    //   fullscreen fill . L2+ punch-in
    {{480, 728, 16}, {384, 640, 20}, {240, 400, 32}, {192, 320, 40}, {120, 200, 64}, {96, 160, 80}},
    // rot 90/270: landscape crops (rotated into the portrait panel)
    {{1280, 720, 9}, {800, 480, 16}, {640, 384, 20}, {400, 240, 32}, {320, 192, 40}, {200, 120, 64}},
};
static int s_zoom = 0;               // DEFAULT = #1 = most zoomed out = widest usable view
static volatile int s_clear_pending; // fbs to blank before blit (zoom/orient change)
static volatile int s_osd_frames;    // frames left to show the on-video digits

// ---- two-column mode (ADDENDUM-01) ----
// The preview rect in PANEL space, and the crop/scale that fills it. Zero w
// means the legacy behavior: the camera owns all 480x800 and flips framebuffers
// every frame.
//
// The UI is an 800x480 landscape canvas rotated into a 480x800 portrait panel by
// rot_flush in main.c, which maps logical (lx,ly) -> panel (479-ly, lx). So a UI
// x range becomes a panel y range unchanged, and a UI y range becomes a panel x
// range reflected. Both conversions live in set_preview_rect so no other code
// has to hold that mapping in its head.
// Initialised to the WHOLE panel, not to zero. Every expression below adds the
// rect's origin and centres inside its size, so the fullscreen modes are the
// same arithmetic with the full panel in it; left at zero they would centre the
// picture at a negative offset and put the reticle in the corner.
static volatile int s_vp_x = 0, s_vp_y = 0, s_vp_w = PANEL_W, s_vp_h = PANEL_H;
// The same rect in LANDSCAPE space, kept because the reticle and every other
// overlay primitive already draw in landscape coordinates. Storing both means
// neither the drawing code nor the blit code has to convert.
static volatile int s_vp_lx = 0, s_vp_ly = 0, s_vp_lw = PANEL_H, s_vp_lh = PANEL_W;
static bool s_vp_on;
static volatile bool s_paused;   // see camera_spike_pause

void camera_spike_set_preview_rect(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0) {                    // restore the full-panel default
    s_vp_on = false;
    s_vp_x = 0; s_vp_y = 0; s_vp_w = PANEL_W; s_vp_h = PANEL_H;
    s_vp_lx = 0; s_vp_ly = 0; s_vp_lw = PANEL_H; s_vp_lh = PANEL_W;
    return;
  }
  s_vp_lx = x; s_vp_ly = y; s_vp_lw = w; s_vp_lh = h;
  // UI x -> panel y directly; UI y -> panel x reflected, so the far edge of the
  // UI rect becomes the near edge of the panel rect.
  s_vp_y = x;
  s_vp_h = w;
  s_vp_x = (PANEL_W - 1) - (y + h - 1);
  s_vp_w = h;
  if (s_vp_x < 0) { s_vp_w += s_vp_x; s_vp_x = 0; }
  if (s_vp_y < 0) { s_vp_h += s_vp_y; s_vp_y = 0; }
  if (s_vp_x + s_vp_w > PANEL_W) s_vp_w = PANEL_W - s_vp_x;
  if (s_vp_y + s_vp_h > PANEL_H) s_vp_h = PANEL_H - s_vp_y;
  s_vp_on = (s_vp_w > 0 && s_vp_h > 0);
  s_clear_pending = 2;                       // blank the new rect, not the panel
  // Pin framebuffer 0 and make it the one being scanned out, because from here
  // on both the video and LVGL write into it and neither one flips. Blanked
  // first: it holds whatever a previous fullscreen session left behind, and
  // flipping to that would show a stale frame until LVGL repaints over it.
  if (s_vp_on && s_fb[0] && s_panel) {
    memset(s_fb[0], 0, (size_t)PANEL_W * PANEL_H * 2);
    esp_cache_msync(s_fb[0], (size_t)PANEL_W * PANEL_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, s_fb[0]);
    s_fb_wr = 0;
    // The wallet screen under the video has to repaint into the buffer we just
    // flipped to; nothing else would ask it to.
    lv_obj_invalidate(lv_screen_active());
  }
}

// The one state in which LVGL must not paint: video on, and no preview rect, so
// the picture covers all 480x800. With a rect set, LVGL owns the panel and the
// video takes its own rectangle back every frame.
//
// This replaced a per-rect "is this area clear of the preview" test. The test
// itself was right; asking it from a flush callback was not, because LVGL flushes
// in full-width bands and the caller could only accept or drop a whole band. See
// the comment in rot_flush (main.c) for the failure that produced.
bool camera_spike_owns_panel(void)
{
  return s_cam.streaming && !s_vp_on;
}

// Freeze the picture without tearing the pipeline down. The stream task keeps
// dequeuing V4L2 buffers, so the sensor stays warm and resuming costs one frame,
// but show_frame returns before it blits and before it decodes.
//
// This exists because an LVGL overlay is not enough on its own. The video writes
// its rect straight into the scanned-out framebuffer, past LVGL entirely, so a
// help card opened over the scan screen was painted over inside the preview rect
// while the decoder went on reading QR codes behind it: the screen could advance
// to a transaction the reader never asked to scan, from a card explaining what a
// transaction is. Pausing is what makes an overlay mean what it looks like.
//
// Volatile and unguarded on purpose: one bool, written by the LVGL task and read
// by the stream task, and neither cares which frame the change lands on.
void camera_spike_pause(bool on)
{
  if (s_paused == on) return;
  s_paused = on;
  // Coming back, the rect holds whatever LVGL painted over it while we were away
  // and the zoom bars are stale, so blank it before the next picture lands.
  if (!on) s_clear_pending = 2;
}

// ---- step 6 scan mode: k_quirc runs on every SCAN_EVERY'th raw sensor frame,
// at HALF resolution (device-proven: half-res decodes where full-res chokes on
// sensor line artifacts, and it's 4x cheaper). The gray copy un-mirrors the
// image — the raw sensor is mirrored, and a mirrored QR locates but never
// decodes. See memory: qr-scan-camera-recipe.
#define SCAN_EVERY 3
// k_quirc caps images at K_QUIRC_MAX_IMAGE_DIM (1280); the sensor frame is
// 1288 wide, so the (pre-halving) crop is centered. 0.6% FOV loss.
#define SCAN_MAX_DIM 1280
static void (*s_scan_cb)(const char *data, size_t len);
static k_quirc_t *s_quirc;           // half-res decoder
static int s_scan_w, s_scan_h;       // decoder dims (half of the cropped sensor)
static volatile bool s_scan_mode;
static volatile int s_scan_seen, s_scan_total;
static volatile int s_scan_found;    // frames left to show "QR located" (yellow)
// How much of the decoded frame the located code fills, in 1/256ths, or 0 if
// the last locate came from the whole-sensor pass (see scan_decode) and so
// cannot be compared with what the panel is showing. Drives the reticle.
static volatile int s_qr_fill;
static volatile int s_scan_osd = OSD_SEARCH;   // which baked strip to draw
static uint32_t s_scan_att;
// Consecutive decode passes that located a QR, had it fully in frame, and
// still could not read it. Passes, not frames: the decoder only runs every
// SCAN_EVERY'th sensor frame, so 40 here is ~4s at 30fps. Long enough that a
// normal code (which reads within a pass or two) never trips it, short enough
// that nobody stands there re-aiming at a QR that will never resolve.
#define SCAN_STUCK_FRAMES 40
static int s_scan_stuck;

void camera_scan_progress(int seen, int total) {
  s_scan_seen = seen;
  s_scan_total = total;
}

// ---- step 7 entropy mode: gather across frames, do not gate on one.
//
// The seed is SHA256(chain || hardware TRNG), where the chain is folded from
// EVERY sampled frame the holder shows the camera. The live Shannon estimate
// over one frame's 65536-bin histogram is still computed and still shown, but
// it is now a rate, not a verdict: it says how fast the bar is filling.
//
// This used to gate a single frame at 6.0 bits and refuse anything under it.
// Three things were wrong with that.
//
// It threw away every frame but one. A sampled frame arrives ten times a
// second, and sensor read noise is independent between them while a vignette
// or a hot pixel is not. Chaining twenty frames therefore gathers real
// unpredictability that one frame cannot, and dilutes the fixed pattern
// described below rather than counting it once per attempt.
//
// The refusal bought nothing. kiss_setup folds this chain together with
// esp_fill_random AND the user's tap timing, so a wholly predictable scene
// still leaves the seed no worse than the other two.
//
// And 6.0 was picked against an image with the sensor's black pedestal intact
// and no auto exposure, both of which inflated it. There is no honest way to
// re-derive that number off the device. Accumulating dissolves the question:
// a messy scene fills the bar in about two seconds and a dull one crawls,
// with no constant having to be calibrated against a lens.
//
// What the Shannon number still is not. A histogram is order blind: shuffle
// every pixel in the frame and the estimate does not move. So any FIXED
// pattern the optics and sensor impose on every frame of every device widens
// the histogram and raises the reading without adding one bit anybody could
// not predict. The sensor's black pedestal is now zeroed at source. Lens
// vignetting is not corrected at all: ov02c10_default.json carries no lsc
// section, so the ISP's shading block is never programmed, and correcting it
// needs per lens coefficients measured on a flat field that we do not have.
//
// Which is exactly why a frame is credited with its EXCESS over a floor and
// not with its raw reading. The floor is the part of the histogram width that
// a featureless view produces anyway — vignetting, fixed pattern, the sensor's
// own noise shape. Counting it would pay the holder for pointing at a wall.
//
// A frame under the floor now scores ZERO, and that is the part that changed.
// It used to score ENT_FRAME_MIN, a trickle justified as "never stalls": the
// bar always finished, a covered lens included, in about forty seconds. An
// owner watched it fill against a dark surface and called it a bug. It is one.
// Whether the resulting seed was weak is beside the point — sources 2 and 3
// still fold in — what was broken is that a meter labelled with how much
// randomness has been gathered reported gathering where there was none, which
// is the exact failure this project spends its time auditing other wallets
// for. Nothing is stranded by the change: the screen now says WHY the bar is
// not moving, a dead camera still reaches a wallet through the taps, and DICE
// is one screen back.
//
// The second gate is NOVELTY, and it closes a hole of the same shape. The
// Shannon estimate scores ONE frame in isolation, so a detailed but motionless
// view — a printed photo under a propped device, a pipeline that has stopped
// delivering new buffers — scores high forever while adding almost nothing
// after the first frame. Spatial detail is not new information. So the strided
// subsample's green channel is kept from frame to frame and compared. Green
// because it is 6 bits where blue is 5, and the closest thing in RGB565 to a
// luma channel: it moves first when anything in the scene does.
//
// This is deliberately a test for a FROZEN source and not a steadiness meter.
// A handheld device always passes it, and should: photon shot noise across a
// real scene is genuine unpredictability and there is no reason to make an
// owner wave the thing about. What it refuses is a view that is not arriving.
//
// At 10 sampled frames a second, target 1200:
//   covered lens                 -> 0    -> never fills, and the screen says so
//   frozen view, any detail      -> 0    -> never fills, and the screen says so
//   dark wall      3.5 bits      -> 6    -> ~20s
//   lit wall       4.5 bits      -> 30   -> ~4s
//   a lit desk     6.5 bits      -> 70   -> ~1.8s
//   gravel         8.0 bits      -> 100  -> ~1.2s
#define ENT_TARGET_X10  1200            // 20 frames at the knee, ~2s when good
#define ENT_FLOOR_X10   30              // what an empty view reads on its own
#define ENT_FRAME_GAIN  2               // excess over the floor, doubled
#define ENT_FRAME_CAP   120             // so no one frame carries a session
// Novelty, in green steps and in percent of the subsample. Two 6-bit steps is
// about 8/255, above the shot noise a still scene shows in good light and far
// below anything a hand or a scene does. Full credit once a quarter of the
// samples have moved; under ENT_NOV_MIN the frame is treated as not arriving
// and scores nothing, and between the two the credit scales, so a view that is
// only just alive fills slowly rather than lying either way.
#define ENT_NOV_DELTA   2
#define ENT_NOV_MIN     3               // percent of samples, below this = frozen
#define ENT_NOV_FULL    25              // percent of samples for undiluted credit
// About 4130 of the 937,664 pixels, and 20 frames of those against a 256 bit
// output. Prime, and coprime with the 1288 pixel row pitch, so the lattice
// walks instead of landing on the same columns every frame.
#define ENT_SUB_STRIDE  227
#define ENT_SUB_MAX     4200

static void *s_bus_saved;
static volatile bool s_ent_mode;
static volatile int s_ent_meter;        // Shannon estimate of ONE frame, x10
static volatile int s_ent_accum;        // summed across frames, x10
static volatile bool s_ent_req;         // UI tapped: finish if the bar is full
static volatile bool s_ent_done;        // s_ent_hash + s_ent_trng are ready
static volatile int s_ent_reason;       // ENT_R_*: why the last frame scored what it did
static uint8_t s_ent_hash[32];          // source 1 alone: the frame fold, frozen
static uint8_t s_ent_trng[32];          // source 2 alone: the chip read at capture
static uint8_t s_ent_chain[32];         // running fold over sampled frames
static uint16_t s_ent_sub[ENT_SUB_MAX]; // strided subsample, hashed per frame
static uint8_t s_ent_prev[ENT_SUB_MAX]; // last frame's green channel, for novelty
static bool s_ent_prev_ok;              // false until the first frame lands
static uint32_t *s_ent_hist;            // 256KB histogram, PSRAM

// ---- proof mode (CAMERA AUDIT): freeze ONE whole raw frame for the owner's
// off-device audit. No meter and no gates — any frame proves the machinery,
// so there is nothing here to score and nothing that can refuse. The bytes
// are public by design (they go to the SD card in cleartext), which is why
// the buffer gets a plain free rather than a wipe.
static volatile bool s_proof_mode;
static volatile bool s_proof_req;       // UI tapped CAPTURE: copy the next frame
static volatile bool s_proof_done;      // s_proof_buf holds the frozen frame
static uint8_t *s_proof_buf;            // WPROOF_FRAME_BYTES, PSRAM

void camera_spike_set_bus(void *i2c_bus) { s_bus_saved = i2c_bus; }

// stream-task context
static void ent_frame(const uint8_t *frame, uint32_t w, uint32_t h) {
  size_t n = (size_t)w * h;
  esp_cache_msync((void *)frame, s_cam.buf_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  const uint16_t *px = (const uint16_t *)frame;
  memset(s_ent_hist, 0, 65536u * sizeof(uint32_t));
  for (size_t i = 0; i < n; i++) s_ent_hist[px[i]]++;
  double ent = 0;
  for (int i = 0; i < 65536; i++) {
    if (s_ent_hist[i]) {
      double p = (double)s_ent_hist[i] / (double)n;
      ent -= p * log2(p);
    }
  }
  s_ent_meter = (int)(ent * 10.0);

  // Fold this frame into the chain. A subsample rather than the whole frame
  // because wally_sha256 over 1.9MB is software SHA at tens of milliseconds,
  // ten times a second, on the same task that drives the preview. 8KB is free,
  // and 4130 pixels a frame across twenty frames is not a close margin against
  // 256 bits of output.
  if (!s_ent_done) {
    size_t k = 0;
    for (size_t i = 0; i < n && k < ENT_SUB_MAX; i += ENT_SUB_STRIDE)
      s_ent_sub[k++] = px[i];

    // Gate one: is there detail at all. Gate two: is any of it new. Both are
    // scored before anything is folded, because a frame that earns nothing has
    // nothing to contribute to the chain either — folding it would only dilute
    // the frames that did earn something with predictable material.
    int add = (s_ent_meter - ENT_FLOOR_X10) * ENT_FRAME_GAIN;
    if (add > ENT_FRAME_CAP) add = ENT_FRAME_CAP;
    if (add <= 0) { add = 0; s_ent_reason = ENT_R_DARK; }

    size_t moved = 0;
    for (size_t i = 0; i < k; i++) {
      uint8_t g = (uint8_t)((s_ent_sub[i] >> 5) & 0x3F);
      int dg = (int)g - (int)s_ent_prev[i];
      if (dg < 0) dg = -dg;
      if (dg >= ENT_NOV_DELTA) moved++;
      s_ent_prev[i] = g;
    }
    bool have_base = s_ent_prev_ok && k;
    s_ent_prev_ok = true;
    int nov = have_base ? (int)(moved * 100 / k) : 0;
    if (!have_base) {
      // The first frame has no baseline: every sample would read as moved
      // against a zeroed buffer. Score it nothing and say nothing about it —
      // the opening reason set by camera_entropy_start still stands.
      add = 0;
    } else if (add > 0) {
      if (nov < ENT_NOV_MIN) { add = 0; s_ent_reason = ENT_R_STILL; }
      else {
        if (nov < ENT_NOV_FULL) add = add * nov / ENT_NOV_FULL;
        if (add <= 0) { add = 0; s_ent_reason = ENT_R_STILL; }
        else s_ent_reason = ENT_R_OK;
      }
    }

    if (add > 0) {
      uint8_t d[32];
      if (wally_sha256((const unsigned char *)s_ent_sub, k * sizeof s_ent_sub[0],
                       d, sizeof d) == WALLY_OK &&
          kiss_entropy_mix(s_ent_chain, d, s_ent_chain) == 0) {
        if (s_ent_accum < ENT_TARGET_X10) s_ent_accum += add;
      }
      wally_bzero(d, sizeof d);
    }
  }

  if (s_ent_req) {
    s_ent_req = false;
    if (s_ent_accum >= ENT_TARGET_X10 && !s_ent_done) {
      // Freeze source 1 and read source 2, and keep them SEPARATE. This used
      // to fold them together and call the result a seed; it no longer does,
      // because the tap screen adds source 3 and the one call that builds a
      // wallet has to be able to name all three. Folding here would leave that
      // call site passing the same chain twice, which is a fine seed and an
      // unreadable audit.
      memcpy(s_ent_hash, s_ent_chain, sizeof s_ent_hash);
      esp_fill_random(s_ent_trng, sizeof s_ent_trng);
      s_ent_done = true;
    }                                   // an early tap just does nothing —
  }                                     // the part-filled bar already says why
}

// in-video chrome geometry (bands + bar), shared by scan and entropy modes;
// drawing helpers live further down with the rest of the chrome.
#define BAND_TOP_X0 375     // panel x range of the landscape-top band (deep
#define BAND_TOP_X1 451     // enough for a title + subtitle strip)
#define BAND_BOT_X0 34      // landscape-bottom band
#define BAND_BOT_X1 100
#define BAR_PX0     52      // bar rows inside the bottom band
#define BAR_THICK   22
#define BAR_LEN     560
#define STRIP_TOP_PX 443    // panel x of a top-band strip's first text row
static void draw_hbar(uint16_t *fb, int fill, uint16_t base);

// How much has been gathered, not how good the current frame is. It fills as
// the holder holds, faster on a messy scene than on a wall, and turns green
// when it is full and a tap would be accepted.
//
// There is no tick mark any more. It used to sit at 6.0 bits to mark the pass
// point on a bar that showed one frame's estimate. A progress bar does not
// have a pass mark: being full IS the pass mark, and a line partway along one
// only invites the question of what happens past it.
//
// The displayed fill still EASES toward the real figure, so the bar glides.
static void draw_ent_bar(uint16_t *fb) {
  static int disp;
  int target = BAR_LEN * s_ent_accum / ENT_TARGET_X10;
  if (target > BAR_LEN) target = BAR_LEN;
  disp += (target - disp) / 4;
  if (disp < 0) disp = 0;
  draw_hbar(fb, disp, s_ent_accum >= ENT_TARGET_X10 ? 0x368F : 0xF5C9);
}

// No ISP pipeline controller is configured, so the sensor just runs its
// power-on default exposure — a full frame time, which turns hand tremor into
// module-killing motion blur. While scanning, halve it; restored on stop.
//
// This was deleted for a while, on the reasoning that a running AGC meters any
// value written underneath it and corrects it away on the next frame, so the
// two would oscillate. That reasoning is still right, and it will apply again
// the day the controller comes back. It is not right today: the controller is
// off, on evidence, and the note in components/esp_cam_sensor/VENDOR.kiss.md
// section 4 says why. With nothing else driving the sensor there is nothing
// for this to fight, and without it scanning is blurrier than it was before
// any of the camera work started.
static int32_t s_exp_saved = -1;

static void scan_exposure(bool on) {
  struct v4l2_query_ext_ctrl qc = {.id = V4L2_CID_EXPOSURE};
  if (ioctl(s_cam.fd, VIDIOC_QUERY_EXT_CTRL, &qc) != 0) {
    ESP_LOGW(TAG, "scan: sensor has no exposure control");
    return;
  }
  struct v4l2_ext_control c = {.id = V4L2_CID_EXPOSURE};
  struct v4l2_ext_controls cs = {.ctrl_class = V4L2_CID_CAMERA_CLASS,
                                 .count = 1, .controls = &c};
  if (on) {
    if (ioctl(s_cam.fd, VIDIOC_G_EXT_CTRLS, &cs) == 0)
      s_exp_saved = c.value;
    else
      s_exp_saved = (int32_t)qc.default_value;
    int32_t want = s_exp_saved / 2;   // mild: halves motion smear, preview stays usable
    if (want < (int32_t)qc.minimum) want = (int32_t)qc.minimum;
    c.value = want;
    if (ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &cs) == 0)
      ESP_LOGI(TAG, "scan: exposure %d -> %d (min %lld max %lld)",
               (int)s_exp_saved, (int)want, (long long)qc.minimum,
               (long long)qc.maximum);
    else
      ESP_LOGW(TAG, "scan: set exposure failed");
  } else if (s_exp_saved >= 0) {
    c.value = s_exp_saved;
    ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &cs);
    s_exp_saved = -1;
  }
}

static void set_status(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status, sizeof(s_status), fmt, ap);
  va_end(ap);
  ESP_LOGI(TAG, "%s", s_status);
}

const char *camera_spike_status(void) { return s_status; }

// TEMPORARY diagnostic. esp_video reports exactly which ioctl a sensor refused
// through ESP_LOGE, then collapses every one of them into a single flat return
// code, so the failure screen can only say NOT_SUPPORTED. There is no serial
// console to read the real line from either: CONFIG_ESP_CONSOLE_UART_DEFAULT
// puts the log on UART0 and the only cable on this device is USB.
//
// So tap the log for the length of esp_video_init and keep the last line that
// says something failed. Every gate in esp_video_isp_pipeline.c phrases its
// error as "failed to <thing>", which is precisely the thing we cannot
// otherwise see. Written as a diagnostic and kept as a feature: it is what
// turns "camera unavailable" into a line a holder can read off the glass and
// send us, on a device that will never have a console attached.
static char s_cam_log[64];
static vprintf_like_t s_cam_log_prev;

static int cam_log_tap(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    char line[192];
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);

    if (n > 0) {
        const char *hit = strstr(line, "failed");
        if (hit) {
            size_t k = 0;
            while (hit[k] && hit[k] != '\r' && hit[k] != '\n' &&
                   hit[k] != '\033' && k < sizeof s_cam_log - 1) {
                s_cam_log[k] = hit[k];
                k++;
            }
            s_cam_log[k] = '\0';
        }
    }
    return s_cam_log_prev ? s_cam_log_prev(fmt, ap) : n;
}

bool camera_spike_is_on(void) { return s_cam.streaming; }

// Poll from the UI loop: true ONCE if the stream died on its own (error, not a
// user stop). Caller should repaint the LVGL UI and show camera_spike_status().
bool camera_spike_check_died(void) {
  if (!s_task_err) return false;
  s_task_err = false;
  s_cam.streaming = false;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
  return true;
}

void camera_spike_set_panel(esp_lcd_panel_handle_t panel, void *fb0, void *fb1) {
  s_panel = panel;
  s_fb[0] = fb0;
  s_fb[1] = fb1;
}

const char *camera_spike_cycle_orientation(void) {
  s_orient = (s_orient + 1) % 8;
  s_clear_pending = 2;
  s_osd_frames = 60;                  // ~2s of on-video digits
  set_status("CAM: orientation %d/8 (rot %d%s)", s_orient + 1,
             (s_orient % 4) * 90, s_orient >= 4 ? " + mirror" : "");
  return s_status;
}

const char *camera_spike_zoom(int dir) {
  int z = s_zoom + dir;
  if (z < 0) z = 0;
  if (z >= ZOOM_LEVELS) z = ZOOM_LEVELS - 1;
  if (z != s_zoom) s_clear_pending = 2;   // letterbox bars change: blank both fbs
  s_zoom = z;
  s_osd_frames = 60;
  set_status("CAM: zoom %d/%d", s_zoom + 1, ZOOM_LEVELS);
  return s_status;
}

// NOTE (learned on device): a sideways-rotated LIVE view can never be made to read
// correctly by turning the device — the camera rotates with the screen, so the
// render rotation follows you around. Whole-sensor views must stay upright (slim).
// Pick the source crop + scale for the current orientation+zoom. Output is
// crop*N/16 exactly (integer, no Q4.4 edge garbage), centered on the panel —
// smaller than the panel at the letterbox levels.
static bool orient_geometry(uint32_t w, uint32_t h, uint32_t *cw, uint32_t *ch,
                            float *scale, uint32_t *ow, uint32_t *oh) {
  // Two-column mode has one fixed crop and scale sized to the preview rect, and
  // ignores the zoom ladder: zoom and the orientation finder are dev
  // affordances on the fullscreen preview, and a letterbox bar inside a 300px
  // column would eat most of it.
  if (s_vp_on) {
    // Fill the rect exactly, and derive the crop from it rather than from a
    // table: the output size is whatever the screen asked for, so the crop is
    // that at 2x and the scale is a clean 8/16. If the sensor cannot give 2x
    // (a rect wider than half the frame) fall back to 1:1, which always can.
    *ow = (uint32_t)s_vp_w;
    *oh = (uint32_t)s_vp_h;
    if (*ow * 2 <= w && *oh * 2 <= h) {
      *cw = *ow * 2; *ch = *oh * 2; *scale = 8 / 16.0f;
    } else if (*ow <= w && *oh <= h) {
      *cw = *ow; *ch = *oh; *scale = 1.0f;
    } else {
      return false;
    }
    return true;
  }
  bool quarter = (s_orient % 2) == 1;  // 90/270 swaps output dims
  int zi = quarter ? 1 : 0;
  for (int z = s_zoom; z < ZOOM_LEVELS; z++) {  // fall deeper if crop won't fit sensor
    const zoom_lvl_t *L = &s_zoom_tab[zi][z];
    if (L->bw > w || L->bh > h) continue;
    uint32_t sw = L->bw * L->n16 / 16, sh = L->bh * L->n16 / 16;
    *cw = L->bw; *ch = L->bh; *scale = L->n16 / 16.0f;
    if (quarter) { *ow = sh; *oh = sw; } else { *ow = sw; *oh = sh; }
    if (*ow > PANEL_W || *oh > PANEL_H) continue;
    return true;
  }
  return false;
}

// Alpha-blit a composed 4-bit-alpha strip (anti-aliased text from osd_text.c),
// upright in landscape: (ux,uy) -> panel px = cx - uy, py = cy + ux. cx is the
// panel x of the strip's FIRST text row; the strip grows toward screen-bottom.
//
// dim scales the coverage, 0..255. The baked art used to carry its muting in
// its own alpha, drawing subtitles at 145 and the close hint at 190. A composed
// strip is always full coverage and has to be, because full coverage is what
// sim/osdcheck.c compares against LVGL's own label draw. So the muting moved
// here, at the same numbers, and nothing on screen changed brightness.
static void blit_a4(uint16_t *fb, const scan_osd_strip_t *s, int cx, int cy,
                    int dim) {
  if (!s || !s->a4) return;                   // a strip that failed to compose
  for (int uy = 0; uy < s->h; uy++) {
    int px = cx - uy;
    if (px < 0 || px >= PANEL_W) continue;
    for (int ux = 0; ux < s->w; ux++) {
      int i = uy * s->w + ux;
      uint8_t a = (i & 1) ? (s->a4[i >> 1] & 0x0F) : (s->a4[i >> 1] >> 4);
      if (!a) continue;
      int py = cy + ux;
      if (py < 0 || py >= PANEL_H) continue;
      uint16_t d = fb[py * PANEL_W + px];
      int aa = a * 17 * dim / 255;             // 0..255
      int r = (d >> 11) & 31, g = (d >> 5) & 63, b = d & 31;
      r += ((31 - r) * aa) >> 8;
      g += ((63 - g) * aa) >> 8;
      b += ((31 - b) * aa) >> 8;
      fb[py * PANEL_W + px] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// Blend a solid landscape-space rectangle toward an arbitrary RGB565 colour
// (a = 0..15). Components are in 565 scale: r,b are 0..31 and g is 0..63.
//
// Division, not >>8, on the delta: it is signed here. Blending DOWN toward a
// darker target makes (t - c) negative, and an arithmetic shift of a negative
// value rounds away from zero, so the colour would creep past its target and
// keep going a little every frame.
static void lrect_blend_rgb(uint16_t *fb, int lx, int ly, int lw, int lh,
                            uint8_t a, int tr, int tg, int tb) {
  int aa = a * 17;
  for (int yy = ly; yy < ly + lh; yy++) {
    int px = (PANEL_W - 1) - yy;
    if (px < 0 || px >= PANEL_W) continue;
    for (int xx = lx; xx < lx + lw; xx++) {
      if (xx < 0 || xx >= PANEL_H) continue;
      uint16_t d = fb[xx * PANEL_W + px];
      int r = (d >> 11) & 31, g = (d >> 5) & 63, b = d & 31;
      r += ((tr - r) * aa) / 256;
      g += ((tg - g) * aa) / 256;
      b += ((tb - b) * aa) / 256;
      fb[xx * PANEL_W + px] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// Blend toward white — the shimmer and the searching brackets.
static void lrect_blend(uint16_t *fb, int lx, int ly, int lw, int lh, uint8_t a) {
  lrect_blend_rgb(fb, lx, ly, lw, lh, a, 31, 63, 31);
}

// Camera-app viewfinder: four corner brackets marking the region that is
// actually DECODED, so "fill the brackets" is true advice.
//
// It used to be a 260x260 box tucked between the two OSD bands, chosen to look
// tidy. That quietly instructed the one thing that cannot work on a dense code:
// a QR filling 260 landscape px lands on ~2.2 pixels per module for a
// version-25 PSBT, and quirc needs cleaner edges than that. The device said
// "put it here", the user did, and it never read until they ignored the guide
// and moved closer. See scan_decode for the other half of that bug.
//
// The bands are NOT an exclusion zone. They are drawn into the display
// framebuffer only; scan_decode reads the raw sensor frame, so nothing under a
// band is lost to the decoder. Letting the brackets run the full height of the
// view costs a little visual tidiness under the bands and buys ~450 px across
// a filled code, which is 3.8 px/module at version 25 -- the difference
// between "never reads" and "reads instantly".
// The frame is 30fps (see the 60-frame OSD hold, commented as ~2s), so these
// periods are in thirtieths of a second. Kept as named frame counts rather
// than milliseconds because s_frames is the only clock this path has.
#define BRK_BREATH_F 60     // 2s: brackets breathe while searching
#define BRK_HALF     225    // half the guide box, when nothing is located
#define BRK_HALF_MIN 95     // never close tighter than this, however small the code

// Where the brackets are now, eased toward where the located code says they
// should be. Eased rather than snapped because the fill estimate jitters by a
// few percent between frames as the finder squares are re-located, and a box
// that twitched would look like a fault rather than a lock.
static int s_brk_half = BRK_HALF;

static void draw_brackets(uint16_t *fb) {
  // Centre and travel limits come from the preview rect, which defaults to the
  // whole landscape screen, so the fullscreen numbers below are the same
  // expression. In two-column mode the reticle keeps every behaviour it has --
  // the breathing while searching, the ease toward the located code, the close
  // on acquire -- inside the 300px column instead of across the panel. That
  // gesture is the one thing on this screen that says the device is looking, so
  // it follows the picture rather than being dropped with the text chrome LVGL
  // took over.
  const int cx = s_vp_lx + s_vp_lw / 2, cy = s_vp_ly + s_vp_lh / 2;
  const int arm = s_vp_on ? 26 : 44, t = s_vp_on ? 3 : 4;
  // Half the guide box, and how tight it may close. Fullscreen keeps its
  // measured 225/95; a column derives them from its own short side, with a 6px
  // margin so the corner arms never cross the border LVGL drew around it.
  const int brk_half = s_vp_on
      ? (s_vp_lw < s_vp_lh ? s_vp_lw : s_vp_lh) / 2 - 6 : BRK_HALF;
  const int brk_min = s_vp_on ? arm + 8 : BRK_HALF_MIN;
  bool found = s_scan_found > 0;

  // Closing in on the code is the whole "it found it" gesture. s_qr_fill is
  // how much of the frame the code occupies; the guide follows it down, with a
  // floor so a distant code does not shrink the guide into a dot the user then
  // cannot aim with.
  int want = brk_half;
  if (found && s_qr_fill > 0) {
    want = brk_half * s_qr_fill / 256 + arm / 2;
    if (want < brk_min) want = brk_min;
    if (want > brk_half) want = brk_half;
  }
  s_brk_half += (want - s_brk_half) / 4;          // ~4 frames to settle
  if (s_brk_half > brk_half) s_brk_half = brk_half;
  if (s_brk_half < brk_min) s_brk_half = brk_min;
  const int half = s_brk_half;
  // Located: solid, and coloured rather than white. It used to be a hardcoded
  // 6/52/15 -- WT_OK's green -- on the reasoning that green is the status-OK
  // colour everywhere else on the device. But this is not a status readout, it
  // is the one piece of chrome on the whole scan screen, and chrome wears the
  // accent. wt_lock_565 answers with the accent and keeps the green for MONO,
  // which has no accent to wear. Asked once per frame: it is three shifts.
  // Searching: a slow breath between 5 and 11, so the guide reads as live
  // rather than as a static overlay somebody forgot to remove.
  int lr, lg, lb;
  wt_lock_565(&lr, &lg, &lb);
  int ph = (int)(s_frames % BRK_BREATH_F);
  int tri = ph < BRK_BREATH_F / 2 ? ph : BRK_BREATH_F - ph;   // 0..30..0
  uint8_t a = found ? 15 : (uint8_t)(5 + tri * 6 / (BRK_BREATH_F / 2));
  for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2) {
      int x = cx + sx * half, y = cy + sy * half;
      if (found) {
        lrect_blend_rgb(fb, sx < 0 ? x : x - arm, y - t / 2, arm, t, a, lr, lg, lb);
        lrect_blend_rgb(fb, x - t / 2, sy < 0 ? y : y - arm, t, arm, a, lr, lg, lb);
      } else {
        lrect_blend(fb, sx < 0 ? x : x - arm, y - t / 2, arm, t, a);
        lrect_blend(fb, x - t / 2, sy < 0 ? y : y - arm, t, arm, a);
      }
    }

  // A second, thinner bracket set inset from the first, and short ticks at the
  // midpoint of each edge. Together they read as a sighting reticle rather
  // than a photo app's crop marks, which is the whole ask -- and both are the
  // same two-rectangle primitive as the corners, so they cost the same
  // nothing per frame.
  {
    const int in = s_vp_on ? 12 : 22, arm2 = s_vp_on ? 14 : 26, t2 = 2;
    uint8_t a2 = (uint8_t)(a > 6 ? a - 4 : 2);
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) {
        int x = cx + sx * (half - in), y = cy + sy * (half - in);
        lrect_blend(fb, sx < 0 ? x : x - arm2, y - t2 / 2, arm2, t2, a2);
        lrect_blend(fb, x - t2 / 2, sy < 0 ? y : y - arm2, t2, arm2, a2);
      }
    const int tick = 26;
    lrect_blend(fb, cx - t2 / 2, cy - half, t2, tick, a2);          // top
    lrect_blend(fb, cx - t2 / 2, cy + half - tick, t2, tick, a2);   // bottom
    lrect_blend(fb, cx - half, cy - t2 / 2, tick, t2, a2);          // left
    lrect_blend(fb, cx + half - tick, cy - t2 / 2, tick, t2, a2);   // right
  }

  // There WAS a line sweeping down the guide while nothing was located, to say
  // "still looking" during the state that otherwise has no motion. It is gone,
  // and the reason is what it looked like rather than what it meant: the bar
  // spanned the full width of the guide, so the code being aimed at covered its
  // middle and only the slivers either side of the QR were visible. Two short
  // dark segments crawling up the picture read as a rendering fault, not as a
  // search. The breath above already says the device is live, and the guide
  // closing on acquire already says it found something.
}

// Draw the zoom indicator into a framebuffer (panel coords). The landscape-right
// screen edge = the last panel rows; segments run along panel x. Inset well away
// from the edge — the panel has 30-50px of overscan hidden behind the bezel.
static void draw_zoom_bar(uint16_t *fb) {
  const int seg_w = 40, seg_h = 12, gap = 10;
  const int total = ZOOM_LEVELS * seg_w + (ZOOM_LEVELS - 1) * gap;
  const int x0 = (PANEL_W - total) / 2, y0 = PANEL_H - 90;
  for (int s = 0; s < ZOOM_LEVELS; s++) {
    uint16_t col = (s <= s_zoom) ? 0xFFFF : 0x39E7;   // filled vs dim gray
    for (int y = 0; y < seg_h; y++) {
      uint16_t *row = fb + (y0 + y) * PANEL_W + x0 + s * (seg_w + gap);
      for (int x = 0; x < seg_w; x++) row[x] = col;
    }
  }
}

// ---- in-video chrome: feathered darkened bands (landscape top and bottom),
// anti-aliased text on the top band, meter/progress in the bottom band. All
// raw pixels — LVGL is suppressed while video is live.
static void darken_band(uint16_t *fb, int x0, int x1) {
  static uint8_t keep[128];           // 256-x darkening factor per band column
  const int bw = x1 - x0, edge = 12;
  for (int x = 0; x < bw && x < 128; x++) {
    int din = (x < bw - 1 - x) ? x : bw - 1 - x;    // distance to band edge
    int f = din < edge ? 159 * (din + 1) / (edge + 1) : 159;   // 62% max
    keep[x] = (uint8_t)(256 - f - 1);
  }
  for (int y = 0; y < PANEL_H; y++) {
    uint16_t *row = fb + y * PANEL_W;
    for (int x = x0; x < x1; x++) {
      uint16_t c = row[x];
      int k = keep[x - x0];
      int r = (((c >> 11) & 31) * k) >> 8, g = (((c >> 5) & 63) * k) >> 8,
          b = ((c & 31) * k) >> 8;
      row[x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// Text strip on the top band, centered along landscape-x.
// A caption: a title, and under it the second line the state may or may not
// have. Two strips now rather than one two-line bitmap, so each centres on its
// own width — which is what the baked art did inside itself anyway, so the
// result on screen is the same arrangement.
//
// A font's line height already carries its leading, so the gap between the two
// is small on purpose. 2px, not the generator's 8, because the generator was
// spacing bare TrueType pixel sizes with no descent in them.
//
// A title and at most one line under it is the whole budget here, and that is
// a decision, not a limit of this function. A block of five rows explaining
// what a scan can and cannot do was built on this and then taken back out: a
// code that is aimed at decodes in about a second, so the screen a person
// actually gets is a flash of text they cannot finish, over the one view where
// aiming is the job. The SEARCH state looks like dead time on a desktop and is
// not dead time in a hand.
//
// So anything a reader needs SENTENCES for belongs on a screen they are not
// mid task on. What earns a place here is what they need while pointing:
// what the device is looking for, and whether it has found it.
static void draw_osd_strip(uint16_t *fb, int idx) {
  const scan_osd_strip_t *t = osd_title(idx);
  if (!t) return;
  blit_a4(fb, t, STRIP_TOP_PX, (PANEL_H - t->w) / 2, OSD_DIM_FULL);
  const scan_osd_strip_t *s = osd_sub(idx);
  if (s)
    blit_a4(fb, s, STRIP_TOP_PX - t->h - 2, (PANEL_H - s->w) / 2, OSD_DIM_SUB);
}

// The live Shannon estimate as digits, in the free end of the bottom band past
// the bar. The bar says how much has been gathered; the number says how good
// the view is right now, which is the half a filling bar cannot show. A holder
// standing at a blank wall watches the number climb as they turn toward
// something with detail, and the bar speed up with it, which teaches what the
// screen is asking for far faster than any wording would. Anyone tuning
// ENT_FLOOR_X10 gets a figure they can write down without a special build.
//
// The decimal point is a real '.' now. It used to be a 7px square drawn as a
// rectangle, because the baked atlas held digits and no period and adding one
// meant regenerating 1.7MB of art on a Mac.
//
// Spacing is wider than it looks. A composed glyph strip is exactly its advance
// width, where a baked one carried a pixel of padding on each side, so the
// tracking that used to come free from the art has to be asked for here.
#define ENT_TRACK 5
static void draw_ent_digits(uint16_t *fb)
{
    static int disp;                    // eased like the bar, or it is a blur
    disp += (s_ent_meter - disp) / 4;
    int v = disp < 0 ? 0 : disp > 999 ? 999 : disp;
    int hi = v / 100, mid = (v / 10) % 10, lo = v % 10;

    const scan_osd_strip_t *g_hi = osd_digit(hi), *g_mid = osd_digit(mid);
    const scan_osd_strip_t *g_lo = osd_digit(lo), *g_dot = osd_dot();
    if (!g_mid || !g_lo || !g_dot) return;

    const int cx = 98;                  // glyph top row, inside the bottom band
    int w = g_mid->w + ENT_TRACK + g_dot->w + ENT_TRACK + g_lo->w;
    if (hi && g_hi) w += g_hi->w + ENT_TRACK;
    int cy = PANEL_H - 14 - w;          // right aligned to the band's far end

    if (hi && g_hi) {
        blit_a4(fb, g_hi, cx, cy, OSD_DIM_FULL);
        cy += g_hi->w + ENT_TRACK;
    }
    blit_a4(fb, g_mid, cx, cy, OSD_DIM_FULL);
    cy += g_mid->w + ENT_TRACK;
    blit_a4(fb, g_dot, cx, cy, OSD_DIM_FULL);
    cy += g_dot->w + ENT_TRACK;
    blit_a4(fb, g_lo, cx, cy, OSD_DIM_FULL);
}


// "Reading  12 of 34" as one line of real type: the title strip, then live
// counts from the glyph atlas (total may be unknown early — show seen alone).
//
// The atlas rather than one composed string, because these counts change while
// frames are flowing and recomposing would put an allocation on the stream task
// once per part arrival, for a line that is already laid out correctly here.
static void draw_read_line(uint16_t *fb, int seen, int total) {
  const scan_osd_strip_t *strip = osd_title(OSD_READ);
  const scan_osd_strip_t *of = osd_of();
  if (!strip || !of) return;
  if (seen > 99) seen = 99;
  if (total > 99) total = 99;
  int gi[8], n = 0;
  if (seen >= 10) gi[n++] = seen / 10;
  gi[n++] = seen % 10;
  if (total > 0) {
    gi[n++] = -1;                       // narrow space
    gi[n++] = -2;                       // localized word "of"
    gi[n++] = -1;
    if (total >= 10) gi[n++] = total / 10;
    gi[n++] = total % 10;
  }
  int tw = strip->w + 14;
  for (int i = 0; i < n; i++) {
    const scan_osd_strip_t *g = gi[i] == -2 ? of
                              : gi[i] >= 0  ? osd_digit(gi[i]) : NULL;
    tw += gi[i] == -1 ? 10 : g ? g->w + ENT_TRACK : 0;
  }
  int cy = (PANEL_H - tw) / 2;
  blit_a4(fb, strip, STRIP_TOP_PX, cy, OSD_DIM_FULL);
  cy += strip->w + 14;
  for (int i = 0; i < n; i++) {
    if (gi[i] == -1) { cy += 10; continue; }
    const scan_osd_strip_t *g = gi[i] == -2 ? of : osd_digit(gi[i]);
    if (!g) continue;
    blit_a4(fb, g, STRIP_TOP_PX, cy, OSD_DIM_FULL);
    cy += g->w + ENT_TRACK;
  }
}

// Rounded track + inset rounded fill (landscape-horizontal, drawn in panel
// coords: length runs along panel y, thickness along panel x).
#define BAR_INS 4
// The gate tick mark that used to be drawn here went with the entropy meter's
// pass threshold. Both bars this draws are progress now, and progress bars do
// not mark a point partway along themselves.
static void draw_hbar(uint16_t *fb, int fill, uint16_t base) {
  const int cy0 = (PANEL_H - BAR_LEN) / 2;
  const int R = BAR_THICK / 2;
  // track: rounded dark pill
  for (int i = 0; i < BAR_LEN; i++) {
    int dc = i < R ? R - i : i >= BAR_LEN - R ? i - (BAR_LEN - 1 - R) : 0;
    uint16_t *col0 = fb + (cy0 + i) * PANEL_W + BAR_PX0;
    for (int t = 0; t < BAR_THICK; t++) {
      int dt = t - R;
      if (dc && dt * dt + dc * dc > R * R) continue;
      col0[t] = 0x18E3;
    }
  }
  // fill: brighter rounded pill inset in the track, gentle ramp along it
  int r0 = (base >> 11) & 0x1F, g0 = (base >> 5) & 0x3F, b0 = base & 0x1F;
  const int R2 = (BAR_THICK - 2 * BAR_INS) / 2;
  int flen = fill - 2 * BAR_INS;
  if (flen > BAR_LEN - 2 * BAR_INS) flen = BAR_LEN - 2 * BAR_INS;
  if (base && flen > 2 * R2) {
    for (int i = 0; i < flen; i++) {
      int dc = i < R2 ? R2 - i : i >= flen - R2 ? i - (flen - 1 - R2) : 0;
      int boost = i * 10 / BAR_LEN;
      int r = r0 + boost / 2, g = g0 + boost, b = b0 + boost / 2;
      if (r > 31) r = 31;
      if (g > 63) g = 63;
      if (b > 31) b = 31;
      uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
      uint16_t *col0 = fb + (cy0 + BAR_INS + i) * PANEL_W + BAR_PX0 + BAR_INS;
      for (int t = 0; t < BAR_THICK - 2 * BAR_INS; t++) {
        int dt = t - R2;
        if (dc && dt * dt + dc * dc > R2 * R2) continue;
        col0[t] = c;
      }
    }
  }
}

// Scan progress: one rounded segment per QR part as they assemble; a soft
// traveling shimmer while searching; a yellow sliver when a QR is located but
// nothing has decoded yet.
static void draw_scan_bar(uint16_t *fb) {
  int tot = s_scan_total, seen = s_scan_seen;
  bool found = s_scan_found > 0;
  if (found) s_scan_found--;

  const int cy0 = (PANEL_H - BAR_LEN) / 2;
  const int L = BAR_LEN - 2 * BAR_INS, gap = 5;
  int segw = tot > 1 ? (L - gap * (tot - 1)) / tot : 0;
  if (tot > 1 && segw >= 8) {           // segmented: one pill per part
    draw_hbar(fb, 0, 0);                // track only
    const int R2 = (BAR_THICK - 2 * BAR_INS) / 2;
    for (int s = 0; s < tot; s++) {
      uint16_t c = s < seen ? 0x368F : 0x2166;
      int y0 = cy0 + BAR_INS + s * (segw + gap);
      for (int i = 0; i < segw; i++) {
        int dc = i < R2 ? R2 - i : i >= segw - R2 ? i - (segw - 1 - R2) : 0;
        uint16_t *col0 = fb + (y0 + i) * PANEL_W + BAR_PX0 + BAR_INS;
        for (int t = 0; t < BAR_THICK - 2 * BAR_INS; t++) {
          int dt = t - R2;
          if (dc && dt * dt + dc * dc > R2 * R2) continue;
          col0[t] = c;
        }
      }
    }
  } else if (tot > 0 || seen > 0) {     // many-part or unknown-total fallback
    int fill = tot > 0 ? BAR_LEN * seen / tot : BAR_LEN / 8;
    if (fill > BAR_LEN) fill = BAR_LEN;
    draw_hbar(fb, fill, 0x368F);
  } else if (found) {                   // located, nothing read yet
    draw_hbar(fb, BAR_LEN / 10, 0xFF20);
  } else {                              // searching: soft traveling shimmer
    draw_hbar(fb, 0, 0);
    int pos = (int)((s_frames * 5) % (uint32_t)(BAR_LEN + 160)) - 80;
    for (int i = pos - 40; i < pos + 40; i++) {
      if (i < BAR_INS + 4 || i >= BAR_LEN - BAR_INS - 4) continue;
      int d = i - pos;
      int a = (40 - (d < 0 ? -d : d)) / 6;            // 0..6 of 15
      if (a <= 0) continue;
      uint16_t *col0 = fb + (cy0 + i) * PANEL_W + BAR_PX0 + BAR_INS;
      int aa = a * 17;
      for (int t = 0; t < BAR_THICK - 2 * BAR_INS; t++) {
        uint16_t dpx = col0[t];
        int r = (dpx >> 11) & 31, g = (dpx >> 5) & 63, b = dpx & 31;
        r += ((31 - r) * aa) >> 8;
        g += ((63 - g) * aa) >> 8;
        b += ((31 - b) * aa) >> 8;
        col0[t] = (uint16_t)((r << 11) | (g << 5) | b);
      }
    }
  }
}

// stream-task context: un-mirrored half-res grayscale (green channel of RGB565
// is plenty for black/white QR), then decode.
static void scan_decode(const uint8_t *frame, uint32_t w, uint32_t h) {
  s_scan_att++;
  if (!s_quirc) return;
  int qw = 0, qh = 0;
  uint8_t *img = k_quirc_begin(s_quirc, &qw, &qh);
  if (!img || qw != s_scan_w || qh != s_scan_h) return;
  // The CSI DMA wrote this frame straight to PSRAM; the CPU cache may still
  // hold lines from an OLDER frame. Invalidate before reading or the decoder
  // sees a mix of two frames.
  esp_cache_msync((void *)frame, s_cam.buf_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  // WHICH PART OF THE SENSOR TO READ. This used to be, unconditionally, the
  // whole 1280x728 frame halved to 640x364 -- while the preview showed only a
  // 480x728 crop of that same sensor. The decoder's field of view was 2.67x
  // WIDER than what the user was aiming, so a code that looked well framed was
  // a third of the size the decoder saw, and a dense one never resolved. That
  // mismatch, not the optics, is why "bring it closer" was the only thing that
  // worked.
  //
  // Now attempts alternate:
  //   AIMED - exactly the rectangle orient_geometry gives the preview, at 1:1.
  //           What you see is what gets decoded. At default zoom that is
  //           480x728, so a filled code is ~4 px/module at version 25.
  //   WIDE  - the whole sensor, as before, downsampled into the same buffer.
  //           Keeps a code that is outside the brackets readable, and keeps the
  //           half-res path that qr-scan-camera-recipe records as the workhorse
  //           when sensor line artifacts spoil a full-res read.
  //
  // The decoder buffer is fixed at the default crop's size and never resized:
  // re-allocating it on a zoom change is an allocation that can fail inside the
  // scan loop, and a failed image alloc is the hang this project has already
  // paid for once.
  uint32_t sw, sh;
  if (s_scan_att & 1) {
    uint32_t cw, ch, ow, oh;
    float sc;
    if (orient_geometry(w, h, &cw, &ch, &sc, &ow, &oh)) {
      sw = cw; sh = ch;
    } else {
      sw = w; sh = h;
    }
  } else {
    sw = w > SCAN_MAX_DIM ? (uint32_t)SCAN_MAX_DIM : w;
    sh = h > SCAN_MAX_DIM ? (uint32_t)SCAN_MAX_DIM : h;
  }
  if (sw > w) sw = w;
  if (sh > h) sh = h;

  // Centred, matching show_frame's block_offset_{x,y} = (dim - crop) / 2 --
  // if these two ever disagree the guide starts lying again.
  const uint32_t ox = (w - sw) / 2, oy = (h - sh) / 2;
  const uint32_t stepx = (sw << 16) / (uint32_t)qw;   // 16.16, no float, no div in the loop
  const uint32_t stepy = (sh << 16) / (uint32_t)qh;
  const uint16_t *src = (const uint16_t *)frame;
  for (int y = 0; y < qh; y++, img += qw) {
    const uint16_t *row = src + (size_t)(oy + ((uint32_t)y * stepy >> 16)) * w + ox;
    uint32_t fx = 0;
    for (int x = 0; x < qw; x++, fx += stepx)   // reversed write = un-mirror the sensor
      img[qw - 1 - x] = (uint8_t)((row[fx >> 16] >> 3) & 0xFC);
  }
  k_quirc_end(s_quirc, false);
  int cnt = k_quirc_count(s_quirc);
  if (cnt > 0) {
    s_scan_found = 10;                // yellow bar: a QR is in view
    bool cut = false, decoded = false;
    for (int i = 0; i < cnt && s_scan_cb; i++) {
      static k_quirc_result_t res;    // 2.6KB result: keep off the task stack
      k_quirc_error_t err = k_quirc_decode(s_quirc, i, &res);
      // corners are valid for any located grid (vendored k_quirc patch);
      // a corner hugging the frame edge = the QR doesn't fully fit
      const int M = 8;
      for (int c = 0; c < 4; c++)
        if (res.corners[c].x < M || res.corners[c].x >= qw - M ||
            res.corners[c].y < M || res.corners[c].y >= qh - M)
          cut = true;
      // How much of the frame the located code fills, in 1/256ths, for the
      // reticle to close in on. Size only, not position: a centred box needs
      // no knowledge of which way the PPA rotates, while tracking an off-centre
      // code would, and that is not checkable anywhere in this tree.
      //
      // ONLY from an odd attempt. Even attempts decode the whole sensor
      // downsampled while the panel is showing a tighter crop, so a fraction
      // measured there describes a different picture than the one on screen
      // and would close the brackets onto nothing.
      if (i == 0) {
        if (s_scan_att & 1) {
          int minx = res.corners[0].x, maxx = minx;
          int miny = res.corners[0].y, maxy = miny;
          for (int c = 1; c < 4; c++) {
            if (res.corners[c].x < minx) minx = res.corners[c].x;
            if (res.corners[c].x > maxx) maxx = res.corners[c].x;
            if (res.corners[c].y < miny) miny = res.corners[c].y;
            if (res.corners[c].y > maxy) maxy = res.corners[c].y;
          }
          int fw = (maxx - minx) * 256 / qw, fh = (maxy - miny) * 256 / qh;
          bool quarter = (s_orient % 2) == 1;   // 90/270 swap width and height
          s_qr_fill = quarter ? (fh > fw ? fh : fw) : (fw > fh ? fw : fh);
        } else {
          s_qr_fill = 0;
        }
      }
      if (err == K_QUIRC_SUCCESS && res.data.payload_len > 0) {
        decoded = true;
        s_scan_cb((const char *)res.data.payload, (size_t)res.data.payload_len);
      } else if ((s_scan_att % 20) == 0) {
        ESP_LOGI(TAG, "scan: located but %s", k_quirc_strerror(err));
      }
      // `res` is static so the 2.6KB stays off this task's stack, which means
      // it is .bss that outlives the scan. A SeedQR restore puts a full BIP39
      // mnemonic in there, and a passphrase QR puts the passphrase, and both
      // would sit in RAM until the next decode happened to overwrite them --
      // for the rest of the boot if none ever did. The callback has consumed
      // the payload by now, so this is the last moment it is still ours.
      wally_bzero(&res, sizeof res);
    }
    if (decoded || s_scan_seen > 0) {
      s_scan_osd = OSD_READ;
      s_scan_stuck = 0;
    } else if (cut) {
      s_scan_osd = OSD_CUTOFF;        // a real "move back", not a dense code
      s_scan_stuck = 0;
    } else {
      // Located, fully in frame, and still not decoding. Holding steadier is
      // not going to fix that: a whole PSBT in one static QR is version ~25-40,
      // so unless it FILLS the brackets each module lands on only a pixel or
      // two before any optical blur, and quirc wants cleaner edges than that.
      // The finder squares are coarse enough to keep locating regardless, which
      // is why this state can persist indefinitely while looking like progress.
      // Say so, and name the fix: animated QR fragments are ~60 bytes each,
      // a low-version code with fat modules, and they read first time.
      s_scan_osd = (++s_scan_stuck > SCAN_STUCK_FRAMES) ? OSD_STUCK : OSD_SEEN;
    }
  } else if (s_scan_seen == 0) {
    s_scan_osd = OSD_SEARCH;          // keep READING once parts have landed
    s_scan_stuck = 0;
  }
}

// video task context: one PPA op per frame straight into the off-screen panel FB,
// then a zero-copy flip.
static void show_frame(const uint8_t *frame, uint32_t w, uint32_t h) {
  static const ppa_srm_rotation_angle_t rot[4] = {
      PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
      PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270};
  // Paused: drop this frame whole. Returning here is safe because the caller
  // re-queues the V4L2 buffer once show_frame returns, and it is the point that
  // skips BOTH halves of the job — the blit below and the decode at the bottom.
  // Half a pause, picture frozen but the decoder still reading, would be worse
  // than none: the screen would advance with no sign of why.
  if (s_paused) return;
  uint32_t cw, ch, ow, oh;
  float scale;
  if (!orient_geometry(w, h, &cw, &ch, &scale, &ow, &oh)) return;
  // Two-column mode pins framebuffer 0 and never flips: a flip would swap in the
  // buffer LVGL did NOT just paint, so the column beside the video would
  // alternate between the layout and whatever was there a frame ago. Writing the
  // live buffer can tear, but only inside the preview rect, which is video.
  uint16_t *fb = s_vp_on ? s_fb[0] : s_fb[s_fb_wr];
  if (!fb) return;
  if (s_clear_pending > 0) {          // zoom/orientation changed: blank stale bars.
    s_clear_pending--;                // CPU writes land in cache; the scanout reads
    if (s_vp_on) {                    // PSRAM directly -> write back explicitly
      // Only the rect. The rest of the panel is LVGL's and blanking it would
      // erase the column this mode exists to show.
      for (int py = s_vp_y; py < s_vp_y + s_vp_h; py++)
        memset(fb + (size_t)py * PANEL_W + s_vp_x, 0, (size_t)s_vp_w * 2);
      esp_cache_msync(fb + (size_t)s_vp_y * PANEL_W,
                      (size_t)s_vp_h * PANEL_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    } else {
      memset(fb, 0, PANEL_W * PANEL_H * 2);
      esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
  }
  ppa_srm_oper_config_t op = {
      .in = {
          .buffer = frame,
          .pic_w = w,
          .pic_h = h,
          .block_w = cw,
          .block_h = ch,
          .block_offset_x = (w - cw) / 2,
          .block_offset_y = (h - ch) / 2,
          .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .out = {
          .buffer = fb,
          .buffer_size = PANEL_W * PANEL_H * 2,
          .pic_w = PANEL_W,
          .pic_h = PANEL_H,
          // Centered in the preview rect, which defaults to the whole panel, so
          // the legacy letterbox behavior is the same expression.
          .block_offset_x = s_vp_x + (s_vp_w - (int)ow) / 2,
          .block_offset_y = s_vp_y + (s_vp_h - (int)oh) / 2,
          .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .rotation_angle = rot[s_orient % 4],
      .scale_x = scale,
      .scale_y = scale,
      .mirror_x = (s_orient >= 4),
      .mode = PPA_TRANS_MODE_BLOCKING,
  };
  if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) {
    ESP_LOGW(TAG, "PPA blit failed");
    return;
  }
  // The reticle is the exception, and it draws in BOTH modes: it is the only
  // thing on this screen that says the device is looking, its primitives already
  // work in landscape coordinates, and draw_brackets takes its centre and travel
  // from the preview rect, so in two-column mode the whole gesture happens
  // inside the column. Everything else below is TEXT at panel coordinates
  // chosen for a fullscreen preview, and in two-column mode that text is LVGL's
  // job in the column beside the video, where the overlap gate can measure it
  // and the locale tables can translate it.
  if (s_scan_mode) draw_brackets(fb);
  if (!s_vp_on) {
    if (s_scan_mode || s_ent_mode) {    // cinematic bands carry all the chrome
      darken_band(fb, BAND_TOP_X0, BAND_TOP_X1);
      darken_band(fb, BAND_BOT_X0, BAND_BOT_X1);
      blit_a4(fb, osd_title(OSD_CLOSE), 449, 22, OSD_DIM_CLOSE);  // top-left
    }
    if (s_scan_mode) {
      draw_scan_bar(fb);
      int osd = s_scan_osd;
      if (osd == OSD_READ)              // "Reading  12 of 34" in one line of type
        draw_read_line(fb, s_scan_seen, s_scan_total);
      else
        draw_osd_strip(fb, osd);
    }
    if (s_ent_mode) {
      draw_ent_bar(fb);
      draw_ent_digits(fb);                // the same estimate as a figure
      draw_osd_strip(fb, s_ent_accum >= ENT_TARGET_X10 ? OSD_ENT_OK : OSD_ENT_LOW);
    }
    if (!s_scan_mode && !s_ent_mode && !s_proof_mode)  // dev preview only:
      draw_zoom_bar(fb);                // purposeful screens don't need the
                                        // zoom ladder cluttering the video
    if (s_osd_frames > 0) {             // orientation (left) / zoom (right) level
      s_osd_frames--;                   // digits, real type, inset from overscan
      if (s_orient + 1 <= 9)
        blit_a4(fb, osd_digit(s_orient + 1), 430, 66, OSD_DIM_FULL);
      if (s_zoom + 1 <= 9)
        blit_a4(fb, osd_digit(s_zoom + 1), 430, 660, OSD_DIM_FULL);
    }
  }
  if (s_vp_on) {
    // Push just the rect, and do NOT flip: framebuffer 0 is the one being
    // scanned out and the one LVGL is painting the other columns into, so the
    // picture appears in place with no buffer swap.
    esp_cache_msync(fb + (size_t)s_vp_y * PANEL_W,
                    (size_t)s_vp_h * PANEL_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  } else {
    // CPU overlays (bar/digits) sit in cache; push them to PSRAM before scanout
    esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    // draw_bitmap with a panel-owned framebuffer pointer = scanout flip, no copy
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, fb);
    s_fb_wr ^= 1;
  }
  s_frames++;
  // decode AFTER the flip so the preview stays smooth between attempts; the
  // V4L2 buffer is only re-queued once show_frame returns, so `frame` is ours
  if (s_scan_mode && s_quirc && s_scan_cb && (s_frames % SCAN_EVERY) == 0)
    scan_decode(frame, w, h);
  if (s_ent_mode && s_ent_hist && (s_frames % SCAN_EVERY) == 0)
    ent_frame(frame, w, h);
  // Proof capture takes the very NEXT frame, not a sampled one: the owner
  // tapped on what they were seeing. The copy happens after the blit and the
  // pause lands in the same frame, so the picture frozen on the panel is the
  // frame in the buffer — what the owner sees is what gets hashed.
  if (s_proof_mode && s_proof_req && !s_proof_done && s_proof_buf) {
    s_proof_req = false;
    esp_cache_msync((void *)frame, s_cam.buf_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(s_proof_buf, frame, (size_t)w * h * 2);
    s_proof_done = true;
    s_paused = true;
  }
  if ((s_frames % 60) == 0) {
    int64_t dt = esp_timer_get_time() - s_t0;
    ESP_LOGI(TAG, "60 frames in %.1fs (%.1f fps)", dt / 1e6, 60e6 / dt);
    s_t0 = esp_timer_get_time();
  }
}

static void stream_task(void *arg) {
  (void)arg;
  bool err = false;
  while (!s_cam.stop) {
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &b)) {
      if (errno == ETIMEDOUT || errno == EPERM) continue;
      ESP_LOGE(TAG, "DQBUF failed: %s", strerror(errno));
      err = true;
      break;
    }
    if (b.index < CAM_BUF_NUM && (b.flags & V4L2_BUF_FLAG_DONE))
      show_frame(s_cam.buf[b.index], s_cam.w, s_cam.h);
    if (s_cam.stop) break;              // NEVER re-queue after stop: a buffer queued
    if (ioctl(s_cam.fd, VIDIOC_QBUF, &b)) {  // post-STREAMOFF poisons the next start
      if (!s_cam.stop) { ESP_LOGE(TAG, "QBUF failed: %s", strerror(errno)); err = true; }
      break;
    }
  }
  if (err && !s_cam.stop) {
    set_status("CAM: stream died (%s)", strerror(errno));
    s_task_err = true;                  // main loop notices, repaints the UI
  }
  s_cam.task = NULL;
  vTaskDelete(NULL);
}

static bool cam_init(i2c_master_bus_handle_t bus) {
  if (!bus) { set_status("CAM: no I2C bus"); return false; }
  if (!s_panel || !s_fb[0] || !s_fb[1]) { set_status("CAM: no panel FBs"); return false; }

  // Guition demo confirms: camera SCCB shares the touch I2C bus (GPIO7/8)
  if (i2c_master_probe(bus, OV02C10_SCCB_ADDR, 100) == ESP_OK)
    ESP_LOGI(TAG, "OV02C10 SCCB found on touch I2C bus (0x36)");
  else
    ESP_LOGW(TAG, "no SCCB ack at 0x36 on touch bus - trying init anyway");

  esp_video_init_csi_config_t csi = {
      .sccb_config = {.init_sccb = false, .i2c_handle = bus, .freq = 100000},
      .reset_pin = -1,
      .pwdn_pin = -1,
  };
  esp_video_init_config_t cfg = {.csi = &csi};
  s_cam_log[0] = '\0';                  // see cam_log_tap
  s_cam_log_prev = esp_log_set_vprintf(cam_log_tap);
  esp_err_t err = esp_video_init(&cfg);
  esp_log_set_vprintf(s_cam_log_prev);
  if (err != ESP_OK) {
    set_status("CAM: init %s%s%s", esp_err_to_name(err),
               s_cam_log[0] ? " / " : "", s_cam_log);
    return false;
  }

  s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (s_cam.fd < 0) { set_status("CAM: open %s", strerror(errno)); return false; }

  struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  if (ioctl(s_cam.fd, VIDIOC_G_FMT, &f)) { set_status("CAM: G_FMT failed"); return false; }
  s_cam.w = f.fmt.pix.width;
  s_cam.h = f.fmt.pix.height;
  if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(s_cam.fd, VIDIOC_S_FMT, &f)) { set_status("CAM: S_FMT failed"); return false; }
  }

  struct v4l2_requestbuffers req = {.count = CAM_BUF_NUM,
                                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    .memory = V4L2_MEMORY_MMAP};
  if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req)) { set_status("CAM: REQBUFS failed"); return false; }
  for (int i = 0; i < CAM_BUF_NUM; i++) {
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .memory = V4L2_MEMORY_MMAP, .index = i};
    if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &b)) { set_status("CAM: QUERYBUF failed"); return false; }
    s_cam.buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        s_cam.fd, b.m.offset);
    if (s_cam.buf[i] == MAP_FAILED) { set_status("CAM: mmap failed"); return false; }
    s_cam.buf_len = b.length;
  }

  ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
  if (ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) {
    set_status("CAM: PPA client failed");
    return false;
  }

  uint32_t cw, ch, ow, oh;
  float sc;
  if (!orient_geometry(s_cam.w, s_cam.h, &cw, &ch, &sc, &ow, &oh)) {
    set_status("CAM: unsupported frame %ux%u", (unsigned)s_cam.w, (unsigned)s_cam.h);
    return false;
  }

  s_cam.inited = true;
  ESP_LOGI(TAG, "pipeline up: %ux%u RGB565, %d buffers of %u bytes",
           (unsigned)s_cam.w, (unsigned)s_cam.h, CAM_BUF_NUM, (unsigned)s_cam.buf_len);
  return true;
}

static bool prime_buffers(void) {
  for (int i = 0; i < CAM_BUF_NUM; i++) {
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .memory = V4L2_MEMORY_MMAP, .index = i,
                            .length = s_cam.buf_len};
    if (ioctl(s_cam.fd, VIDIOC_QBUF, &b)) return false;
  }
  return true;
}

static bool cam_start(void) {
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  // Compose the overlay's captions here, on the caller's task, before the
  // stream task exists. Every draw below reads these and none of them
  // allocates, so a frame never waits on a heap the display path is competing
  // for. It is also the only moment the language is known to be settled: the
  // camera cannot be up while the settings screen is.
  if (!osd_strips_open()) ESP_LOGW(TAG, "overlay text: nothing composed");
  if (!prime_buffers()) {
    // a stale queued buffer from a previous run: force a clean stop and retry once
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    if (!prime_buffers()) {
      set_status("CAM: QBUF failed");
      osd_strips_close();
      return false;
    }
  }
  if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type)) {
    set_status("CAM: STREAMON %s", strerror(errno));
    osd_strips_close();
    return false;
  }
  s_cam.stop = false;
  s_task_err = false;
  s_clear_pending = 2;                // start from black (letterbox bars)
  s_frames = 0;
  s_t0 = esp_timer_get_time();
  if (xTaskCreatePinnedToCore(stream_task, "camspike", 6144, NULL, 3,
                              &s_cam.task, 1) != pdPASS) {
    set_status("CAM: task create failed");
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    osd_strips_close();
    return false;
  }
  s_cam.streaming = true;
  return true;
}

static void cam_stop(void) {
  s_cam.stop = true;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
  for (int i = 0; i < 50 && s_cam.task; i++) vTaskDelay(pdMS_TO_TICKS(20));
  s_cam.streaming = false;
  // After the join, never before: the strips are what the stream task draws.
  osd_strips_close();
  // Drop any preview rect with the stream that asked for it. A rect left set
  // would confine the NEXT session, including the fullscreen dev preview, to a
  // column of a screen that is no longer on the panel. A pause is dropped here
  // for the same reason: a screen torn down while its help card was open would
  // otherwise hand the next scan a camera that never draws.
  s_paused = false;
  s_vp_on = false;
  s_vp_x = 0; s_vp_y = 0; s_vp_w = PANEL_W; s_vp_h = PANEL_H;
  s_vp_lx = 0; s_vp_ly = 0; s_vp_lw = PANEL_H; s_vp_lh = PANEL_W;
}

bool camera_scan_start(void *bus_v, void (*on_decode)(const char *, size_t)) {
  i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_v;
  if (s_cam.streaming) cam_stop();               // spike preview was live: restart clean
  if (!s_cam.inited && !cam_init(bus)) return false;
  if (!s_quirc) {
    // Size the decoder to the DEFAULT PREVIEW CROP, at 1:1, and never resize
    // it. That is what makes the aimed attempt in scan_decode full-resolution:
    // at zoom L0 the preview shows a 480x728 slice of the sensor, so a code
    // filling the short axis arrives as ~480 px instead of the 240 it would get
    // from the old half-of-the-whole-frame buffer.
    //
    // Fixed, because a zoom change must never trigger an allocation inside the
    // scan loop. k_quirc puts images in PSRAM first (k_malloc_large), so 480x728
    // is affordable; a failed image alloc is not something to risk mid-scan.
    int cw = (int)s_zoom_tab[0][0].bw;
    int ch = (int)s_zoom_tab[0][0].bh;
    if (cw > SCAN_MAX_DIM) cw = SCAN_MAX_DIM;
    if (ch > SCAN_MAX_DIM) ch = SCAN_MAX_DIM;
    if (cw > (int)s_cam.w) cw = (int)s_cam.w;
    if (ch > (int)s_cam.h) ch = (int)s_cam.h;
    s_scan_w = cw;
    s_scan_h = ch;
    s_quirc = k_quirc_new();
    if (!s_quirc || k_quirc_resize(s_quirc, s_scan_w, s_scan_h) != 0) {
      if (s_quirc) { k_quirc_destroy(s_quirc); s_quirc = NULL; }
      set_status("CAM: QR decoder init failed (%dx%d)", s_scan_w, s_scan_h);
      return false;
    }
  }
  s_scan_osd = OSD_SEARCH;
  s_scan_stuck = 0;                                // fresh scan, fresh patience
  s_paused = false;                                // and a camera that draws
  s_scan_seen = 0;
  s_scan_total = 0;
  s_scan_cb = on_decode;
  s_zoom = 0;                                    // widest view = easiest aiming
  s_scan_mode = true;
  if (!cam_start()) {
    s_scan_mode = false;
    s_scan_cb = NULL;
    return false;
  }
  scan_exposure(true);                           // freeze hand shake
  set_status("CAM: scanning %ux%u", (unsigned)s_cam.w, (unsigned)s_cam.h);
  return true;
}

void camera_scan_stop(void) {
  if (!s_scan_mode && !s_cam.streaming) return;
  s_scan_mode = false;
  s_scan_cb = NULL;
  scan_exposure(false);                          // back to the default look
  cam_stop();                                    // waits for the stream task to exit
  if (s_quirc) { k_quirc_destroy(s_quirc); s_quirc = NULL; }
  lv_obj_invalidate(lv_screen_active());         // repaint LVGL over the video
  set_status("CAM: scan stopped");
}

bool camera_entropy_start(void) {
  if (s_cam.streaming) cam_stop();
  if (!s_cam.inited && !cam_init((i2c_master_bus_handle_t)s_bus_saved))
    return false;
  if (!s_ent_hist) {
    s_ent_hist = heap_caps_malloc(65536u * sizeof(uint32_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ent_hist) { set_status("CAM: entropy histogram alloc failed"); return false; }
  }
  s_ent_meter = 0;
  s_ent_accum = 0;
  s_ent_req = false;
  s_ent_done = false;
  // DARK until a frame says otherwise: the screen opens on the honest state
  // rather than on "good, keep going" for the fraction of a second before the
  // first frame lands.
  s_ent_reason = ENT_R_DARK;
  s_ent_prev_ok = false;
  memset(s_ent_prev, 0, sizeof s_ent_prev);
  // A fresh chain per session. Carrying one over would mean a holder who
  // backed out and came in again started part filled, on frames they saw
  // during a visit they abandoned.
  wally_bzero(s_ent_chain, sizeof s_ent_chain);
  wally_bzero(s_ent_trng, sizeof s_ent_trng);
  s_zoom = 0;
  s_ent_mode = true;
  if (!cam_start()) { s_ent_mode = false; return false; }
  set_status("CAM: entropy %ux%u", (unsigned)s_cam.w, (unsigned)s_cam.h);
  return true;
}

void camera_entropy_tap(void) { s_ent_req = true; }

int camera_entropy_progress(void) {
  int p = s_ent_accum * 100 / ENT_TARGET_X10;
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

int camera_entropy_reason(void) { return s_ent_reason; }

// Both sources in ONE call, deliberately. Each is wiped as it is handed over,
// so two separate one-shot accessors would leave the second caller reading a
// zeroed buffer depending on which ran first. One call has no such order.
bool camera_entropy_sources(uint8_t chain_out[32], uint8_t trng_out[32]) {
  if (!s_ent_done) return false;
  memcpy(chain_out, s_ent_hash, 32);
  memcpy(trng_out, s_ent_trng, 32);
  wally_bzero(s_ent_hash, sizeof s_ent_hash);   // seed material: don't linger
  wally_bzero(s_ent_trng, sizeof s_ent_trng);
  s_ent_done = false;
  return true;
}

void camera_entropy_stop(void) {
  if (!s_ent_mode && !s_cam.streaming) return;
  s_ent_mode = false;
  cam_stop();                                 // waits for the stream task, so
  wally_bzero(s_ent_chain, sizeof s_ent_chain);   // nothing is folding into
  wally_bzero(s_ent_sub, sizeof s_ent_sub);       // these while they are wiped
  wally_bzero(s_ent_prev, sizeof s_ent_prev);
  wally_bzero(s_ent_hash, sizeof s_ent_hash);
  wally_bzero(s_ent_trng, sizeof s_ent_trng);
  s_ent_prev_ok = false;
  s_ent_accum = 0;
  if (s_ent_hist) { free(s_ent_hist); s_ent_hist = NULL; }
  lv_obj_invalidate(lv_screen_active());
  set_status("CAM: entropy stopped");
}

// ---- proof mode: CAMERA AUDIT. Contract in camera_spike.h; what the resulting
// file does and does not prove is docs/specs/prove-it.md. ----

bool camera_proof_start(void) {
  if (s_cam.streaming) cam_stop();
  if (!s_cam.inited && !cam_init((i2c_master_bus_handle_t)s_bus_saved))
    return false;
  // The pinned test vector, the owner's recipe and the file format all name
  // one exact size. A sensor negotiating anything else is a surprise to
  // surface, not to adapt to: a proof file of a novel size proves nothing.
  if ((size_t)s_cam.w * s_cam.h * 2 != WPROOF_FRAME_BYTES) {
    set_status("CAM: proof needs %ux%u, sensor gave %ux%u",
               WPROOF_FRAME_W, WPROOF_FRAME_H,
               (unsigned)s_cam.w, (unsigned)s_cam.h);
    return false;
  }
  // The full-frame copy, allocated before the stream starts so the failure a
  // low-memory session hits is this status line, not a dead CAPTURE pill.
  if (!s_proof_buf) {
    s_proof_buf = heap_caps_malloc(WPROOF_FRAME_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_proof_buf) { set_status("CAM: proof frame alloc failed"); return false; }
  }
  s_proof_req = false;
  s_proof_done = false;
  s_zoom = 0;
  s_proof_mode = true;
  if (!cam_start()) { s_proof_mode = false; return false; }
  set_status("CAM: proof %ux%u", (unsigned)s_cam.w, (unsigned)s_cam.h);
  return true;
}

void camera_proof_capture(void) { s_proof_req = true; }

bool camera_proof_done(void) { return s_proof_done; }

const uint8_t *camera_proof_data(size_t *len) {
  if (!s_proof_done || !s_proof_buf) return NULL;
  if (len) *len = WPROOF_FRAME_BYTES;
  return s_proof_buf;
}

void camera_proof_stop(void) {
  if (!s_proof_mode && !s_cam.streaming) return;
  s_proof_mode = false;
  cam_stop();                           // joins the stream task; the frozen
}                                       // copy survives for the SD write

void camera_proof_end(void) {
  s_proof_mode = false;
  if (s_cam.streaming) cam_stop();
  if (s_proof_buf) { free(s_proof_buf); s_proof_buf = NULL; }
  s_proof_done = false;
  s_proof_req = false;
  lv_obj_invalidate(lv_screen_active());
  set_status("CAM: proof ended");
}

bool camera_spike_toggle(lv_obj_t *parent, i2c_master_bus_handle_t bus) {
  (void)parent;                                // display path is direct; no LVGL widget
  if (s_cam.streaming) {                       // ON -> OFF
    cam_stop();
    // LVGL knows nothing of the flipped framebuffers: repaint the whole UI
    lv_obj_invalidate(lv_screen_active());
    set_status("CAM: stopped (%u frames)", (unsigned)s_frames);
    return false;
  }

  if (!s_cam.inited && !cam_init(bus)) return false;
  if (!cam_start()) return false;

  set_status("CAM: LIVE %ux%u direct", (unsigned)s_cam.w, (unsigned)s_cam.h);
  return true;
}

#endif  // !SIMULATOR
