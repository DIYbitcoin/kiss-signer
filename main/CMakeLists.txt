# One board file per image, never both: each is that board's whole answer to
# kiss_board.h, and the other board's would not even compile against the
# drivers it names.
if(CONFIG_KISS_BOARD_WS35)
    set(KISS_BOARD_SRC "board_ws35.c")
    set(KISS_CAMERA_SRC "camera_spike.c")
    # The baked backdrops drawn at this board's 480x320 by the same
    # generators (--board ws35). Same symbols, same headers, one set per image.
    set(KISS_ART_SRCS "menu_img_ws35.c" "menu_logo_ws35.c" "gameover_img_ws35.c" "game_bg.c" "kiss_img_ws35.c")
    set(KISS_SPRITES_SRC "sprites_ws35.c")
    # The small faces with strong hinting (tools/fonts/gen_fonts.sh says why),
    # same names, same link slots.
    set(KISS_F_LAT14 "fonts_ws35/font_kiss_lat14.c")
    set(KISS_F_LAT18 "fonts_ws35/font_kiss_lat18.c")
    set(KISS_F_LAT23 "fonts_ws35/font_kiss_lat23.c")
    set(KISS_F_MONO14 "fonts_ws35/font_kiss_mono14.c")
    set(KISS_F_MONO18 "fonts_ws35/font_kiss_mono18.c")
    set(KISS_F_MONO21 "fonts_ws35/font_kiss_mono21.c")
    set(KISS_F_MONO23 "fonts_ws35/font_kiss_mono23.c")
    set(KISS_F_NUM28 "fonts_ws35/font_kiss_num28.c")
elseif(CONFIG_KISS_BOARD_GUITION OR CMAKE_BUILD_EARLY_EXPANSION)
    # The early pass collects requirements before any CONFIG_ symbol exists,
    # and the source list it sees is not the one that gets built.
    set(KISS_BOARD_SRC "board_guition.c")
    set(KISS_CAMERA_SRC "camera_spike.c")
    # In this order, with game_bg.c where it always was: the link order is
    # the source order, and the Guition image is held byte-identical.
    set(KISS_ART_SRCS "menu_img.c" "menu_logo.c" "gameover_img.c" "game_bg.c" "kiss_img.c")
    set(KISS_SPRITES_SRC "sprites.c")
    set(KISS_F_LAT14 "font_kiss_lat14.c")
    set(KISS_F_LAT18 "font_kiss_lat18.c")
    set(KISS_F_LAT23 "font_kiss_lat23.c")
    set(KISS_F_MONO14 "font_kiss_mono14.c")
    set(KISS_F_MONO18 "font_kiss_mono18.c")
    set(KISS_F_MONO21 "font_kiss_mono21.c")
    set(KISS_F_MONO23 "font_kiss_mono23.c")
    set(KISS_F_NUM28 "font_kiss_num28.c")
elseif(CONFIG_KISS_BOARD_JC1060)
    set(KISS_BOARD_SRC "board_jc1060.c")
    # No camera path is written for this board's landscape framebuffer yet:
    # camera_none.c answers camera_spike.h with refusals until it is.
    set(KISS_CAMERA_SRC "camera_none.c")
    # The 4.3in's art and faces until this board's own are drawn. The screens
    # scale to 1024x600 already; the baked backdrops and sprites do not.
    set(KISS_ART_SRCS "menu_img.c" "menu_logo.c" "gameover_img.c" "game_bg.c" "kiss_img.c")
    set(KISS_SPRITES_SRC "sprites.c")
    set(KISS_F_LAT14 "font_kiss_lat14.c")
    set(KISS_F_LAT18 "font_kiss_lat18.c")
    set(KISS_F_LAT23 "font_kiss_lat23.c")
    set(KISS_F_MONO14 "font_kiss_mono14.c")
    set(KISS_F_MONO18 "font_kiss_mono18.c")
    set(KISS_F_MONO21 "font_kiss_mono21.c")
    set(KISS_F_MONO23 "font_kiss_mono23.c")
    set(KISS_F_NUM28 "font_kiss_num28.c")
else()
    message(FATAL_ERROR "no source list for this board in main/CMakeLists.txt")
endif()

idf_component_register(
    SRCS "main.c" ${KISS_BOARD_SRC} "kiss_touch.c" ${KISS_SPRITES_SRC} ${KISS_ART_SRCS} "kiss_art.c" "kiss_art_rle.c" "kiss_crypto.c" "kiss_psbt.c" "kiss_sp.c" "kiss_ui.c" "kiss_theme.c" "kiss_terms.c" "kiss_info.c" "kiss_recv.c" "kiss_sign.c" "kiss_scan.c" "kiss_settings.c" "kiss_fw.c" "kiss_fw_ui.c" "kiss_pqsig.c" "kiss_setup.c" "kiss_duress.c" "kiss_gword.c" "kiss_coverword.c" "kiss_duress_ui.c" "kiss_word_ui.c" "kiss_seed.c" "kiss_seed_sd.c" "kiss_kef.c" "kiss_kef_crypto.c" "kiss_pbkdf2.c" "kiss_cryptobench.c" "kiss_usage.c" "kiss_payee.c" "kiss_backup.c" "kiss_tapent.c" "kiss_dice.c" "kiss_dice_q.c" "kiss_rngq.c" "kiss_rngaudit.c" "kiss_cards_q.c" "kiss_lastword.c" "kiss_rehearse.c" "i18n.c" "i18n_tables.c" ${KISS_F_LAT14} ${KISS_F_LAT23} "font_kiss_lat28.c" "font_kiss_lat34.c" ${KISS_F_MONO14} ${KISS_F_MONO18} ${KISS_F_MONO21} ${KISS_F_MONO23} "font_kiss_mono28.c" "font_kiss_mono34.c" "font_kiss_num48.c" "font_kiss_ja23.c" "font_kiss_ko23.c" "font_kiss_zh23.c" "flag_imgs.c" "font_kiss_ja14.c" "font_kiss_ja28.c" "font_kiss_ko14.c" "font_kiss_ko28.c" "font_kiss_zh14.c" "font_kiss_zh28.c" "qr_transport.c" "platform_sd.c" ${KISS_CAMERA_SRC} "osd_text.c" "osd_strips.c" ${KISS_F_LAT18} "font_kiss_ja18.c" "font_kiss_ko18.c" "font_kiss_zh18.c" ${KISS_F_NUM28}
    INCLUDE_DIRS "."
    REQUIRES esp_system spi_flash app_update esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_lcd esp_lcd_st7701 esp_lcd_st7796 esp_lcd_jd9165 esp_lcd_touch esp_lcd_touch_gt911 esp_lcd_touch_ft5x06 esp_timer esp_driver_ppa esp_mm libwally-core esp_video fatfs sdmmc esp_driver_sdmmc cUR k_quirc slhdsa nvs_flash bootloader_support efuse
)

# THE LINT LANE. There is no separate one: this component is the only place in
# the repo compiled by a compiler that implements these, and IDF already passes
# -Werror, so a hit here is a build failure on the lane CI runs for every pull
# request and the one dev/HOUSE-RULES.md tells you to run by hand.
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

# The project name, for the same reason: it is the app descriptor's
# project_name, the one field an SD update image carries that says which BOARD
# it was built for, and kiss_fw.c compares it against the running image. The
# simulator has no descriptor and takes a default in the source.
target_compile_definitions(${COMPONENT_LIB} PRIVATE KISS_PROJECT_STR="${CMAKE_PROJECT_NAME}")

# The board profile, asserted rather than trusted. The root CMakeLists.txt
# picks the project name from -DKISS_BOARD before Kconfig runs, and Kconfig
# then says CONFIG_KISS_BOARD_* from whichever sdkconfig it was handed. Those
# are two sources, and a build directory configured for one board and then
# re-pointed at the other would otherwise link the 4.3in image under the
# 3.5in name, or the reverse, without a word. Not during IDF's early expansion
# pass, which runs this file once to collect requirements before any CONFIG_
# symbol exists; in the real pass they are guaranteed to be visible.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Every board, not one against the other: CONFIG_KISS_BOARD_<ID> is a
    # Kconfig choice, so exactly one is set, and it has to be this one.
    string(TOUPPER "${KISS_BOARD}" _kiss_board_uc)
    if(NOT CONFIG_KISS_BOARD_${_kiss_board_uc})
        if(KISS_BOARD STREQUAL "guition")
            message(FATAL_ERROR "the committed sdkconfig has been switched to another board's "
                                "profile; it must stay the Guition profile")
        else()
            message(FATAL_ERROR "KISS_BOARD=${KISS_BOARD} but the sdkconfig is not the "
                                "${KISS_BOARD} profile: delete the build directory's "
                                "sdkconfig and reconfigure")
        endif()
    endif()
    # Both camera sensors answer at SCCB address 0x36, so an image carrying
    # both drivers would let esp_video's auto-detect pick one by chip-ID
    # readback order. Exactly one per board profile.
    if(CONFIG_CAMERA_OV5647 AND CONFIG_CAMERA_OV02C10)
        message(FATAL_ERROR "both camera sensors are enabled and both answer at SCCB 0x36: "
                            "exactly one per board profile")
    endif()
endif()

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
