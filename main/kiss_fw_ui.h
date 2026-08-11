// The screens that replace the firmware from an SD card (see kiss_fw.h for
// what is actually checked). Split from kiss_fw.c so the version comparison
// and descriptor parsing there stay linkable into the desktop test runner
// without LVGL, the same split kiss_duress_ui.c has.
#pragma once
#include "lvgl.h"

// Open on the firmware screen: what is on the card, what is running, and
// whether the one can replace the other. done_cb fires however it ends, because
// the only caller (Settings) has somewhere to go back to either way.
//
// Nothing is written until the hold on the confirm screen completes, and
// nothing becomes bootable unless its signature checked out.
void kiss_fw_ui_open(lv_obj_t *parent, void (*done_cb)(void));

// The idle auto-lock path, and the touch owner check that goes with it.
//
// This screen is a child of the active screen, not of the wallet container the
// lock hides, so without these it stays lit and on top of a device that has
// locked underneath it -- and its BACK rebuilds Settings, where RECOVERY WORDS
// is one row away and nothing on that path asks whether a session is still
// open. In KEEP and SD modes the words unseal with the device key, which the
// lock does not wipe, so they render for whoever is holding the box.
bool kiss_fw_ui_active(void);
void kiss_fw_ui_close(void);   // idle auto-lock: drop the screen, no done_cb
