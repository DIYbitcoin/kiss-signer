// The board seam. main.c and every screen see a board only through this
// header: the size of the canvas they draw on, the name in the boot log, and
// the handful of calls app_main makes to bring the hardware up. Which board
// answers is decided at build time by CONFIG_KISS_BOARD_* (main/Kconfig.projbuild)
// on the device and by -DKISS_BOARD_WS35 on the desktop; nothing detects a
// board at runtime.
//
// Two boards, two files: board_guition.c and board_ws35.c. Each is compiled
// only into its own image, so neither carries a dead arm of the other, and the
// -Werror lint set compiles every line it ships.
#pragma once
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_KISS_BOARD_WS35) && !defined(KISS_BOARD_WS35)
#define KISS_BOARD_WS35 1
#endif
#endif

#ifdef KISS_BOARD_WS35
#define KISS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5"
// LANDSCAPE, like the Guition: the owner holds both boards the same way. The
// glass is 320x480 portrait; the ST7796 turns the picture in hardware (the
// MADCTL swap board_ws35.c sets), so the canvas is 480x320 and no software
// rotate runs on the way out. The panel numbers stay the glass's own: the
// camera's overlay is laid out in that portrait frame, as on the Guition.
#define SCREEN_W 480
#define SCREEN_H 320
#define KISS_PANEL_W 320
#define KISS_PANEL_H 480
#define KISS_NARROW 1
// The OV5647's frame against this canvas: a rotation index (0..3, quarter
// turns counter-clockwise, the PPA's direction) plus 4 when the preview is
// mirrored, and whether the raw frame arrives mirrored, which the QR decoder
// has to undo because a mirrored code locates and never reads.
//
// Turned a quarter. At 0 the picture reads on the glass as turned a quarter to
// the right, and the vendors predict exactly that: Kern and Waveshare's
// examples show this sensor unturned on the glass's portrait frame (MADCTL
// MX), and this board turns that frame a quarter into landscape (MADCTL MV,
// the same turn the touch mapping in board_ws35.c is proven on), so the
// picture has to turn back by one. Unmirrored: the driver's mode table sets
// the sensor's own mirror bit and Kern reads QR codes from that frame with no
// un-mirror, and a turn cannot change handedness. One quarter from 0 is either
// upright or upside down, so upside down here means 3. Upright with text
// reading backwards means a raw mirror of 1 with 5 or 7: which of the two
// depends on whether the PPA mirrors before or after it turns, and nothing
// documents that. The camera logs both when it starts.
#define KISS_CAM_ORIENT 1
#define KISS_CAM_RAW_MIRRORED 0
#else
#define KISS_BOARD_NAME "Guition JC4880P443C"
// LOGICAL UI canvas: the whole game is LANDSCAPE. The device reaches this via
// a one-time boot rotation (the flush in board_guition.c turns each region
// 90 degrees into the 480x800 panel); the sim creates an 800x480 display
// directly. All UI, gameplay and art is authored in these coordinates.
#define SCREEN_W 800
#define SCREEN_H 480
#define KISS_PANEL_W 480
#define KISS_PANEL_H 800
#define KISS_NARROW 0
// The OV02C10 reads out mirrored, and rot0 plus the mirror is upright in panel
// space: the orientation finder's result (camera_spike.c has its history).
#define KISS_CAM_ORIENT 4
#define KISS_CAM_RAW_MIRRORED 1
#endif

// THE UI IS DRAWN ONCE, ON THE WIDE CANVAS. Every length in the screens and
// the kit is written for 800x480, and a board that is smaller scales it at
// compile time: SX for an x or a width, SY for a y or a height. On the wide
// board both fold to the number itself, so nothing there can move by a pixel;
// on the 3.5in they are 3/5 and 2/3. Integer arithmetic, floor: a position and
// a width scaled apart can disagree with their sum by one pixel, which is why
// the kit's right edges are computed from the scaled parts, never scaled as a
// sum. Fonts do not scale by this; the composites in kiss_theme.c are set at
// three fifths on a narrow board, a rung chosen per size rather than computed.
#define KISS_DESIGN_W 800
#define KISS_DESIGN_H 480
#define SX(v) ((v) * SCREEN_W / KISS_DESIGN_W)
#define SY(v) ((v) * SCREEN_H / KISS_DESIGN_H)

// The touch read is the platform seam: the device reads its controller, the
// simulator feeds scripted input.
//
// TWO ENTRY POINTS, and they ask two different questions of the same cached
// sample (main/kiss_touch.h). main.c's game_tick classifies taps itself, so it
// is handed the EDGES a slow pass slept through -- one per call, each at the
// point its own contact had. kiss_ui.c's pointer indev builds its own presses,
// clicks and gestures out of a LEVEL it is shown every pass, so it is handed
// exactly that and no history: a replayed edge reaches it as a click at a place
// the finger has already left. One function cannot answer both questions, which
// is why the pair is here instead of one name. Both simulators serve them the
// same way the board does.
bool platform_read_touch(int *x, int *y);      // main.c: the game and the collector
bool platform_read_touch_ui(int *x, int *y);   // kiss_ui.c: the LVGL pointer indev

// ---- UPSIDE DOWN -------------------------------------------------------
// One runtime truth for the three surfaces that have to turn TOGETHER: the
// picture on the glass, the touch map, and the camera's preview with its
// overlays. Turning one without the others is the failure that matters --
// the unlock is a drawn WORD and its recogniser is deliberately
// orientation-sensitive (kiss_gword.c), so a display flipped without its
// touch is an owner whose enrolled word stops matching, on a device with no
// keyboard to fall back to.
//
// Neither board has a motion sensor, so nothing here is automatic: it is a
// control the owner taps when they want the cable coming out of the other
// side, and the byte behind it is remembered the way the theme's is.
//
// Declared HERE, beside platform_read_touch, because that is already the one
// seam this header declares for the device and the desktop both, and the
// touch reader is one of the three things that turns. Each board applies the
// flip at the SAME seam it applies its existing quarter turn -- the
// controller's address map on the 3.5in, the rotating flush on the 4.3in --
// so the two compose by construction rather than by arithmetic written twice.
//
// `repaint` is false at boot, where nothing has been drawn yet and the first
// frame is still on its way, and true from the control, where the glass is
// already holding a picture that has just become the wrong way up.
bool kiss_flip_get(void);
void kiss_flip_set(bool on, bool repaint);

#ifdef ESP_PLATFORM
#include "lvgl.h"
#include "driver/i2c_master.h"
// In the order app_main calls them. The radio hold comes first, before
// anything else runs, so the window in which the C6 could execute its
// factory firmware is as small as the boot ROM leaves it.
void kiss_board_radio_hold(void);
void kiss_board_log_info(void);
lv_display_t *kiss_board_display_start(void);
void kiss_board_backlight_on(void);
void kiss_board_touch_start(void);
// ...and then the sampler over it, which is where both readers above get their
// answers from (main/kiss_touch.c). After touch_start, because it needs the
// controller handle to exist, and before build_game, because the first LVGL
// pass already reads the seam.
void kiss_touch_start(void);
// ONE raw read of the controller, mapped to canvas coordinates, board private
// in everything but linkage: kiss_touch.c's sampler is its only caller, and
// keeping it the only caller is the fix. Each board's flip lives in here or in
// the driver flags behind it, so what comes back is what the owner is looking
// at either way up.
bool kiss_board_touch_point(int *x, int *y);
// Did the touch controller answer at init. It is one of the four gates that
// decide whether a freshly installed image gets to keep its slot.
bool kiss_board_touch_ok(void);
// The I2C bus the touch controller lives on; the camera's SCCB shares it.
i2c_master_bus_handle_t kiss_board_i2c_bus(void);
#ifdef KISS_BOARD_WS35
// The one door onto the SPI panel, shared by LVGL and the camera. x2 and y2
// are exclusive, esp_lcd's convention, and they are canvas coordinates: the
// controller turns the glass, so a canvas rect is a panel rect. `px` is
// already in the panel's byte order and written back from the cache. LVGL's
// calls return at once and the DMA-done callback hands the buffer back; a
// camera call (cam) returns when the transfer is complete, so its buffer is
// free again. False when the driver refused the transfer: nothing was sent
// and no completion will follow, so an LVGL caller owes its own flush_ready.
bool kiss_board_blit(int x1, int y1, int x2, int y2, const void *px, bool cam);
#endif
#endif
