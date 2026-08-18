// The RANDOMNESS AUDIT: an owner facing check that the chip spreads its
// numbers the way real noise does. 5000 draws from the same stream that
// feeds key material, counted into 100 piles on a live histogram, scored
// with chi square against the interval honest noise leaves 499 times in 500.
//
// What it deliberately is NOT: a gate. Keys never ride on this chip alone
// (kiss_entropy_mix3 folds camera or dice in), and no spread score can
// identify a SOURCE -- a formula passes every one -- which is why the screen
// leads with the provenance fact (kiss_trng_live) instead of the bars.
// Arithmetic in kiss_rngq.c, where /tmp/kisstest scores the same sums.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void kiss_rngaudit_open(lv_obj_t *parent, void (*done_cb)(void));
bool kiss_rngaudit_active(void);   // main.c's touch owner and lock registry
void kiss_rngaudit_close(void);    // the lock's teardown: no done callback

#ifdef SIMULATOR
// Render the finished screen for a rigged spread, so the walk photographs
// the two states an honest chip shows once in 500 runs: verdict < 0 is the
// flat comb (TOO EVEN), > 0 the sawtooth (UNEVEN). Precedent:
// kiss_settings_sim_reopen_storage.
void kiss_rngaudit_sim_result(int verdict);
#endif
