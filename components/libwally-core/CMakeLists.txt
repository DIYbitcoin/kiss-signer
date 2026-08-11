# libwally-core (ElementsProject) as an ESP-IDF component.
# upstream/ is vendored at commit c5591834 (release_1.5.4) — the exact commit both
# Blockstream Jade and Kern pin, so it's proven on ESP32 (Jade) and ESP32-P4 (Kern).
# Component layout follows Jade/Kern; differences from theirs:
#   - no mbedtls SHA (uses libwally's internal ccan SHA) so the identical sources also
#     compile in the desktop test runner (sim/build_test.sh). mbedtls acceleration can
#     be added later (needs Kern-style mbedtls v4 compat shims on IDF 6).
#   - BUILD_ELEMENTS off (no Liquid support) and elements-only secp modules off.
idf_component_register(SRCS
                       "upstream/src/amalgamation/combined.c"
                       INCLUDE_DIRS
                       "upstream/include"
                       # public: kiss_sp.c calls the embedded secp256k1
                       # directly for the BIP352/374 point math
                       "upstream/src/secp256k1/include"
                       PRIV_INCLUDE_DIRS
                       "."
                       "upstream"
                       "upstream/src"
                       "upstream/src/ccan"
                       "upstream/src/secp256k1"
                       "upstream/src/secp256k1/src"
                       "upstream/src/secp256k1/include")

set_source_files_properties(
    upstream/src/amalgamation/combined.c
    PROPERTIES COMPILE_FLAGS
    "-Wno-nonnull-compare -Wno-unused-function -Wno-error=maybe-uninitialized -Wno-type-limits -Wno-error=array-parameter -Wno-unterminated-string-initialization"
    )

target_compile_definitions(${COMPONENT_TARGET} PRIVATE NDEBUG=1)
target_compile_options(${COMPONENT_TARGET} PRIVATE "-fstack-protector-strong")

# English-only BIP39 wordlist (v1 scope; saves flash)
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DBUILD_MINIMAL=1")

# libsecp: small precomputed ecmult tables (flash size); modules the amalgamation
# needs are defined inside combined.c itself.
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DECMULT_WINDOW_SIZE=8")
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_BUILTIN_POPCOUNT=1")
