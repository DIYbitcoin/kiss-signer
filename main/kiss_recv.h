// Step 4: Receive (address + QR) and watch-only export screens.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool kiss_recv_active(void);              // true while either screen is up (owns touch)
void kiss_recv_open(lv_obj_t *parent);    // derive + show address, static QR, index nav
// KEYS' FIRST ADDRESS rows jump here: THIS ADDRESS tab, index 0 -- the one
// place on the device a full receive address renders as text.
void kiss_recv_open_first(lv_obj_t *parent);
// The scan key gate's way home: lands on the SILENT tab.
void kiss_recv_open_sp(lv_obj_t *parent);
// PAIR COORDINATOR's VERIFY control: straight into the address scan, with no
// receive screen built in front of it. Backing out or finishing lands on
// RECEIVE, the way every other exit from the scan already does.
void kiss_recv_open_verify(lv_obj_t *parent);
void kiss_recv_close(void);               // idle auto-lock: drop whichever is up
#ifdef SIMULATOR
// The derivation path's "?" sits after its caption, so its x moves with the
// translation: DERIVATION PATH is 15 characters and PERCORSO DI DERIVAZIONE is
// 23. A coordinate tap would miss in most locales and save the unchanged
// screen, which reads as a pass. Same reason kiss_info has these.
void kiss_recv_sim_open_path_help(void);
#endif
