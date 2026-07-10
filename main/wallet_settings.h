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
