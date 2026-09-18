# The boards a release is built for, and what each one's build is called.
# Sourced by tools/build_release.sh and tools/make_web_release.sh; not
# executable on its own.
#
# One table, because the two scripts have to agree on every name in it: the
# build directory the first one fills is the directory the second one publishes
# from, and an image published under the other board's file name is a device
# that boots to a dark screen. YAML and HTML cannot source this, so the same ids
# are written out again in the reproducible-build workflow's matrix and in the
# board picker on the install page (docs/index.html, and the offline copy in
# tools/make_offline_zip.py). Change them together.
#
# The Guition row is exactly what the release lane produced before there was a
# second board -- build-release/, guition_kiss_bringup, no -DKISS_BOARD and no
# file name suffix -- so its image, its published names and every link to them
# stay what they were.

KISS_RELEASE_BOARD_IDS="guition ws35"

# kiss_board_profile <id>: sets BOARD_BUILD, BOARD_APP, BOARD_SUFFIX,
# BOARD_NAME, BOARD_MODEL and the BOARD_ARGS array for that board, or returns 1
# for an id this tree does not know.
kiss_board_profile() {
    case "$1" in
        guition)
            BOARD_BUILD=build-release
            BOARD_APP=guition_kiss_bringup
            BOARD_ARGS=()
            BOARD_SUFFIX=""
            BOARD_NAME="Guition 4.3in"
            BOARD_MODEL="Guition JC4880P443C"
            ;;
        ws35)
            BOARD_BUILD=build-release-ws35
            BOARD_APP=ws35_kiss_bringup
            BOARD_ARGS=(-DKISS_BOARD=ws35)
            BOARD_SUFFIX="-ws35"
            BOARD_NAME="Waveshare 3.5in"
            BOARD_MODEL="Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5"
            ;;
        *)
            return 1
            ;;
    esac
}
