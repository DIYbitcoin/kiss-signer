// The WALLET tile: facts about the logged-in wallet (with plain-words "?"
// education) and PAIR COORDINATOR exports.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool wallet_info_active(void);            // true while any of its screens is up
void wallet_info_open(lv_obj_t *parent);
// Shared fingerprint explainer. Pass the home fingerprint to include it in the
// title, or NULL for the generic WALLET-page card.
lv_obj_t *wallet_info_fp_card_open(lv_obj_t *parent, const char *fingerprint);
// Settings owns the RECOVERY WORDS entry. The sensitive reveal/verify screens
// stay here so there is only one implementation of that flow; done_cb returns
// to Settings when the user leaves it.
void wallet_info_open_words(lv_obj_t *parent, void (*done_cb)(void));
void wallet_info_close(void);             // idle auto-lock: drop whichever is up
#ifdef SIMULATOR
// Deterministic visual-QA captures. The walk used to tap the "?" chips by
// coordinate, but they sit after their section label, so the x moves with the
// translation and the y moves whenever this column is re-laid out. A missed
// tap saves the unchanged screen instead of the card, which looks like a pass.
void wallet_info_sim_open_type_help(void);
void wallet_info_sim_open_fp_help(void);
#endif
