// Desktop shim -- see sim/shims/esp_log.h. The yields k_quirc makes are a
// no-op here: there is no watchdog to feed and one thread to yield to.
#ifndef KISS_SIM_TASK_H
#define KISS_SIM_TASK_H
static inline void vTaskDelay(unsigned t) { (void)t; }
#endif
