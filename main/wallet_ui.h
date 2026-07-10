// KISS wallet login UI (step 3): passphrase keyboard -> fingerprint -> home.
// Platform-independent LVGL; compiled in both device and sim builds.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Open the login flow (passphrase QWERTY). unlocked_cb runs after the user
// confirms the fingerprint screen. CANCEL returns silently to the game.
void wallet_login_open(void (*unlocked_cb)(void));

// Setup variant (first login after the seed wizard): the passphrase must be
// typed TWICE — a typo here is an unreproducible wallet later (spec safety net).
void wallet_login_open_setup(void (*unlocked_cb)(void));

// True while any login screen is on top (game must ignore touch meanwhile).
bool wallet_ui_active(void);

// Fingerprint of the most recently unlocked wallet (4 bytes).
void wallet_ui_last_fp(uint8_t out[4]);

// Register the LVGL pointer indev if not yet present (the setup wizard can run
// before the first login and needs touch too).
void wallet_ui_ensure_indev(void);

// Fill a label with the build-identity line (release: version + commit and the
// flash-encryption state read from the chip; dev: amber warning banner).
// Shared by the Settings footer and the wallet home corner.
void wallet_build_id_apply(lv_obj_t *lbl);
