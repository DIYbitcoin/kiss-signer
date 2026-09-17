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
// rotate runs on the way out. The panel numbers stay the glass's own, for the
// camera transport.
#define SCREEN_W 480
#define SCREEN_H 320
#define KISS_PANEL_W 320
#define KISS_PANEL_H 480
#define KISS_NARROW 1
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
bool platform_read_touch(int *x, int *y);

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
// Did the touch controller answer at init. It is one of the four gates that
// decide whether a freshly installed image gets to keep its slot.
bool kiss_board_touch_ok(void);
// The I2C bus the touch controller lives on; the camera's SCCB shares it.
i2c_master_bus_handle_t kiss_board_i2c_bus(void);
#ifdef KISS_BOARD_WS35
// The one door onto the SPI panel, shared by LVGL and the camera. x2 and y2
// are exclusive, esp_lcd's convention. `px` is already in the panel's byte
// order and written back from the cache. LVGL's calls return at once and the
// DMA-done callback hands the buffer back; a camera call (cam) returns when
// the transfer is complete, so its buffer is free again.
void kiss_board_blit(int x1, int y1, int x2, int y2, const void *px, bool cam);
#endif
#endif
