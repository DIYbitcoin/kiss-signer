// KISS Signer login UI (step 3): passphrase keyboard -> fingerprint -> home.
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
// True after the words + exact passphrase were rehearsed on this device
// (BACKUP VERIFY flow), false until then. Settings' RECOVERY WORDS row uses
// this to show "paper never checked" instead of pretending everything is fine.
bool wallet_ui_backup_verified(void);
// Record it without going through a login screen. Only the decoy unlock needs
// this: it opens with an empty passphrase and never draws the keyboard.
void wallet_ui_set_last_fp(const uint8_t fp[4]);

// Register the LVGL pointer indev if not yet present (the setup wizard can run
// before the first login and needs touch too).
void wallet_ui_ensure_indev(void);
#ifdef SIMULATOR
void wallet_ui_drop_indev_for_test(void);   // put the process back to power-on
#endif

// Create the build-identity line at (x,y): version + commit in calm ink, plus
// an amber warning while the chip reports flash encryption off (dev builds get
// the amber dev banner). Shared by the Settings footer and wallet home corner.
//
// with_radio adds the C6 radio-reset readback. Only Settings passes true: it
// is a diagnostic for people who already know what the C6 is, and the home
// corner is not where you go looking for one.
// stacked: version on its own row with the status facts under it (Settings,
// where this sits beside a row of buttons). false puts everything on ONE line,
// which is what the home corner wants: two facts along an empty bottom edge.
lv_obj_t *wallet_build_id_make(lv_obj_t *parent, int x, int y, bool with_radio,
                               bool stacked);
void wallet_build_id_restyle(lv_obj_t *version_label);
