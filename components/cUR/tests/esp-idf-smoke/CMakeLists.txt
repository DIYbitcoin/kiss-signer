# Minimal ESP-IDF project that builds cUR as a component. Used by CI to
# verify that src/ compiles under xtensa-esp32 with UR_USE_MBEDTLS_SHA256.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cur_smoke)
