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

#ifndef SIMULATOR
#include "sdkconfig.h"
#if defined(CONFIG_KISS_BOARD_WS35) && !defined(KISS_BOARD_WS35)
#define KISS_BOARD_WS35 1
#endif
#endif

#ifdef KISS_BOARD_WS35
#define KISS_BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5"
// The logical canvas IS the physical panel: portrait, no rotation.
#define SCREEN_W 320
#define SCREEN_H 480
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

// The touch read is the platform seam: the device reads its controller, the
// simulator feeds scripted input.
bool platform_read_touch(int *x, int *y);

#ifndef SIMULATOR
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
#endif
