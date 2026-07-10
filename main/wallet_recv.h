// Step 4: Receive (address + QR) and watch-only export screens.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool wallet_recv_active(void);              // true while either screen is up (owns touch)
void wallet_recv_open(lv_obj_t *parent);    // derive + show address, static QR, index nav
void wallet_export_open(lv_obj_t *parent);  // watch-only descriptor for Sparrow etc.
void wallet_recv_close(void);               // idle auto-lock: drop whichever is up
