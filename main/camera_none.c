// camera_spike.h for a board whose camera path is not written yet: every
// start refuses and says why, and nothing else pretends there is a picture.
//
// The camera code composes its preview into the panel's own memory, and how it
// does that depends on the panel -- a portrait framebuffer the flush turns in
// software, an SPI panel it blits to, or, on the 7in, a landscape framebuffer
// it can address as the canvas. Until a board's arm of camera_spike.c has been
// written and read off that board's glass, the board builds this file instead:
// the scan screen and the entropy page take the same path they take when a
// sensor does not answer, which shows "camera unavailable" and, on the seed
// page, drops the camera from the three sources rather than inventing one.
#include "camera_spike.h"

static const char *const WHY = "CAM: no camera path on this board yet";

void camera_spike_set_panel(esp_lcd_panel_handle_t panel, void *fb0, void *fb1)
{
    (void)panel; (void)fb0; (void)fb1;
}
bool camera_spike_toggle(lv_obj_t *parent, i2c_master_bus_handle_t i2c_bus)
{
    (void)parent; (void)i2c_bus;
    return false;
}
const char *camera_spike_status(void) { return WHY; }
void camera_spike_set_preview_rect(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}
void camera_spike_flip_refresh(void) {}
bool camera_spike_owns_panel(void) { return false; }
void camera_spike_pause(bool on) { (void)on; }
bool camera_spike_is_on(void) { return false; }
bool camera_spike_check_died(void) { return false; }
const char *camera_spike_cycle_orientation(void) { return WHY; }
const char *camera_spike_zoom(int dir)
{
    (void)dir;
    return WHY;
}
void camera_spike_set_bus(void *i2c_bus) { (void)i2c_bus; }

bool camera_entropy_start(void) { return false; }
void camera_entropy_tap(void) {}
// Never true: no bytes from a camera that is not there.
bool camera_entropy_sources(uint8_t chain_out[32], uint8_t trng_out[32])
{
    (void)chain_out; (void)trng_out;
    return false;
}
void camera_entropy_stop(void) {}
int camera_entropy_progress(void) { return 0; }
int camera_entropy_reason(void) { return ENT_R_DARK; }

bool camera_scan_start(void *i2c_bus, void (*on_decode)(const char *data, size_t len))
{
    (void)i2c_bus; (void)on_decode;
    return false;
}
void camera_scan_stop(void) {}
void camera_scan_progress(int seen, int total) { (void)seen; (void)total; }
