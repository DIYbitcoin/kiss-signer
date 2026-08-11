// Step 5: Sign via SD — file picker -> verify screen -> hold-to-sign -> SD out.
#pragma once
#include "lvgl.h"
#include <stdbool.h>

void kiss_sign_open(lv_obj_t *parent);
bool kiss_sign_active(void);
void kiss_sign_close(void);   // idle auto-lock: drop the screen + any loaded PSBT

#ifndef ESP_PLATFORM
// Walk only: is HOLD TO SIGN live on the verify screen? See kiss_sign.c.
bool kiss_sign_test_armed(void);
#endif
