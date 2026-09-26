cmake_minimum_required(VERSION 3.16)
# The board. Chosen here, at configure time, and nowhere else. The image name
# and the app descriptor's project_name both come from project(), which runs
# before Kconfig, so the profile has to be picked from a variable project() can
# see; main/CMakeLists.txt then asserts that the sdkconfig this produced agrees
# with it, so -DKISS_BOARD and CONFIG_KISS_BOARD_* can never drift apart.
#
# guition is the default and produces exactly what this file produced before
# the second board existed: project(guition_kiss_bringup), the committed
# sdkconfig, and not a byte of difference in the image. Every other board keeps
# its generated sdkconfig inside its own build directory (gitignored /build*/),
# layered from sdkconfig.defaults, the committed sdkconfig (the Guition
# profile: everything the boards share, which is nearly everything) and
# sdkconfig.<id> (only what differs). Later files win. An explicit -DSDKCONFIG
# or -DSDKCONFIG_DEFAULTS on the command line still takes precedence, which is
# how the release scripts hand in their regenerated sdkconfig.release.
#
# Above the include on purpose: project.cmake reads SDKCONFIG the moment it is
# included, so a value set after that line is a value it never sees.
set(KISS_BOARD "guition" CACHE STRING "board profile: guition (default), ws35 or jc1060")
if(NOT KISS_BOARD MATCHES "^(guition|ws35|jc1060)$")
    message(FATAL_ERROR "KISS_BOARD=${KISS_BOARD}: this tree knows guition, ws35 and jc1060")
endif()
# The Guition is the one board whose profile IS the committed sdkconfig. Every
# other board is that plus its own fragment, sdkconfig.<id>, which must exist:
# without it the layers would build the Guition's profile under another name.
if(NOT KISS_BOARD STREQUAL "guition")
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${KISS_BOARD}")
        message(FATAL_ERROR "KISS_BOARD=${KISS_BOARD} has no sdkconfig.${KISS_BOARD}")
    endif()
    if(NOT SDKCONFIG)
        set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    endif()
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS
            "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${KISS_BOARD}")
    endif()
endif()

# The component manager asks the registry for newer versions on every
# configure. dependencies.lock already fixes every version this build uses, so
# the answer can only ever be advice -- except that the manager also reads the
# `if:` rules of the newer versions it finds, and when one names a Kconfig
# symbol our pinned components do not define it reruns configure until it
# gives up with a FATAL_ERROR. esp_video 2.5.0 did exactly that on 2026-09-19
# and every build of every commit here stopped at configure. What the registry
# published today must not decide whether yesterday's tree builds. Set before
# the include, which runs the manager, and inherited by the process it spawns.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The VERSION file, read ONCE. It becomes PROJECT_VER, which lands in the
# image's own app descriptor, and main/CMakeLists.txt takes the same variable
# back off the build properties for the string on the Settings line. Without
# this the descriptor carries IDF's fallback and an SD update has nothing
# truthful to compare against: the update screen would be reading a version the
# firmware never claimed, and the downgrade refusal would be guesswork.
#
# file(STRINGS) is a READ, and a read is not a dependency. CMake never
# re-configures because a file it once read has changed, so PROJECT_VER froze
# at whatever VERSION said the day each build directory was FIRST configured,
# and every later `idf.py build` happily relinked an image declaring the frozen
# number. That is how build-release/ came to declare 0.1.0-beta8 against a tree
# that has never held anything past beta7, and how a device and the card in its
# own slot ended up disagreeing about which of them was newer. Neither number
# was lying; both were honest reports of stale configure state.
#
# No desktop gate can see this. Every sim/build_*.sh re-reads VERSION with
# `head -1` on each invocation, so the simulator is always fresh and the bug is
# structurally invisible to it. CMAKE_CONFIGURE_DEPENDS is what turns the read
# into a dependency, and it covers main/CMakeLists.txt too: a re-configure
# re-runs every component.
# Must be set BEFORE project().
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/VERSION)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION PROJECT_VER LIMIT_COUNT 1)

project(${KISS_BOARD}_kiss_bringup)
