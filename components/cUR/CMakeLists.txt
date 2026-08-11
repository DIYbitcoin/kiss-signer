idf_component_register(
    SRCS
        "src/ur.c"
        "src/ur_encoder.c"
        "src/ur_decoder.c"
        "src/fountain_encoder.c"
        "src/fountain_decoder.c"
        "src/fountain_utils.c"
        "src/bytewords.c"
        "src/crc32.c"
        "src/utils.c"
        "src/types/cbor_data.c"
        "src/types/bytes_type.c"
        "src/types/cbor_encoder.c"
        "src/types/cbor_decoder.c"
        "src/types/psbt.c"
        "src/types/registry.c"
        "src/types/byte_buffer.c"
        "src/sha256/sha256.c"
    INCLUDE_DIRS
        "src"
)

# kiss-signer-os: bundled SHA256 (no UR_USE_MBEDTLS_SHA256) so the identical
# sources build on desktop (sim/build_test.sh) and device — same policy as
# components/libwally-core. SHA here only seeds the fountain-code RNG; not
# consensus-critical perf.
