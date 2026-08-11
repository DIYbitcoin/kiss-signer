// Desktop shim: k_quirc includes esp_log.h unconditionally, and kisstest now
// compiles k_quirc so the QR parser -- the first code to touch bytes off the
// camera -- is reachable from a test at all. Shimmed on the include path
// rather than by editing the vendored tree, so the component stays byte for
// byte what the device builds.
#ifndef KISS_SIM_ESP_LOG_H
#define KISS_SIM_ESP_LOG_H
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)
#endif
