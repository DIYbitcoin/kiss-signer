cmake_minimum_required(VERSION 3.16)
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

project(guition_kiss_bringup)
