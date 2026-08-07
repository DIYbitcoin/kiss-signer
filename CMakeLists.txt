cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The same VERSION file main/CMakeLists.txt puts on the Settings line, set here
# as PROJECT_VER so it also lands in the image's own app descriptor. Without
# this the descriptor carries IDF's fallback and an SD update has nothing
# truthful to compare against: the update screen would be reading a version the
# firmware never claimed, and the downgrade refusal would be guesswork.
# Must be set BEFORE project().
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION PROJECT_VER LIMIT_COUNT 1)

project(guition_kiss_bringup)
