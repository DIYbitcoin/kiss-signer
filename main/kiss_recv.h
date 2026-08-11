// Step 4: Receive (address + QR) and watch-only export screens.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool kiss_recv_active(void);              // true while either screen is up (owns touch)
void kiss_recv_open(lv_obj_t *parent);    // derive + show address, static QR, index nav
void kiss_recv_close(void);               // idle auto-lock: drop whichever is up
#ifdef SIMULATOR
// The derivation path's "?" sits after its caption, so its x moves with the
// translation: DERIVATION PATH is 15 characters and PERCORSO DI DERIVAZIONE is
// 23. A coordinate tap would miss in most locales and save the unchanged
// screen, which reads as a pass. Same reason kiss_info has these.
void kiss_recv_sim_open_path_help(void);
#endif
