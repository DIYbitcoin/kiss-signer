# Camera Audit Verify Page — Design Spec

Date: 2026-08-04
Status: approved for implementation
Branch context: builds on the CAMERA AUDIT pipeline (docs/specs/prove-it.md): one
frame → `kiss-proof.bin` on the card → SHA256 → 24 burned words.

## 1. Motivation

The camera audit is checkable on any computer — in principle. In practice the
recipe is `shasum -a 256` in a terminal, then finding a BIP39 tool, then a human
comparing 64 hex characters by eye. That is three tools and one error-prone
comparison for the exact user the audit exists to convince.

This spec collapses the check to one gesture: a page that takes the file and
says **MATCH**. Three additions, one artifact:

1. **`docs/verify.html`** — a single self contained page (no network, no
   external CSS/JS/fonts). Drop `kiss-proof.bin` on it; it computes the SHA256
   and the 24 words in pure JS and shows both. If the URL fragment carries
   `#h=<64 hex>`, it compares and renders MATCH / MISMATCH — the human never
   eyeballs hex.
2. **A self verifying copy on the card.** The proof run writes the page as
   `kiss-verify.html` next to `kiss-proof.bin`, with the hash it claimed
   written into the page, so a user with nothing but the card and a browser
   opens it, drops the file and reads the verdict.
3. **The AUDIT entry in the entropy screen's action row**, not only behind the
   "?" overlay: finding out you can test the camera should not require already
   doubting it.

A QR of `verify.html#h=<hash>` shipped on the result screen first and was cut.
The check needs the 1.9MB file, the file is on the card, and a phone that
scanned the code could never finish — so the machine that reads the card is
the machine that checks, and the claim belongs in the card's own copy.

## 2. Threat model

1. **The page on the card is not the referee.** It was written by the device
   being audited; firmware that lies about the hash can ship a page that lies
   the same way. The card copy is a convenience for the casual check. The
   independent check uses the hosted or repo copy, fetched on a machine the
   owner trusts. The page itself says this above the fold, and the spec says it
   in stop colour.
2. **The QR is the device's claim, not evidence.** It moves the comparison from
   a human to the page; it proves nothing by itself. A device that shows a QR
   with a false hash is caught the moment an honest page hashes the real file.
3. **Drift between implementations is an attack surface.** C (firmware), Python
   (`tools/verify_proof.py`) and JS (the page) all implement
   `BIP39(SHA256(file))`. All three are pinned to the same independent test
   vector (`buf[i] = (i*31+7) & 0xFF` over the 1,875,328 frame bytes) by
   `sim/test_proof.c`, and CI runs the page's actual script body under Node
   against that vector, plus a byte drift gate between `docs/verify.html` and
   the embedded C array.
4. **Same concession as the audit itself:** firmware that special cases the
   proof path passes every check here and cheats the wizard. Reproducible
   builds remain the answer to that one.

## 3. Goals / non-goals

Goals: one gesture verification; zero install; works offline over `file://`
from a FAT card; hex comparison done by software; the burned words warning
survives into every surface that shows the words.

Non-goals: verifying the real wizard seed (TRNG mix, impossible by design);
translating the page (English only, like the docs site); any change to the
audit's cryptography or file format; hosting infrastructure beyond GitHub
Pages serving `/docs` when the repo goes public.

## 4. Design

### The page

One file, ~26 KB: inline CSS in the docs palette, the 2048 word English list
(byte checked against libwally's vendored `english.txt` by the generator), a
~70 line pure JS SHA256, and ~15 lines of BIP39. Pure functions sit above a
`typeof document` guard so Node can execute the script body unchanged — the CI
vector check runs the shipped code, not a copy. No WebCrypto: `file://` support
is inconsistent, and a dual path would mean CI tests a path some users never
run.

States: no fragment → drop zone only; fragment, no file → amber "waiting for
the file" with the claimed hash; file present → hash + numbered words + burned
warning card, and with a fragment the MATCH/MISMATCH verdict with both hashes
printed for the record.

### The card copy

`tools/gen_verify_page.py` embeds `docs/verify.html` as `main/verify_page.c/.h`
(committed, CI drift gated, like the i18n tables). `kiss_proof_run` writes it
as `kiss-verify.html` via the same atomic write as the frame, after the frame:
the frame is the artifact, the page is the courtesy. If the page write fails
the run deletes the frame (best effort) and reports the existing SD failure —
the invariant "an SD error leaves no proof file" and the fail copy "nothing was
kept and nothing was made" stay true, and no partial file of either name can
exist.

### The screens

AUDIT RESULT keeps the shape it already had: the hash card, then the why pair
at rule 2's proven geometry (344 wide at 48 and 408). The entry point moves:
`wt_pill_icon` at 368 on the entropy screen's action row, between CAPTURE and
BACK, wearing the frame mark. Its callback hands the camera over the way BACK
does, or the proof screen would open a second stream onto a framebuffer the
entropy preview is still writing.

## 5. Out of scope

- Naming `kiss-verify.html` in translated copy (filenames stay C literals).
- A QR of the words or the seed itself — never.
- Serving the page anywhere but Pages/repo; shorteners or custom domains.
- Signing the page; reproducible builds cover the firmware, git covers the page.
