// Step 5: Sign via SD — file picker -> verify screen -> hold-to-sign -> SD out.
#pragma once
#include "lvgl.h"
#include <stdbool.h>

void wallet_sign_open(lv_obj_t *parent);
bool wallet_sign_active(void);
void wallet_sign_close(void);   // idle auto-lock: drop the screen + any loaded PSBT
