# bitsquiggle32

BitSquiggles turns a 32 bit value into a 16x22 binary pattern meant to be
compared by eye. KISS draws the wallet's master fingerprint with it, beside the
hex and never instead of it.

Upstream: https://github.com/maggo83/BitSquiggles
Licence: Grug 2-Clause (do what want; not sue grug), recorded in
`THIRD_PARTY_NOTICES.md`.

## The pin

    commit b004deb2349a29e3c05441ac758445f7491d66ac   2026-07-22
    path   c/bitsquiggle32.h   sha256 e66b7b470d0b5f6558b0db40305760e3293c4098bec2524340749ebaaba2c4d6
    path   c/bitsquiggle32.c   sha256 6bdedae5f67d24aa03fda0834af7538bc4d50b93cffdb39ec79be4ea70e3231d

Both files are byte-identical copies. Nothing is patched, so anyone can verify
the vendored copy against upstream with those hashes and nothing else.

## The encoding is frozen

`sim/test_squiggle.c` asserts the full 16x22 raster for four known fingerprints.
That test is not a nit: the pattern a wallet draws is something owners learn by
sight, so if a firmware update changed it, every owner who learned their picture
would see the wrong one on the screen where a false alarm costs the most. If
upstream ever changes the mixer or the copy families, KISS stays on this pin.

Bumping the pin means the vector fails. That failure is the point. Do not
re-bless it without deciding, deliberately, to repaint every existing wallet.

## What is used

`bitsquiggle32_pixels()` and the `Bitsquiggle32Style` enum. That is all.

The rest of the API (smooth blobs, edge tables, mode helpers) is dead weight in
this image and stays anyway, because a clean copy is what makes the hashes above
worth printing.

`bitsquiggle32_pixels()` reaches `derive_colors()`, which does `double` maths
through `pow`/`cos`/`sin`. KISS ignores the colours it produces — a squiggle is
drawn in `wt_accent()` so the theme keeps meaning what it means — and the cost is
paid once per screen, not per frame, so soft-float on the ESP32 never shows.

## Not a security boundary

32 bits. Upstream says plainly that a targeted collision is computationally
feasible and that this is not a hash, a checksum, or a fingerprint derivation
function. It catches the accident (a mistyped passphrase opening a wallet the
owner did not mean to open), not an attacker. Every screen that draws one also
shows the hex.
