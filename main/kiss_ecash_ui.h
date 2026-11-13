// The ecash screens: reached from SIGN's scanner when what it read is an
// ecash vault page's QR (docs/specs/ecash-spend-request.md), not a PSBT.
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// True when scanned bytes are for these screens: JSON, where a PSBT is
// binary. The scanner hands everything else to the PSBT path as before.
bool kiss_ecash_ui_wants(const uint8_t *bytes, size_t len);

// The page's key request opens the ecash key; anything else is reviewed as a
// spend request. done runs when the owner leaves, signed or not.
void kiss_ecash_ui_open(lv_obj_t *parent, const uint8_t *bytes, size_t len,
                        void (*done)(void));

bool kiss_ecash_ui_active(void);
void kiss_ecash_ui_close(void);   // idle lock: drop the screen and the request
