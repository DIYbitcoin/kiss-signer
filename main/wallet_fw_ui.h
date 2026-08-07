// The screens that replace the firmware from an SD card (see wallet_fw.h for
// what is actually checked). Split from wallet_fw.c so the version comparison
// and descriptor parsing there stay linkable into the desktop test runner
// without LVGL, the same split wallet_duress_ui.c has.
#pragma once
#include "lvgl.h"

// Open on the firmware screen: what is on the card, what is running, and
// whether the one can replace the other. done_cb fires however it ends, because
// the only caller (Settings) has somewhere to go back to either way.
//
// Nothing is written until the hold on the confirm screen completes, and
// nothing becomes bootable unless its signature checked out.
void wallet_fw_ui_open(lv_obj_t *parent, void (*done_cb)(void));
