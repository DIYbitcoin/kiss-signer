// The WALLET tile: facts about the logged-in wallet (with plain-words "?"
// education), backup-words re-view, and PAIR COORDINATOR exports.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool wallet_info_active(void);            // true while any of its screens is up
void wallet_info_open(lv_obj_t *parent);
void wallet_info_close(void);             // idle auto-lock: drop whichever is up
