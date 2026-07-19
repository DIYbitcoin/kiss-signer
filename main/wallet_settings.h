// Settings screen (from the home Settings tile). v1: network selection
// (mainnet / testnet) — testnet lets the whole sign flow be exercised with
// worthless coins. Persisted in NVS on device; theme switching lands later.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void wallet_settings_open(lv_obj_t *parent);
bool wallet_settings_active(void);
void wallet_settings_close(void);   // idle auto-lock: drop the screen

// Load persisted settings at boot (device: NVS; sim: no-op). Safe to call once
// before any wallet screen exists.
void wallet_settings_load(void);

// Full-screen language picker (21 locale choices + flags). Persists NVS "lang"
// on pick, then calls picked_cb (which owns rebuilding its screen; the overlay
// is a child of `parent` and dies with it). Also used by first-boot setup.
void wallet_lang_picker_open(lv_obj_t *parent, void (*picked_cb)(void));

// Picker grid slot (0..I18N_LANG_N-1, row-major) of a language: the picker
// displays alphabetically, not in enum order. Used by the sim's scripted walk.
int wallet_lang_pick_slot(int lang);
