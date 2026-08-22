# SLH-DSA (FIPS 205), vendored from pq-code-package/slhdsa-c. See UPSTREAM.md.
#
# The SHA2 half only. SLH-DSA-SHA2-128s is the parameter set this firmware uses
# and nothing reaches the SHAKE sets, so their sources stay in upstream/ to keep
# the drop matching upstream and stay out of the image. sha2_512.c is in because
# slh_sha2.c's 192 and 256 bit sets call it, not because anything here does.
#
# slh_prehash.c is out for a sharper reason: hash_slh_verify() takes the WHOLE
# message and pre-hashes it itself, so verifying a 4.6 MB firmware image with it
# would mean holding 4.6 MB of RAM. The image is hashed as it streams past
# instead and the 32 byte digest is what gets signed, which is what every
# detached signature does anyway.
idf_component_register(
    SRCS "pq_hw_sha.c"
         "upstream/slh_dsa.c"
         "upstream/slh_sha2.c"
         "upstream/sha2_256.c"
         "upstream/sha2_512.c"
    INCLUDE_DIRS "." "upstream"
    REQUIRES mbedtls
)

# Renames the definition of sha2_256_compress in the vendored sha2_256.c so
# pq_hw_sha.c can supply the dispatcher under that name. UPSTREAM.md explains
# why it cannot be done by renaming the symbol at every call site instead.
target_compile_definitions(${COMPONENT_LIB} PRIVATE PQ_SHA256_COMPRESS_HOOK=1)
