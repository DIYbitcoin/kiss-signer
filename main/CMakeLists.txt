idf_component_register(
    SRCS "main.c" "sprites.c" "menu_img.c" "menu_logo.c" "gameover_img.c" "game_bg.c" "kiss_img.c" "kiss_art.c" "kiss_art_rle.c" "kiss_crypto.c" "kiss_psbt.c" "kiss_sp.c" "kiss_ui.c" "kiss_theme.c" "kiss_info.c" "kiss_recv.c" "kiss_sign.c" "kiss_scan.c" "kiss_settings.c" "kiss_fw.c" "kiss_fw_ui.c" "kiss_pqsig.c" "kiss_setup.c" "kiss_duress.c" "kiss_gword.c" "kiss_coverword.c" "kiss_duress_ui.c" "kiss_word_ui.c" "kiss_seed.c" "kiss_seed_sd.c" "kiss_kef.c" "kiss_kef_crypto.c" "kiss_usage.c" "kiss_payee.c" "kiss_backup.c" "kiss_tapent.c" "kiss_dice.c" "kiss_dice_q.c" "kiss_rngq.c" "kiss_rngaudit.c" "kiss_cards_q.c" "kiss_lastword.c" "kiss_rehearse.c" "i18n.c" "i18n_tables.c" "font_kiss_lat14.c" "font_kiss_lat23.c" "font_kiss_lat28.c" "font_kiss_lat34.c" "font_kiss_mono14.c" "font_kiss_mono18.c" "font_kiss_mono21.c" "font_kiss_mono23.c" "font_kiss_mono28.c" "font_kiss_mono34.c" "font_kiss_num48.c" "font_kiss_ja23.c" "font_kiss_ko23.c" "font_kiss_zh23.c" "flag_imgs.c" "font_kiss_ja14.c" "font_kiss_ja28.c" "font_kiss_ko14.c" "font_kiss_ko28.c" "font_kiss_zh14.c" "font_kiss_zh28.c" "qr_transport.c" "platform_sd.c" "camera_spike.c" "osd_text.c" "osd_strips.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system spi_flash app_update esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_lcd esp_lcd_st7701 esp_lcd_touch esp_lcd_touch_gt911 esp_lvgl_port esp_timer esp_driver_ppa esp_mm libwally-core esp_video fatfs sdmmc esp_driver_sdmmc cUR k_quirc slhdsa nvs_flash bootloader_support efuse
)

# generated font_kiss_*.c include lvgl via the plain "lvgl.h" form
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# version string shown in Settings. NOT a second read of the VERSION file: this
# takes the value the root CMakeLists.txt already set as PROJECT_VER, which is
# the same value the app descriptor carries. Reading the file again here made
# the Settings line and the descriptor two independent pipelines that only
# happened to agree, and nothing enforced it.
idf_build_get_property(KISS_VERSION_LINE PROJECT_VER)
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

# field-debug PSBT dump (idf.py -DKISS_PSBT_DUMP=1): logs the received PSBT as
# hex on the serial console so it can be parsed with tools/psbt_fields.py.
# Deliberately opt-in per build -- an unsigned PSBT is not a secret, but it is
# amounts, addresses and counterparties, which must never reach a log by
# default. See the guard comment in kiss_sign.c.
if(KISS_PSBT_DUMP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KISS_PSBT_DUMP=1)
endif()
