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
#include "wallet_crypto.h"   // wallet_entropy_mix: camera hash + TRNG -> seed

#include "k_quirc.h"
#include "i18n.h"
#include "scan_osd.h"

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

// ---- step 7 entropy mode: live Shannon estimate over the raw RGB565 frame
// (full 65536-bin histogram, Kern's method + threshold); the SEED entropy is
// SHA256(SHA256(frame) || hardware TRNG) — Shannon is only the quality gate.
//
// What this number is not. A histogram is order blind: shuffle every pixel in
// the frame and the estimate does not move. So any FIXED pattern the optics
// and sensor impose on every frame of every device widens the histogram and
// raises the reading without adding one bit anybody could not predict. Two of
// those were real here. The sensor's black pedestal is now zeroed at source.
// Lens vignetting is not corrected at all: ov02c10_default.json carries no lsc
// section, so the ISP's shading block is never programmed, and correcting it
// needs per lens coefficients measured on a flat field that we do not have.
//
// The seed is not weakened by any of this. wallet_entropy_mix folds the frame
// hash together with esp_fill_random below, so a wholly predictable scene
// still leaves the seed no worse than the hardware TRNG alone. What is
// overstated is the GATE: it can read 6.0 bits off a scene carrying less, and
// tell the holder they are ready when they are standing at a blank wall.
#define ENT_THRESH_X10 60               // 6.0 bits minimum, same as Kern
// Chosen against the old image: fixed exposure, no white balance, pedestal
// intact. Every one of those inflated the reading, so the honest expectation
// is that corrected frames measure LOWER and this constant has to come down
// to keep the same scenes passing. Measure on device against a blank wall and
// against gravel before moving it. A threshold left calibrated against a bug
// is a threshold that means nothing.
static void *s_bus_saved;
static volatile bool s_ent_mode;
static volatile int s_ent_meter;        // Shannon estimate, bits x10
static volatile bool s_ent_req;         // UI tapped: capture next good frame
static volatile bool s_ent_done;        // s_ent_hash is ready
static uint8_t s_ent_hash[32];
static uint32_t *s_ent_hist;            // 256KB histogram, PSRAM

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
  if (s_ent_req) {
    s_ent_req = false;
    if (s_ent_meter >= ENT_THRESH_X10 && !s_ent_done &&
        wally_sha256((const unsigned char *)frame, n * 2,
                     s_ent_hash, sizeof s_ent_hash) == WALLY_OK) {
      // mix in the chip's hardware TRNG: seed = SHA256(frame_hash || trng), so
      // a predictable scene can't weaken the seed below the TRNG and a weak
      // TRNG is still covered by the photo (belt and braces, invisible to UX)
      uint8_t trng[32];
      esp_fill_random(trng, sizeof trng);
      if (wallet_entropy_mix(s_ent_hash, trng, s_ent_hash) == 0)
        s_ent_done = true;
      wally_bzero(trng, sizeof trng);
    }                                   // sub-threshold taps just do nothing —
  }                                     // the amber bar already says why
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
static void draw_hbar(uint16_t *fb, int fill, int gate, uint16_t base);

// Entropy meter: amber while below the gate (tick mark = 6.0 bits), green
// when a tap would be accepted. Full bar = 8.0 bits. The displayed fill EASES
// toward the live estimate so the bar glides instead of twitching.
static void draw_ent_bar(uint16_t *fb) {
  static int disp;
  int target = BAR_LEN * s_ent_meter / 80;
  if (target > BAR_LEN) target = BAR_LEN;
  disp += (target - disp) / 4;
  if (disp < 0) disp = 0;
  draw_hbar(fb, disp, BAR_LEN * ENT_THRESH_X10 / 80,
            s_ent_meter >= ENT_THRESH_X10 ? 0x368F : 0xF5C9);
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

// Alpha-blit a baked 4-bit-alpha strip (anti-aliased text from scan_osd.py),
// upright in landscape: (ux,uy) -> panel px = cx - uy, py = cy + ux. cx is the
// panel x of the strip's FIRST text row; the strip grows toward screen-bottom.
static void blit_a4(uint16_t *fb, const scan_osd_strip_t *s, int cx, int cy) {
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
      int aa = a * 17;                        // 0..255
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
#define BRK_SWEEP_F  45     // 1.5s: one pass of the scan line
#define BRK_HALF     225    // half the guide box, when nothing is located
#define BRK_HALF_MIN 95     // never close tighter than this, however small the code

// Where the brackets are now, eased toward where the located code says they
// should be. Eased rather than snapped because the fill estimate jitters by a
// few percent between frames as the finder squares are re-located, and a box
// that twitched would look like a fault rather than a lock.
static int s_brk_half = BRK_HALF;

static void draw_brackets(uint16_t *fb) {
  const int cx = 400, cy = 240, arm = 44, t = 4;
  bool found = s_scan_found > 0;

  // Closing in on the code is the whole "it found it" gesture. s_qr_fill is
  // how much of the frame the code occupies; the guide follows it down, with a
  // floor so a distant code does not shrink the guide into a dot the user then
  // cannot aim with.
  int want = BRK_HALF;
  if (found && s_qr_fill > 0) {
    want = BRK_HALF * s_qr_fill / 256 + arm / 2;
    if (want < BRK_HALF_MIN) want = BRK_HALF_MIN;
    if (want > BRK_HALF) want = BRK_HALF;
  }
  s_brk_half += (want - s_brk_half) / 4;          // ~4 frames to settle
  if (s_brk_half > BRK_HALF) s_brk_half = BRK_HALF;
  if (s_brk_half < BRK_HALF_MIN) s_brk_half = BRK_HALF_MIN;
  const int half = s_brk_half;
  // Located: solid, and green rather than white. Green is the wallet's
  // status-OK colour everywhere else on the device, and this is the only
  // moment on this screen where something definite has happened.
  // Searching: a slow breath between 5 and 11, so the guide reads as live
  // rather than as a static overlay somebody forgot to remove.
  int ph = (int)(s_frames % BRK_BREATH_F);
  int tri = ph < BRK_BREATH_F / 2 ? ph : BRK_BREATH_F - ph;   // 0..30..0
  uint8_t a = found ? 15 : (uint8_t)(5 + tri * 6 / (BRK_BREATH_F / 2));
  for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2) {
      int x = cx + sx * half, y = cy + sy * half;
      if (found) {
        lrect_blend_rgb(fb, sx < 0 ? x : x - arm, y - t / 2, arm, t, a, 6, 52, 15);
        lrect_blend_rgb(fb, x - t / 2, sy < 0 ? y : y - arm, t, arm, a, 6, 52, 15);
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
    const int in = 22, arm2 = 26, t2 = 2;
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

  // A line sweeping down the guide while nothing is located. It exists to say
  // "still looking" during the state that otherwise has no motion at all: a
  // dense QR can sit in frame for many seconds while quirc keeps failing, and
  // a completely still screen reads as a hung device.
  //
  // It stops the moment a code is found, so motion means searching and
  // stillness means located -- the opposite of the two being decoration.
  //
  // Drawn INTO the display framebuffer only, like the brackets and the bands:
  // scan_decode reads the raw sensor frame, so nothing the sweep crosses is
  // hidden from the decoder.
  if (!found) {
    int sp = (int)(s_frames % BRK_SWEEP_F);
    int sy = cy - half + sp * (2 * half) / BRK_SWEEP_F;
    // Dim at the ends of the travel and brightest through the middle, so it
    // reads as a pass across the guide rather than a bar that teleports back.
    int st = sp < BRK_SWEEP_F / 2 ? sp : BRK_SWEEP_F - sp;
    uint8_t sa = (uint8_t)(2 + st * 6 / (BRK_SWEEP_F / 2));
    lrect_blend(fb, cx - half, sy, 2 * half, 3, sa);
  }
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
static void draw_osd_strip(uint16_t *fb, int idx) {
  if (idx < 0 || idx >= SCAN_OSD_N) return;
  int lang = i18n_get_lang();
  if (lang < 0 || lang >= I18N_LANG_N) lang = I18N_EN;
  const scan_osd_strip_t *s = &scan_osd[lang][idx];
  blit_a4(fb, s, STRIP_TOP_PX, (PANEL_H - s->w) / 2);
}

// The live Shannon estimate as digits, in the free end of the bottom band past
// the bar. The bar on its own can only say "over" or "under" the gate, and both
// of the people this screen serves want more than that. A holder standing at a
// blank wall gets to watch the number climb as they turn toward something with
// detail, which teaches what the gate is actually asking for far faster than
// any wording would. Anyone recalibrating ENT_THRESH_X10 gets a figure they can
// write down without a special build.
//
// Reuses the baked 0-9 glyphs. The decimal point is a plain square because
// scan_osd bakes digits and no period.
static void draw_ent_digits(uint16_t *fb)
{
    static int disp;                    // eased like the bar, or it is a blur
    disp += (s_ent_meter - disp) / 4;
    int v = disp < 0 ? 0 : disp > 999 ? 999 : disp;
    int hi = v / 100, mid = (v / 10) % 10, lo = v % 10;

    const int cx = 98;                  // glyph top row, inside the bottom band
    const int gap = 3, dot = 7;
    int w = scan_osd_glyph[mid].w + gap + dot + gap + scan_osd_glyph[lo].w;
    if (hi) w += scan_osd_glyph[hi].w + gap;
    int cy = PANEL_H - 14 - w;          // right aligned to the band's far end

    if (hi) {
        blit_a4(fb, &scan_osd_glyph[hi], cx, cy);
        cy += scan_osd_glyph[hi].w + gap;
    }
    blit_a4(fb, &scan_osd_glyph[mid], cx, cy);
    cy += scan_osd_glyph[mid].w + gap;
    lrect_blend(fb, cy, (PANEL_W - 1) - (cx - 37 + dot), dot, dot, 15);
    cy += dot + gap;
    blit_a4(fb, &scan_osd_glyph[lo], cx, cy);
}


// "Reading  12 of 34" as one line of real type: the baked strip, then live
// counts from the glyph atlas (total may be unknown early — show seen alone).
static void draw_read_line(uint16_t *fb, int seen, int total) {
  int lang = i18n_get_lang();
  if (lang < 0 || lang >= I18N_LANG_N) lang = I18N_EN;
  const scan_osd_strip_t *strip = &scan_osd[lang][OSD_READ];
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
  for (int i = 0; i < n; i++)
    tw += gi[i] == -1 ? 10
         : gi[i] == -2 ? scan_osd_of[lang].w + 2
                       : scan_osd_glyph[gi[i]].w + 2;
  int cy = (PANEL_H - tw) / 2;
  blit_a4(fb, strip, STRIP_TOP_PX, cy);
  cy += strip->w + 14;
  for (int i = 0; i < n; i++) {
    if (gi[i] == -1) { cy += 10; continue; }
    const scan_osd_strip_t *g = gi[i] == -2 ? &scan_osd_of[lang]
                                             : &scan_osd_glyph[gi[i]];
    blit_a4(fb, g, STRIP_TOP_PX, cy);
    cy += g->w + 2;
  }
}

// Rounded track + inset rounded fill (landscape-horizontal, drawn in panel
// coords: length runs along panel y, thickness along panel x).
#define BAR_INS 4
static void draw_hbar(uint16_t *fb, int fill, int gate, uint16_t base) {
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
  if (gate >= 0) {                      // slim white tick, slightly proud
    for (int i = gate - 1; i <= gate; i++) {
      if (i < 0 || i >= BAR_LEN) continue;
      for (int t = -4; t < BAR_THICK + 4; t++) {
        int px = BAR_PX0 + t, py = cy0 + i;
        if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H)
          fb[py * PANEL_W + px] = 0xE73C;
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
    draw_hbar(fb, 0, -1, 0);            // track only
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
    draw_hbar(fb, fill, -1, 0x368F);
  } else if (found) {                   // located, nothing read yet
    draw_hbar(fb, BAR_LEN / 10, -1, 0xFF20);
  } else {                              // searching: soft traveling shimmer
    draw_hbar(fb, 0, -1, 0);
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
  uint32_t cw, ch, ow, oh;
  float scale;
  if (!orient_geometry(w, h, &cw, &ch, &scale, &ow, &oh)) return;
  uint16_t *fb = s_fb[s_fb_wr];
  if (!fb) return;
  if (s_clear_pending > 0) {          // zoom/orientation changed: blank stale bars.
    s_clear_pending--;                // CPU writes land in cache; the scanout reads
    memset(fb, 0, PANEL_W * PANEL_H * 2);   // PSRAM directly -> write back explicitly
    esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
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
          .block_offset_x = (PANEL_W - ow) / 2,   // center; bars at letterbox levels
          .block_offset_y = (PANEL_H - oh) / 2,
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
  if (s_scan_mode || s_ent_mode) {    // cinematic bands carry all the chrome
    darken_band(fb, BAND_TOP_X0, BAND_TOP_X1);
    darken_band(fb, BAND_BOT_X0, BAND_BOT_X1);
    int lang = i18n_get_lang();
    if (lang < 0 || lang >= I18N_LANG_N) lang = I18N_EN;
    blit_a4(fb, &scan_osd[lang][OSD_CLOSE], 449, 22); // localized close, top-left
  }
  if (s_scan_mode) {
    draw_brackets(fb);                // viewfinder corners (solid once located)
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
    draw_osd_strip(fb, s_ent_meter >= ENT_THRESH_X10 ? OSD_ENT_OK : OSD_ENT_LOW);
  }
  if (!s_scan_mode && !s_ent_mode)    // dev preview only: scan/entropy screens
    draw_zoom_bar(fb);                // don't need the zoom ladder cluttering
  if (s_osd_frames > 0) {             // orientation (left) / zoom (right) level
    s_osd_frames--;                   // digits, real type, inset from overscan
    if (s_orient + 1 <= 9) blit_a4(fb, &scan_osd_glyph[s_orient + 1], 430, 66);
    if (s_zoom + 1 <= 9)   blit_a4(fb, &scan_osd_glyph[s_zoom + 1], 430, 660);
  }
  // CPU overlays (bar/digits) sit in cache; push them to PSRAM before scanout
  esp_cache_msync(fb, PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  // draw_bitmap with a panel-owned framebuffer pointer = scanout flip, no copy
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, fb);
  s_fb_wr ^= 1;
  s_frames++;
  // decode AFTER the flip so the preview stays smooth between attempts; the
  // V4L2 buffer is only re-queued once show_frame returns, so `frame` is ours
  if (s_scan_mode && s_quirc && s_scan_cb && (s_frames % SCAN_EVERY) == 0)
    scan_decode(frame, w, h);
  if (s_ent_mode && s_ent_hist && (s_frames % SCAN_EVERY) == 0)
    ent_frame(frame, w, h);
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
  if (!prime_buffers()) {
    // a stale queued buffer from a previous run: force a clean stop and retry once
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    if (!prime_buffers()) { set_status("CAM: QBUF failed"); return false; }
  }
  if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type)) {
    set_status("CAM: STREAMON %s", strerror(errno));
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
  s_ent_req = false;
  s_ent_done = false;
  s_zoom = 0;
  s_ent_mode = true;
  if (!cam_start()) { s_ent_mode = false; return false; }
  set_status("CAM: entropy %ux%u", (unsigned)s_cam.w, (unsigned)s_cam.h);
  return true;
}

void camera_entropy_tap(void) { s_ent_req = true; }

bool camera_entropy_result(uint8_t out[32]) {
  if (!s_ent_done) return false;
  memcpy(out, s_ent_hash, 32);
  memset(s_ent_hash, 0, sizeof s_ent_hash);   // seed material: don't linger
  s_ent_done = false;
  return true;
}

void camera_entropy_stop(void) {
  if (!s_ent_mode && !s_cam.streaming) return;
  s_ent_mode = false;
  cam_stop();
  if (s_ent_hist) { free(s_ent_hist); s_ent_hist = NULL; }
  lv_obj_invalidate(lv_screen_active());
  set_status("CAM: entropy stopped");
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
