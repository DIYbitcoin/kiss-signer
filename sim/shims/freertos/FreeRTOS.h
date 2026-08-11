// Desktop shim -- see sim/shims/esp_log.h.
#ifndef KISS_SIM_FREERTOS_H
#define KISS_SIM_FREERTOS_H
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
#endif
