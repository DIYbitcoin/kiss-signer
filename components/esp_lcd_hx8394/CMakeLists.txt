# The 5in's panel driver, vendored (VENDOR.kiss.md says from where and why).
# Compiled into every image, like the other panel drivers the lock carries;
# only board_ws5.c calls it, so the linker drops it from the other boards'
# images and their bytes do not move.
idf_component_register(
    SRCS "esp_lcd_hx8394.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd
    PRIV_REQUIRES esp_driver_gpio
)
