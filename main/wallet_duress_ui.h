// The screens that configure the duress unlock (see wallet_duress.h for what
// it is and why). Split from wallet_duress.c so the classifier there stays
// linkable into the desktop test runner without LVGL.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Run the chooser: teach the idea, pick the real stroke, draw it twice, pick
// the decoy's stroke, draw it twice. done_cb fires however it ends -- finished,
// skipped or cancelled -- because both callers (the last step of the setup
// wizard, and Settings) have somewhere to go next either way.
//
// Nothing is written until both strokes have been confirmed twice. Backing out
// at any point leaves the previous configuration exactly as it was.
void wallet_duress_ui_open(lv_obj_t *parent, void (*done_cb)(void));

// The same screen set, opened on a signer whose wallet has NO passphrase: one
// page explaining that this wallet is already the spare, so there is nothing to
// put behind a stroke. Offers to clear any stroke left over from a build that
// allowed it. done_cb fires on exit, as above.
void wallet_duress_ui_open_nopass(lv_obj_t *parent, void (*done_cb)(void));

// True while these screens own the touch input (the game must not also see it).
bool wallet_duress_ui_active(void);
