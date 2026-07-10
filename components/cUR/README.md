# cUR — C implementation of Blockchain Commons Uniform Resources

A lightweight C implementation of the [Blockchain Commons Uniform Resources (UR)](https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-005-ur.md)
specification — a method for encoding structured binary data optimized
for transport via QR codes.

Targets PC, ESP-IDF (e.g. ESP32, ESP32-P4), and the Kendryte K210
(Krux / MaixPy) from a single source tree. SHA-256 backend is selected
at compile time to use the platform's hardware when available.

## Supported UR types

| UR type    | CBOR tag | Source                            |
|------------|---------:|-----------------------------------|
| `bytes`    | —        | `src/types/bytes_type.c`          |
| `psbt`     | 310      | `src/types/psbt.c`                |
| `bip39`    | —        | `src/types/bip39.c`               |
| `hd-key`   | 303      | `src/types/hd_key.c`              |
| `output`   | 308      | `src/types/output.c`              |
| `account`  | 311      | `src/types/output.c` + `multi_key.c` |

Helper types live alongside: `keypath.c` (BIP-32 derivation paths) and
`multi_key.c` (multisig keys).

## Building (PC)

```bash
make clean && make all        # produces src/libur.a
make test                     # runs every test suite
make check                    # alias for `make test`
```

Sanitizer build — swaps `-O2` for `-O0 -fsanitize=address,undefined`:

```bash
make clean && make DEBUG=1 test
```

Coverage report (requires `lcov`):

```bash
make coverage                 # writes coverage_html/index.html
```

Individual suites are also addressable (`make test-bytes-decoder`,
`make test-negative`, etc.).

## Minimal API example

Encode → decode roundtrip:

```c
#include "ur_encoder.h"
#include "ur_decoder.h"

// Your CBOR payload (produced via types/*.c or your own encoder).
const uint8_t cbor[] = { /* ... */ };
const size_t  cbor_len = sizeof(cbor);

// Encoder: type name, payload, max/min fragment bytes, starting seq num.
ur_encoder_t *enc = ur_encoder_new("bytes", cbor, cbor_len,
                                   /*max_fragment_len=*/200,
                                   /*first_seq_num=*/0,
                                   /*min_fragment_len=*/10);

ur_decoder_t *dec = ur_decoder_new();
char *part = NULL;
while (!ur_decoder_is_complete(dec)) {
    if (!ur_encoder_next_part(enc, &part)) break;
    ur_decoder_receive_part(dec, part);
    free(part);
}

if (ur_decoder_is_success(dec)) {
    ur_result_t *r = ur_decoder_get_result(dec);
    // r->type, r->cbor_data, r->cbor_len
}

ur_decoder_free(dec);
ur_encoder_free(enc);
```

Type-specific helpers (`bytes_from_cbor`, `psbt_from_cbor`,
`output_from_descriptor_string`, etc.) live in `src/types/*.h`.

## Platform integration

The SHA-256 dependency is the only platform-sensitive piece. A
compile-time flag picks the backend; see `src/sha256/sha256_compat.h`.

| Target           | Flag                    | Backend                       |
|------------------|-------------------------|-------------------------------|
| PC (default)     | *(none)*                | bundled `src/sha256/sha256.c` |
| ESP-IDF          | `UR_USE_MBEDTLS_SHA256` | mbedTLS (HW-accelerated)      |
| Kendryte K210    | `UR_USE_K210_SHA256`    | `sha256_hard_calculate` (HW)  |

### ESP-IDF component

Add this repository as a component (e.g. under `components/cUR/` or as
a submodule). The bundled `CMakeLists.txt` registers it with
`idf_component_register()` and sets `UR_USE_MBEDTLS_SHA256`:

```cmake
# components/cUR/CMakeLists.txt is already provided.
# In your app's main/CMakeLists.txt just list the component as a dep:
idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "." REQUIRES cUR)
```

Then include headers normally: `#include "ur_encoder.h"`.

### Kendryte K210 (Krux / MaixPy)

MaixPy's port does **not** wire components via USERMOD/`py.mk`; it
uses a hardcoded `register_component()` + `append_srcs_dir()` block in
`components/micropython/CMakeLists.txt`. The `-DUR_USE_K210_SHA256`
flag must be added inside that block, or the link fails with
undefined references to `ur_bundled_sha256_*`:

```cmake
# Inside the CONFIG_MAIXPY_BC_UR_ENABLE block:
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUR_USE_K210_SHA256")
```

This mirrors how `CONFIG_MAIXPY_SECP256K1_ENABLE` sets its own flags.
Without this flag the bundled SHA implementation is selected, but its
symbol names now live under the `ur_bundled_sha256_*` namespace
precisely to surface the missing flag as a link error rather than a
silent runtime hardfault.

### MicroPython wrapper (`uUR.c`)

`uUR.c` at the repo root wraps the C library as a MicroPython module
exposing `URDecoder`, `UREncoder`, `UR`, and `FountainEncoder`. Krux
consumes this file as-is (byte-identical). For vanilla MicroPython
USERMOD builds the repo-root `micropython.mk` provides the wiring;
MaixPy K210 ignores it and uses the CMakeLists route above.

## Repository layout

```
src/
  ur_encoder.c, ur_decoder.c, ur.c   # top-level UR API
  fountain_encoder.c, fountain_decoder.c, fountain_utils.c
  bytewords.c, crc32.c
  sha256/                            # bundled SHA-256 + compat shim
  types/                             # CBOR + per-type encode/decode
tests/
  test_ur_*.c                        # one per UR type + negative
  test_harness.{c,h}, test_utils.{c,h}
  test_cases/                        # fixtures organized by type
scripts/coverage.sh                  # wrapped by `make coverage`
uUR.c                                # MicroPython wrapper
CMakeLists.txt                       # ESP-IDF component
micropython.mk                       # vanilla MicroPython USERMOD
```

## License

BSD-2-Clause Plus Patent License. Copyright © 2025 Krux Contributors.
See [LICENSE](LICENSE) for full text.

Independent implementation written using foundation-ur-py as a
reference for validation; does not derive from that codebase.
