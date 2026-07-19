idf_component_register(
    SRCS "main.c" "sprites.c" "menu_img.c" "menu_logo.c" "gameover_img.c" "wallet_img.c" "wallet_crypto.c" "wallet_psbt.c" "wallet_ui.c" "wallet_theme.c" "wallet_info.c" "wallet_recv.c" "wallet_sign.c" "wallet_scan.c" "wallet_settings.c" "wallet_setup.c" "wallet_seed.c" "wallet_usage.c" "i18n.c" "i18n_tables.c" "font_kiss_lat14.c" "font_kiss_lat23.c" "font_kiss_lat28.c" "font_kiss_ja23.c" "font_kiss_ko23.c" "font_kiss_zh23.c" "flag_imgs.c" "font_kiss_ja14.c" "font_kiss_ja28.c" "font_kiss_ko14.c" "font_kiss_ko28.c" "font_kiss_zh14.c" "font_kiss_zh28.c" "qr_transport.c" "platform_sd.c" "camera_spike.c" "scan_osd.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system spi_flash esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_lcd esp_lcd_st7701 esp_lcd_touch esp_lcd_touch_gt911 esp_lvgl_port esp_timer esp_driver_ppa esp_mm libwally-core esp_video fatfs sdmmc esp_driver_sdmmc cUR k_quirc nvs_flash bootloader_support efuse
)

# generated font_kiss_*.c include lvgl via the plain "lvgl.h" form
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# version string shown in Settings (single source: the VERSION file)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../VERSION KISS_VERSION_LINE LIMIT_COUNT 1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE KISS_VERSION_STR="${KISS_VERSION_LINE}")

# short commit for verifier matching. Passed by tools/build_release.sh from the
# HOST's git (as a cache var it re-triggers configure whenever it changes; git
# inside the container can't read the bind-mounted repo's ownership anyway).
if(KISS_COMMIT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KISS_COMMIT_STR="${KISS_COMMIT}")
endif()

# release profile (tools/build_release.sh passes -DKISS_RELEASE=1): compiles
# out the dev seed + selftest fingerprint display and marks the build
if(KISS_RELEASE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KISS_RELEASE=1)
endif()
