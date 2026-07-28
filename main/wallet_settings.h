// Settings screen (from the home Settings tile). v1: network selection
// (mainnet / testnet) — testnet lets the whole sign flow be exercised with
// worthless coins. Persisted in NVS on device; theme switching lands later.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void wallet_settings_open(lv_obj_t *parent);
bool wallet_settings_active(void);
void wallet_settings_close(void);   // idle auto-lock: drop the screen

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
} wallet_settings_load_status_t;

wallet_settings_load_status_t wallet_settings_load(void);

// Stable symbolic cause plus the underlying platform error (0 when there was
// none). Kept ESP-independent so the simulator and host fault harnesses can
// exercise the same boot-routing contract.
const char *wallet_settings_load_status_name(wallet_settings_load_status_t status);
int wallet_settings_load_error_code(void);

#ifdef SIMULATOR
// Host-only fault seam: lets a focused simulator harness prove that build_game
// stops at the blocked screen instead of constructing setup/home behind it.
void wallet_settings_sim_set_load_result(wallet_settings_load_status_t status,
                                         int error_code);
#endif

// Full-screen language picker (21 locale choices + flags). Persists NVS "lang"
// on pick, then calls picked_cb (which owns rebuilding its screen; the overlay
// is a child of `parent` and dies with it). Also used by first-boot setup.
void wallet_lang_picker_open(lv_obj_t *parent, void (*picked_cb)(void));

// Picker grid slot (0..I18N_LANG_N-1, row-major) of a language: the picker
// displays alphabetically, not in enum order. Used by the sim's scripted walk.
int wallet_lang_pick_slot(int lang);
