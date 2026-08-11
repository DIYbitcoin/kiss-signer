// The panel itself: its backlight, and blacking both framebuffers.
//
// Three functions with no file of their own, because they cannot have one.
// They reach straight into the MIPI DSI panel handle and the two framebuffers,
// and main.c is where those live -- so main.c defines them on both sides, real
// PWM on the device and no-ops on the desktop. See the comments there for why
// a flash write and this panel cannot both have the cache.
//
// They were declared in kiss_theme.h until the modules were renamed to kiss_*.
// That was survivable while the kit was called wallet_theme and the prefix said
// they came from somewhere else; now kiss_theme.h reads as the file that owns
// them, and it does not. A header naming the panel says where they really come
// from, and costs one include in the one module that calls them.
#pragma once

// Backlight on or off, so a screen that cannot be drawn is dark rather than
// torn.
void kiss_backlight_set(int on);

// Brightness 0..100 as a progress indicator, for the one window where nothing
// can be drawn. Blacks both framebuffers first or the light reveals tearing.
void kiss_backlight_level(int pct);
void kiss_panel_black(void);
