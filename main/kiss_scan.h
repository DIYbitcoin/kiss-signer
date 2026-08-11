// Step 6: QR scan screen. Device: live camera view (camera_spike pipeline) +
// k_quirc decode feeding qr_transport until a full PSBT is assembled. Sim: the
// script injects decoded QR strings via kiss_scan_inject().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

// on_psbt: called with the assembled raw PSBT + the wire format it arrived in
// (QRT_FMT_*), after the scan screen has closed itself.
// on_cancel: user backed out or the result was unusable.
void kiss_scan_open(lv_obj_t *parent,
                      void (*on_psbt)(const uint8_t *psbt, size_t len, int fmt),
                      void (*on_cancel)(void));

// Raw single-QR mode (verify-address): the FIRST decoded payload is handed to
// on_text as-is (NUL-terminated) — no PSBT assembly. Same camera/cancel UX.
void kiss_scan_open_raw(lv_obj_t *parent,
                          void (*on_text)(const char *txt, size_t len),
                          void (*on_cancel)(void));

bool kiss_scan_active(void);
void kiss_scan_close(void);   // idle auto-lock: stop camera + drop the screen
// Cancel as if the CLOSE control had been used: tears down AND runs the
// on_cancel callback, so the caller lands back where it came from. Safe to call
// when no scan is open. main.c uses this for a raw-touch escape that does not
// depend on LVGL receiving input while the camera is streaming.
void kiss_scan_cancel(void);
                                // WITHOUT firing on_cancel (nothing reopens)

// Device: the shared touch/camera I2C bus (i2c_master_bus_handle_t), set once
// at boot. void* keeps ESP driver types out of the sim build.
void kiss_scan_set_bus(void *i2c_bus);

// Feed one decoded QR payload (sim script; also the device decode path lands
// here via the poll timer).
void kiss_scan_inject(const char *data, size_t len);
