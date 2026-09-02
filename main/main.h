#pragma once
// What main.c exports, in one place.
//
// It had none, so every caller wrote its own extern: kiss_settings.c declared
// three, kiss_setup.c and kiss_ui.c one each, and the simulator harness three
// more. Six copies of a signature with nothing comparing them -- and a copy
// that drifts from the definition is not a compile error, it is a call through
// the wrong prototype at run time.
//
// -Wmissing-prototypes on the device build is what asks for this file: a
// non-static definition the compiler has never seen declared is the half of
// that pair it can see.
#include <stdbool.h>
#include "lvgl.h"

void app_main(void);

// The home screen and the game behind it. build_game is not static because
// the simulator harness builds the same screen the device does.
void build_game(void);

// Re-sync the home badges -- TESTNET, SD storage -- after Settings changes
// something behind them.
void kiss_home_refresh(void);

// Closes whatever screen is open and runs the seed wizard, then the type-twice
// login. This is the whole replacement hand-off: Settings reaches CREATE NEW
// and RESTORE through it, and so does the missing-card recovery action, which
// must not treat restored seed words as an ordinary one-passphrase unlock.
void kiss_begin_setup(void);

// After a wipe, lock straight back to the game. It wipes nothing itself -- the
// keys are already gone, and the next KISS unlock lands in first-boot setup.
void kiss_wiped_lock(void);

// The banner shown when the touch controller never answered.
lv_obj_t *kiss_touch_dead_banner(lv_obj_t *parent);

// Reads back the C6 reset pad (GPIO54).
bool radio_is_held(void);
