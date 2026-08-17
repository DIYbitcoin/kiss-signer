// KISS Signer login UI (step 3): passphrase keyboard -> fingerprint -> home.
// Platform-independent LVGL; compiled in both device and sim builds.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Open the login flow (passphrase QWERTY). unlocked_cb runs after the user
// confirms the fingerprint screen. CANCEL returns silently to the game.
void kiss_login_open(void (*unlocked_cb)(void));

// Setup variant (first login after the seed wizard): the passphrase must be
// typed TWICE — a typo here is an unreproducible wallet later (spec safety net).
void kiss_login_open_setup(void (*unlocked_cb)(void));

// Same flow for words the owner ALREADY OWNS, minus the two checks that belong
// only to a passphrase being invented: typed once, and no weak-strength gate.
// The fingerprint screen that follows is the real check on a restore -- it is
// the one thing that can tell a correctly re-entered passphrase from a
// consistently mistyped one, which type-twice cannot.
void kiss_login_open_restore(void (*unlocked_cb)(void));
// Add-later variant (from Settings): same type-twice + fingerprint reveal, but
// nothing is committed — there is no staged seed, the passphrase only derives
// the session. CANCEL returns to the caller's callback rather than the game.
void kiss_login_open_add_later(void (*unlocked_cb)(void));
// True while any login screen is on top (game must ignore touch meanwhile).
bool kiss_ui_active(void);

// The same keyboard, borrowed to collect a KEF backup password (never a
// passphrase: no wallet, no session). create = type twice with the strength
// meter; otherwise one entry. on_check runs on OK and returns 0 or -1; -1
// keeps the keyboard up with one vague failure, 0 folds it and then calls
// on_done. CANCEL folds it and calls on_cancel.
void kiss_ui_kef_pass_open(bool create,
                           int (*on_check)(const char *pass, size_t len),
                           void (*on_done)(void), void (*on_cancel)(void));

// True while the commit-failed RECOVER screen -- or the words screen it
// opens -- is up. Its own row in main.c's SCREENS[]: owns the touch, holds
// the idle lock off, and registers no close. The staged seed it names may be
// the last copy anywhere, so nothing is allowed to tear it down.
bool kiss_ui_recover_active(void);

// The login row's idle-deadline gate: the login's 120-second wipe must not
// fire while the RECOVER screen holds the retry passphrase in s_pass, or
// TRY AGAIN would open an empty-passphrase wallet under the stale fingerprint.
bool kiss_ui_login_deadline_active(void);

// Fingerprint of the most recently unlocked wallet (4 bytes).
void kiss_ui_last_fp(uint8_t out[4]);
// True after the words + exact passphrase were rehearsed in THIS session
// (BACKUP VERIFY flow). The setup warning screen asks this one: it is deciding
// whether to let the owner walk away, so an older answer will not do.
bool kiss_ui_backup_verified(void);
// True if this wallet's paper has ever been proven against this device, on any
// boot (see kiss_backup.h). Settings' RECOVERY WORDS row uses this to show
// "paper never checked" instead of pretending everything is fine.
bool kiss_ui_backup_checked(void);
// Record it without going through a login screen. Only the decoy unlock needs
// this: it opens with an empty passphrase and never draws the keyboard.
void kiss_ui_set_last_fp(const uint8_t fp[4]);

// Forget which keys were open. Call wherever the session closes: the
// fingerprint outlives kiss_session_close otherwise, and a decoy session that
// cannot derive its own would show the previous keys' fingerprint.
void kiss_ui_forget_fp(void);

// The secret-idle deadline's action: wipe the typed passphrase, never the
// flow. Setup mode, the staged seed and the screen stack survive; only the
// entries and any screen DERIVED from them expire -- the fingerprint screen
// drops back to the keyboard, because TAP TO OPEN there commits with
// whatever s_pass holds, and after a wipe that is an empty passphrase under
// a stale fingerprint.
void kiss_ui_idle_wipe(void);

// Register the LVGL pointer indev if not yet present (the setup wizard can run
// before the first login and needs touch too).
void kiss_ui_ensure_indev(void);
#ifdef SIMULATOR
void kiss_ui_drop_indev_for_test(void);   // put the process back to power-on
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
lv_obj_t *kiss_build_id_make(lv_obj_t *parent, int x, int y, bool with_radio,
                               bool stacked);
// Right edge of the row the call above drew. The version string grows between
// build profiles, so anything placed beside it measures rather than guesses.
int kiss_build_id_right(void);
void kiss_build_id_restyle(lv_obj_t *version_label);

#ifndef ESP_PLATFORM
// Walk only: the screen shown when a commit may have taken the old wallet with
// it and the staged copy is the last one. Unreachable by tapping -- it needs a
// failed flash write -- so the walk opens it directly. The close undoes exactly
// that open: the walk now visits mid-session (the lock tests need a live
// session under the screen), so it can no longer be a leaf that never leaves.
void kiss_ui_test_recover_screen(void);
void kiss_ui_test_recover_close(void);
// True only when the hidden login's LVGL-owned entry/callout copies are empty.
// The recovery walk uses this after the 900 ms mask timer would have fired.
bool kiss_ui_test_rendered_secret_empty(void);
#endif
