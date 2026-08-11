# On-device signature fingerprint

Status: design approved 2026-07-30, not implemented.

KISS signs deterministically, so the same PSBT signed with the same seed on two
independently trusted units produces byte-identical signatures. That is the
Dark Skippy check documented in `security-plan.md` ("Signing is verifiable"):
if the two disagree, one of them chose its nonce. Today performing it means
exporting both signed PSBTs and diffing the bytes. This makes it a glance.

After signing, both signed screens show a short fingerprint of the signature
bytes. Two units that agree show the same code; a user compares a number
instead of a file.

## What it is not

Not a security primitive and not a wallet identity. It is a comparison aid: a
compressed stand-in for "diff the signatures." The signed screen itself carries
no threat-model prose -- it shows a labelled number. The meaning is one tap
away, behind a `?` help chip, and in full in `security-plan.md`.

It is also not the wallet fingerprint. The wallet fingerprint (e.g. 73C5DA0A)
identifies the wallet and appears across the UI; this identifies one signing
result. They must not be confused, so this one carries its own label,
`SIGNATURE`, distinct from the wallet's `FINGERPRINT`.

## The hash: signatures only

A new pure function in `main/kiss_psbt.c`:

    // First 8 lower-case hex of sha256 over every input's signature bytes,
    // concatenated in input order. Writes 8 chars + NUL. Nonzero on a parse
    // failure or a PSBT carrying no signatures.
    int kiss_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                    char out[9]);

It parses the signed PSBT and walks inputs in index order. For each input it
appends, to a running SHA-256:

- every entry in `input->signatures` (the ECDSA partial signatures, each value
  being the DER signature plus its trailing sighash byte), in the order the map
  stores them; and
- the taproot key-path signature, if present: `psbt_fields` integer key `0x13`,
  a 64- or 65-byte value (the same field `sp_tap_key_sig` reads in the tests).

The digest's first four bytes become eight hex characters.

**Why signatures, not the whole PSBT.** Hashing the full signed PSBT would make
two KISS units match, but a different tool that produced the identical
signatures inside a differently framed PSBT would mismatch -- a false alarm on a
check whose entire value is catching a real mismatch. The signature bytes are
the invariant that matters, so the fingerprint is transport- and
framing-independent: anything that signed the same way, KISS or not, agrees.

**Determinism carries over.** Because signing is deterministic, the fingerprint
is deterministic: the same PSBT and seed always yield the same code. That is
exactly the property being surfaced.

The function lives in `kiss_psbt.c`, beside the signing it summarizes, so it
is exercised on the host by `sim/test_crypto.c` rather than being reachable only
through the UI.

## Where it is computed and shown

`do_sign_cb` in `main/kiss_sign.c` (around line 367) already holds the signed
PSBT in `s_out`/`sw` before it branches to the QR or SD exit. It computes the
fingerprint there once, into a file-scope `static char s_sig_fp[9]`, and both
exit screens read it:

- **SD**, `done_screen` (line 313): a single line under the filename (filename
  sits at y=230, the note at y=284), so the fingerprint lands at roughly y=258
  in the 14 px face: the caption `SIGNATURE` and the grouped code `A1B2 C3D4`.
- **QR**, `qr_out_screen` (line 1409): the right column is already dense (part
  counter at y=124, notes at 168 and 201, the EASY SCAN pill at 244, its note at
  304). The fingerprint takes one 14 px line in that column; the exact y is
  settled during implementation against the fit and overlap checks rather than
  guessed here, shrinking or merging an existing note line if needed. It must
  not push any element into the action band, the mistake the comment at
  line 1423 records fixing once already.

If `kiss_psbt_sig_fingerprint` fails (it should not, on a PSBT KISS just
produced), the line is simply omitted -- the screen is still correct, it just
loses an aid.

**The `?` help chip.** Beside the `SIGNATURE` line sits a `wt_help_chip`, the
same control the entropy screen uses at `kiss_setup.c` (`ent_mix_help_cb`) to
explain WHY THREE SOURCES. Tapping it opens a short explainer panel
(`mk_screen` title + `mk_body`, a BACK pill returning to the signed screen).
This is where the meaning lives on the device: a curious user learns what the
code is for without the main screen ever carrying prose, and a user who does not
care never sees it. The explainer says, in a few plain lines, that an identical
code on a second signer they trust means neither device changed the signature --
no Dark Skippy naming, no jargon. Whichever signed screen has room carries the
chip; if the packed QR column cannot fit it, the SD screen's chip is enough,
since both screens show the same code and open the same panel.

## Copy

New keys, English only, following the ADDENDUM convention the entropy strings
use (`tr()` falls back to English for the other locales until a locale pass, so
shipping them needs no translations):

    STR_S_SIG_FP_CAP    = "SIGNATURE"
    STR_S_SIG_FP_HELP_T = "SIGNATURE CHECK"
    STR_S_SIG_FP_HELP_B = a few plain lines for the ? panel: the same code on
                          another signer you trust means neither device changed
                          the signature; a different code means one did. No
                          "Dark Skippy", no "nonce" -- the words a first-time
                          reader can act on.

The caption and the code (eight hex characters, grouped four and four for the
eye) are the whole of the signed screen; the code itself is data and needs no
translation. The explainer body is the only prose, and it lives behind the `?`.
`security-plan.md` carries the full account for anyone who wants it.

## Testing

**Host**, in `sim/test_crypto.c`, on the fixed dev seed:

- `kiss_psbt_sig_fingerprint` over the native signed PSBT equals a value
  computed independently (SHA-256 of the native input's signature bytes, first
  four bytes as hex -- the signature is already the golden `SV_ECDSA_NATIVE`,
  so the expected fingerprint is derivable from it without KISS's own function).
- The fingerprint is stable: signing the same PSBT twice yields the same code
  (determinism, already asserted for the signature bytes, now for the digest).
- The fingerprint differs for a different transaction (e.g. the legacy vs the
  native fixture produce different codes).
- A silent-payment signed PSBT produces a fingerprint over its taproot `0x13`
  signature, confirming the taproot path is included and not silently skipped.

Explicitly not tested on host: the on-screen rendering, which is the device
concern below.

## Device test verdict

**DEVICE TEST: REQUIRED.**

The feature adds content to both signed screens -- a display path -- and its
whole purpose is a visual comparison a person performs on hardware. Host tests
cover the hash; they cannot cover whether the line renders, fits, and reads on
the panel. Flows to exercise:

1. Sign a PSBT from SD: the `SIGNATURE` line appears under the filename, legible,
   not colliding with the note or the action bar.
2. Sign the same PSBT, same seed, on a second unit: the two codes match.
3. Sign a different PSBT: the code differs.
4. Sign the same PSBT twice on one unit: the code is identical (determinism).
5. Sign a PSBT returned by QR: the code appears in the right column without
   pushing the QR card or any pill into the action band, and the code equals the
   one the SD path showed for the same transaction.
6. A silent-payment spend: a fingerprint still appears (taproot signature path).
7. The `?` chip opens the explainer panel and BACK returns to the signed screen
   with the code still shown (no re-sign, nothing torn down).

Passing the host suite is not this verdict and does not substitute for it: it
confirms the hash, not the screen.
