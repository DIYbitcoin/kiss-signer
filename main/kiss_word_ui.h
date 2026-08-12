// Enrolment for a custom way in: the owner picks four marks and rehearses them,
// and from then on that run of marks replaces KISS on this device.
//
// Its own module rather than another act inside kiss_duress_ui.c. That wizard
// teaches the two-signer idea and picks the MODIFIER, measured against a
// printed reference word; this one picks the WORD ITSELF, out of marks that
// have no reference word at all. Same feature family, opposite frame, and
// wiring a second stage machine into the first is how one of them ends up
// measuring the other's geometry.
#pragma once

#include "lvgl.h"

// Open the enrolment. done_cb runs when the owner leaves, whether or not a word
// was set -- Settings rebuilds itself on it, exactly as the duress wizard does.
void kiss_word_ui_open(lv_obj_t *parent, void (*done_cb)(void));

// True while any of its screens is up (or one is about to be built), so the
// game loop knows the touch panel is not its own. Same contract as
// kiss_duress_ui_active.
bool kiss_word_ui_active(void);

// The lock's close: screen and pending done callback both dropped, nothing
// handed back. For the idle deadline only -- BACK TO KISS keeps its own path.
void kiss_word_ui_lock_close(void);
