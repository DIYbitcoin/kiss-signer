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
// Result of the most recently completed/cancelled open_verify flow. Reset to
// false each time verification opens; true only after every word matched.
bool wallet_setup_verify_succeeded(void);

// AMNESIC mode's per-session load. Nothing is stored on this device, so every
// power-on starts here: type the words, or scan a seed QR made elsewhere. The
// mnemonic is staged in RAM only; done_cb then runs the NORMAL login (one
// passphrase), not the setup ritual. Also offers a way into the full wizard,
// otherwise an amnesic device could never make a fresh wallet.
void wallet_setup_open_load(lv_obj_t *parent, void (*done_cb)(void));

// SD storage is deliberately checked before the normal login. A configured
// SD wallet with its card removed is not a fresh device and must never fall
// through to setup. `wallet_setup_sd_status` verifies that the configured
// wallet file can be opened, wiping its temporary plaintext immediately.
// A nonzero result can be handed to the retry/recovery screen below.
int  wallet_setup_sd_status(void);
void wallet_setup_open_sd_missing(lv_obj_t *parent, int reason,
                                  void (*done_cb)(void));

// Feed captured entropy (device: camera page; sim: scripted). len 16 or 32.
// Advances the NEW flow to the word-reveal screen.
void wallet_setup_entropy(const uint8_t *entropy, unsigned len);
