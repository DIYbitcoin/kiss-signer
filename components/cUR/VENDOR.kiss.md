# Vendored: odudex/cUR

- Upstream: https://github.com/odudex/cUR
- Based on commit: `c5f69aa9bc3219542704a0dbaa844ce2b85c7ca9` (the SHA Kern pinned as its
  `components/cUR` submodule, checked 2026-07-04).
- License: BSD-2-Clause-Patent (see LICENSE).
- `uUR.c` / `micropython.mk` are upstream's MicroPython binding, unused here, kept for fidelity.
- `.git`, `.github/` stripped.

## Local changes

This copy is NOT pristine, and an earlier version of this note said it was. `git log --
components/cUR` has each change with its reason. In short:

- CMakeLists.txt builds the bundled `src/sha256/sha256.c` instead of requiring mbedtls
  and Kern's `mbedtls_compat` shim, so identical sources compile on desktop
  (sim/build_test.sh) and on the device.
- fountain_decoder.c: a 96 KiB budget on cached mixed parts, with its accounting and a
  stats hook for sim/test_qr.c; every frame after the first must agree with the first on
  fragment length, seq_len, message_len and checksum; the first frame's header must
  describe a message its fragments can hold; allocation failures are reported, not lost.
- fountain_encoder.c, fountain_utils.c, utils.c, ur_decoder.c: failure paths that free
  what they built, a PRNG scale that cannot land one past the end, and the allocation
  site hooks the out of memory tests use.

## Ported from upstream (2026-09-28)

Upstream has moved far past the base commit, with breaking API changes (a decoder state
machine, float progress) and a rewrite of the same fountain code the local changes
harden. Rather than drop those, the fixes that touch this signer's path, which is
`ur:crypto-psbt` only, were ported onto this copy:

- applied as is: c7d3e9b (refuse a fragment draw past the end of the list), 036f483
  (alias table probabilities accumulated like the reference, which the encoder and the
  decoder must agree on), 453ccf2 (CBOR: repeated map keys and trailing bytes refused),
  7ec70a1 (constructor inputs validated; an empty CBOR byte string decodes).
- ported by hand: e9540eb (CBOR string lengths checked against the remaining input
  before anything is allocated), 0b0437e (seq_num 0 refused; fragment length must be
  exactly ceil(message_len / seq_len)), the work queue half of 3f4a837 (PR303-003:
  grows on demand up to 1024 entries instead of refusing at 8).

Not taken, and why:

- 1343eaf (retry reassembly after an allocation failure): only matters when the heap
  runs out at the moment of the join.
- bad1b3e (stricter `seq` component parsing) and c08b9fa (locale independent character
  classes): no safety effect on this path.
- 09724a0 (bounded cross reduction switched on): fewer frames to finish a decode that
  starts mid loop, but it changes decoding behaviour and wants its own change with a
  measurement. Measured here at 50 to 400 fragments received as mixed frames only:
  about 2 to 3 frames per fragment.
- hd-key, keypath, output, multi-key and binding fixes: types and bindings this signer
  never parses.
