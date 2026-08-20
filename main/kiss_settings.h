// Settings screen (from the home Settings tile). v1: network selection
// (mainnet / testnet) — testnet lets the whole sign flow be exercised with
// worthless coins. Persisted in NVS on device; theme switching lands later.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void kiss_settings_open(lv_obj_t *parent);
bool kiss_settings_active(void);
void kiss_settings_close(void);   // idle auto-lock: drop the screen

// Boot-time NVS gate. A missing namespace/key is a valid fresh-device default;
// every other init/open/read failure is fatal for this boot. In particular,
// NO_FREE_PAGES and NEW_VERSION are reported to the caller and are NEVER
// "repaired" by erasing NVS: that partition may contain the KEEP seed and the
// SD-card device key.
typedef enum {
    WSETTINGS_LOAD_OK = 0,
    WSETTINGS_LOAD_NVS_NO_FREE_PAGES,
    WSETTINGS_LOAD_NVS_NEW_VERSION,
    WSETTINGS_LOAD_NVS_INIT_FAILED,
    WSETTINGS_LOAD_NVS_OPEN_FAILED,
    WSETTINGS_LOAD_NVS_READ_FAILED,
} kiss_settings_load_status_t;

kiss_settings_load_status_t kiss_settings_load(void);

// Display unit for amounts (WT_DENOM_SATS / WT_DENOM_BTC): applies it and
// writes it, so the sign screen can offer the switch where the amounts are.
void kiss_settings_set_denom(int d);

// Stable symbolic cause plus the underlying platform error (0 when there was
// none). Kept ESP-independent so the simulator and host fault harnesses can
// exercise the same boot-routing contract.
const char *kiss_settings_load_status_name(kiss_settings_load_status_t status);
int kiss_settings_load_error_code(void);

#ifdef SIMULATOR
// Host-only fault seam: lets a focused simulator harness prove that build_game
// stops at the blocked screen instead of constructing setup/home behind it.
void kiss_settings_sim_set_load_result(kiss_settings_load_status_t status,
                                         int error_code);
#endif

// Full-screen language picker (21 locale choices + flags). Persists NVS "lang"
// on pick, then calls picked_cb (which owns rebuilding its screen; the overlay
// is a child of `parent` and dies with it). Also used by first-boot setup.
void kiss_lang_picker_open(lv_obj_t *parent, void (*picked_cb)(void));

// Picker grid slot (0..I18N_LANG_N-1, row-major) of a language: the picker
// displays alphabetically, not in enum order. Used by the sim's scripted walk.
int kiss_lang_pick_slot(int lang);

#ifdef SIMULATOR
// Sim only: rebuild the storage chooser so both flash-encryption states render.
void kiss_settings_sim_reopen_storage(void);
void kiss_settings_sim_reopen_network(void);
// Same two-render problem on the history chooser: the ON row's sub follows
// the encryption state.
void kiss_settings_sim_reopen_history(void);
#endif
