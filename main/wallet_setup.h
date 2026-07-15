// Step 7: first-boot seed wizard. Runs when no seed is stored — NEW (camera
// entropy → words → prove backup) or RESTORE (type your words). On success the
// seed is persisted and done_cb fires (caller then runs the type-twice login).
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void wallet_setup_open(lv_obj_t *parent, void (*done_cb)(void));
bool wallet_setup_active(void);

// Verify an EXISTING backup: type the stored wallet's words from paper and the
// device confirms they match, without revealing them. Reuses the restore
// keypad; never stages or alters the seed. done_cb fires on exit. (Reached from
// the wallet's BACKUP screen, not first-boot.)
void wallet_setup_open_verify(lv_obj_t *parent, void (*done_cb)(void));

// Feed captured entropy (device: camera page; sim: scripted). len 16 or 32.
// Advances the NEW flow to the word-reveal screen.
void wallet_setup_entropy(const uint8_t *entropy, unsigned len);
