# Forwarding stub: register cUR as an ESP-IDF component named `cUR` while
# keeping the actual source files at the repo root. Globs `src/*.c` and
# `src/types/*.c` so newly added sources are picked up automatically. The
# bundled SHA-256 (`src/sha256/*.c`) is deliberately excluded here because
# UR_USE_MBEDTLS_SHA256 routes the compat shim to mbedTLS.

get_filename_component(CUR_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

file(GLOB CUR_SRCS
    "${CUR_REPO_ROOT}/src/*.c"
    "${CUR_REPO_ROOT}/src/types/*.c"
)

idf_component_register(
    SRCS ${CUR_SRCS}
    INCLUDE_DIRS "${CUR_REPO_ROOT}/src"
    REQUIRES mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC UR_USE_MBEDTLS_SHA256)
