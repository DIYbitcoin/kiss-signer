# Local libwally changes

`upstream/src/sign.c`: KISS's BIP461 draft hardening verifies each ECDSA
signature against its key and message before returning it. Verification
failure clears the output and returns WALLY_ERROR. Low-R grinding also refuses
uint32 counter exhaustion instead of wrapping. Signature bytes are unchanged.

Preserve or replace these checks when updating the vendored library. Coverage:
`sim/test_crypto.c` (BIP461 fixtures and existing end-to-end PSBT tests).

## Updating the vendored copy

`upstream/` holds every file of the release tag, secp256k1 submodule included,
except these folders: `.github/`, `contrib/`, `docs/`, `tools/`, `src/ctest/`,
`src/test/`, `src/pyexample/`, `src/swig_java/`, `src/swig_python/`,
`src/wasm_package/`, and under `src/secp256k1/`: `.github/`,
`autotools-aux/`, `ci/`, `cmake/`, `contrib/`, `doc/`, `examples/`. Copy the
new tag over with that rule, reapply the `sign.c` change above, and set the
version strings in `config.h`.
