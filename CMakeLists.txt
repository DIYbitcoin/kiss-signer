cmake_minimum_required(VERSION 3.16)
# The board. Chosen here, at configure time, and nowhere else. The image name
# and the app descriptor's project_name both come from project(), which runs
# before Kconfig, so the profile has to be picked from a variable project() can
# see; main/CMakeLists.txt then asserts that the sdkconfig this produced agrees
# with it, so -DKISS_BOARD and CONFIG_KISS_BOARD_* can never drift apart.
#
# guition is the default and produces exactly what this file produced before
# the second board existed: project(guition_kiss_bringup), the committed
# sdkconfig, and not a byte of difference in the image. The 3.5in board keeps
# its generated sdkconfig inside its own build directory (gitignored /build*/),
# layered from sdkconfig.defaults, the committed sdkconfig (the Guition
# profile: everything the two boards share, which is nearly everything) and
# sdkconfig.ws35 (only what differs). Later files win. An explicit -DSDKCONFIG
# or -DSDKCONFIG_DEFAULTS on the command line still takes precedence, which is
# how the release scripts hand in their regenerated sdkconfig.release.
#
# Above the include on purpose: project.cmake reads SDKCONFIG the moment it is
# included, so a value set after that line is a value it never sees.
set(KISS_BOARD "guition" CACHE STRING "board profile: guition (default) or ws35")
if(NOT KISS_BOARD MATCHES "^(guition|ws35)$")
    message(FATAL_ERROR "KISS_BOARD=${KISS_BOARD}: this tree knows guition and ws35")
endif()
if(KISS_BOARD STREQUAL "ws35")
    if(NOT SDKCONFIG)
        set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    endif()
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS
            "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.ws35")
    endif()
endif()

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
