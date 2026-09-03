idf_component_register(
    SRCS "main.c" "sprites.c" "menu_img.c" "menu_logo.c" "gameover_img.c" "game_bg.c" "kiss_img.c" "kiss_art.c" "kiss_art_rle.c" "kiss_crypto.c" "kiss_psbt.c" "kiss_sp.c" "kiss_ui.c" "kiss_theme.c" "kiss_terms.c" "kiss_info.c" "kiss_recv.c" "kiss_sign.c" "kiss_scan.c" "kiss_settings.c" "kiss_fw.c" "kiss_fw_ui.c" "kiss_pqsig.c" "kiss_setup.c" "kiss_duress.c" "kiss_gword.c" "kiss_coverword.c" "kiss_duress_ui.c" "kiss_word_ui.c" "kiss_seed.c" "kiss_seed_sd.c" "kiss_kef.c" "kiss_kef_crypto.c" "kiss_pbkdf2.c" "kiss_cryptobench.c" "kiss_usage.c" "kiss_payee.c" "kiss_backup.c" "kiss_tapent.c" "kiss_dice.c" "kiss_dice_q.c" "kiss_rngq.c" "kiss_rngaudit.c" "kiss_cards_q.c" "kiss_lastword.c" "kiss_rehearse.c" "i18n.c" "i18n_tables.c" "font_kiss_lat14.c" "font_kiss_lat23.c" "font_kiss_lat28.c" "font_kiss_lat34.c" "font_kiss_mono14.c" "font_kiss_mono18.c" "font_kiss_mono21.c" "font_kiss_mono23.c" "font_kiss_mono28.c" "font_kiss_mono34.c" "font_kiss_num48.c" "font_kiss_ja23.c" "font_kiss_ko23.c" "font_kiss_zh23.c" "flag_imgs.c" "font_kiss_ja14.c" "font_kiss_ja28.c" "font_kiss_ko14.c" "font_kiss_ko28.c" "font_kiss_zh14.c" "font_kiss_zh28.c" "qr_transport.c" "platform_sd.c" "camera_spike.c" "osd_text.c" "osd_strips.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system spi_flash app_update esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_lcd esp_lcd_st7701 esp_lcd_touch esp_lcd_touch_gt911 esp_lvgl_port esp_timer esp_driver_ppa esp_mm libwally-core esp_video fatfs sdmmc esp_driver_sdmmc cUR k_quirc slhdsa nvs_flash bootloader_support efuse
)

# THE LINT LANE. There is no separate one: this component is the only place in
# the repo compiled by a compiler that implements these, and IDF already passes
# -Werror, so a hit here is a build failure on the lane CI runs for every pull
# request and the one HOUSE-RULES.md tells you to run by hand.
#
# PRIVATE, so it reaches main/ and never the vendored components.
#
# Why not the desktop build: clang answers -Wformat-truncation and
# -Wstringop-truncation with "unknown warning option" and compiles on. A lint
# pass built on those over there would report nothing, forever, and look green
# doing it.
#
# What each one found the day it went in, and what was rejected, because the
# rejected list is the half that stops this being re-proposed:
#
#   -Wshadow            3. One was a bug that had shipped: game_tick declared
#                       its own s_sd_badge_live over the file scope flag, so
#                       sd_badge_sync() set one and the breathe read the other.
#                       The SD badge had never breathed, and it renders
#                       identically in the simulator, so no frame, no walk and
#                       no overlap check could ever have said so.
#   -Wmissing-prototypes 8. Seven were main.c's exports, declared by hand in
#                       four other files with nothing comparing the copies;
#                       main/main.h is the answer. The eighth was a simulator
#                       seam being compiled into the firmware.
#   -Wnull-dereference  6, all kiss_psbt.c, where psbt_tx() was dereferenced
#                       unchecked in one function and NULL checked in another.
#   -Wswitch-default    2.
#   the rest            0 on the day they landed, and cheap to keep at 0.
#
# NOT enabled, with the count that decided it:
#   -Wundef             1022 hits, every one inside an IDF or LVGL header.
#   -Wredundant-decls   98, of which 94 in IDF headers.
#   -Wformat-truncation=2  28, all deliberate snprintf folds (wt_addr_fold,
#                       wt_name_fold, the markup builder in kiss_theme.c).
#                       Truncating IS the design there. Level 1 is already on
#                       through -Wall and is the one that catches the accident
#                       -- the 64 byte buffer handed a 160 byte caption.
#   -Wjump-misses-init  23, all goto over a declaration in kiss_sp.c,
#                       kiss_psbt.c and kiss_seed.c. A C idiom, not a defect.
#   -Wfloat-equal       3, all in the game: e->av is assigned exactly 0.0f as
#                       a sentinel meaning "this one never rotates", and an
#                       exact test is the correct read of an exact assignment.
#                       A tolerance there would be a lie about the test.
#   -Wcast-qual, -Wbad-function-cast, -Wformat-nonliteral
#                       129 between them, overwhelmingly LVGL idiom and
#                       translated format strings. The format strings have
#                       their own gate: tools/gen_i18n.py checks specifier
#                       parity across all 21 locales and the formatted length
#                       against the C buffer.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wshadow
    -Wnull-dereference
    -Wmissing-prototypes
    -Wstrict-prototypes
    -Wswitch-default
    -Wvla
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wformat-security
    -Winit-self
    -Wpointer-arith
    -Wwrite-strings
    -Wdouble-promotion
    -Wcast-align
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
