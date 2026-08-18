# Camera audit: the camera path, checkable off the device

Status: design approved 2026-08-03, in implementation.

The dice path can be recomputed on any computer, and the WHY THREE SOURCES
card sends doubters there. The camera path has no equivalent: its fold consumes
subsamples of twenty odd frames, then mixes with the chip TRNG and the owner's
taps, and none of that can be replayed off the device. This spec adds the
missing audit: a proof run that captures one frame, writes those exact bytes to
the SD card, and derives twenty four throwaway words from the hash of that file
alone.

## The claim, exactly

The proof run computes

    hash  = SHA256(frame)            // the 1,875,328 raw RGB565 bytes
    words = BIP39(hash[0:16])        // 12 words, standard checksum, English list

and shows both, having first written `frame` byte for byte to `kiss-proof.bin`
and the offline checker page beside it as `kiss-verify.html`. The owner then
checks, on any computer they trust, any of three ways:

1. Open the card's own `kiss-verify.html` and drop the file on it: the page
   computes the hash and the words in the browser, offline, and the check is
   comparing its 12 words against the 12 on the device screen. The page is
   stateless by design — a baked-in claim could survive an interrupted run and
   swear to a hash from a run before. For the independent version of the same
   check, open `verify.html` from the repo or the site instead (a `#h=` link
   pre-fills a comparison target and renders MATCH or MISMATCH).
2. `shasum -a 256 kiss-proof.bin` equals the hash on the screen, and any BIP39
   tool fed the FIRST 16 BYTES of that hash as entropy produces the same 12
   words.
3. `tools/verify_proof.py` does both of step 2's halves in one command.

If both hold, the device's SHA256 and its BIP39 wordlist and checksum are
honest for camera bytes, end to end — the proof path calls the same
`kiss_seed_from_entropy` the real wizard calls, so a device that passes the
proof and lies about a real seed has to be lying somewhere else.

## What it does not prove

Where else it could lie, named so nobody reads more into the proof than it says:

- The entropy fold (`ent_frame`'s subsample and chain) is not exercised. The
  proof hashes one whole frame; the real path hashes a moving lattice of many.
- The TRNG remains unauditable. That is why the real seed cannot be verified
  from any export, and why the proof derives from the frame alone.
- Firmware that special cases the proof path passes the proof and cheats the
  wizard. The proof raises the cost of a lie; reproducible builds are the answer
  to that one, not this screen.

Same caveat class the WHY card already concedes: all three sources are made by
this device.

## The page on the card is not the referee

`kiss-verify.html` is written by the device being audited: firmware that lies
about the hash can ship a page that repeats the lie. The card copy exists so
the casual check costs one double click; the independent check fetches the
page from the repo or the hosted URL on a machine the owner trusts. The page
says this about itself, above the fold.

Three implementations of `BIP39(SHA256(file))` now exist — C in the firmware,
Python in `tools/verify_proof.py`, JS in the page — and drift between them is
an attack surface. All three are pinned to the same independent vector:
`sim/test_proof.c` pins the C, CI runs the page's actual script body under
Node against the identical pattern and words (`tools/check_verify_page.mjs`),
and a second CI step regenerates the embedded C array from `docs/verify.html`
and diffs it against the committed copy (`tools/gen_verify_page.py`).

## Burned words

The words the proof shows are a real, valid BIP39 seed — and they sit on the
SD card in cleartext, because the file they derive from is the point. So the
flow treats them as burned from the moment they exist:

- They live in their own buffers (`s_pf_w`), never the wizard's (`s_w`), so no
  code path can quiz, stage or store them.
- Every exit from the proof screens wipes them.
- The words screen says it in stop colour: burned, on the card in the open,
  never for funds.

## Flow

Entry is the CAMERA AUDIT pill on the WHY THREE SOURCES overlay — the exact moment
the reader is told the camera path cannot be checked is the moment they can now
check it.

    WHY overlay -> CAMERA AUDIT -> [SD gate] -> viewfinder + CAPTURE
                -> one frame frozen on screen -> SD writes (atomic, verified:
                   the frame, then the checker page)
                -> hash + check/burn cards -> 12 words -> back to the wizard

The frame frozen on the preview IS the captured frame: capture pauses the video
on exactly the buffer that was copied, so what the owner saw is what got
hashed. Both SD writes are `platform_sd_write_atomic`, whose read back and byte
compare is the file half of the proof property. The frame goes first because it
is the artifact; if the page write then fails, the run deletes the committed
frame (best effort) and reports the same SD failure, so an error screen never
has files behind it — a truly dead card can refuse the delete too, but that
screen showed no hash, so the leftover frame contradicts nothing.

The real wizard is untouched: three sources, same mix, same screens.

## Testing

**Host.** `sim/test_proof.c`, against a pinned vector computed independently
(python hashlib + the vendored wordlist file):

- a fixed 1,875,328 byte pattern hashes and derives to pinned values through
  the real libwally;
- the file on the (host) card is byte identical to the input and re-hashes to
  the same digest;
- the checker page lands beside it, byte identical to the embedded array;
- SD write and rename faults surface as errors and leave no file of either
  name — including a fault aimed at the second (page) write, which must pull
  the committed frame back out;
- null or empty frames are refused.

**CI.** `tools/gen_verify_page.py` regenerated and diffed (page/array drift),
and `tools/check_verify_page.mjs` runs the page's script body under Node
against the same pinned vector (JS drift).

**Device.** Required; the host cannot see the camera or real SDMMC.

## Device test verdict

**DEVICE TEST: REQUIRED.**

1. Full flow from the WHY overlay through words and back; the wizard's camera
   restarts on a fresh meter.
2. `shasum -a 256` of the card's file equals the on screen hash, and
   `tools/verify_proof.py` reproduces hash and words. This one check exercises
   the cache sync, the exact length rule and the atomic write together; no gate
   compiles `camera_spike.c`, so only hardware can run it.
3. The frozen preview matches the saved file (photograph a clock).
4. A card pulled mid write lands on the fail screen with no partial
   `kiss-proof.bin` and no `kiss-verify.html`.
5. The 1.83MB PSRAM proof buffer allocates with a wallet open.
6. `kiss-verify.html` opened from the card itself, offline in a browser, fed
   the card's file, shows MATCH and the same 12 words as the screen.
7. The AUDIT pill on the entropy screen's action row reaches the same flow as
   the one on the WHY overlay, and hands the camera over cleanly (no second
   stream onto a framebuffer the entropy preview is still writing).

## Why sixteen bytes and not thirty two

The hash is 32 bytes and 32 bytes of entropy is 24 BIP39 words, which is what
this derived for as long as it existed. This signer does not make 24 word seeds:
every creation path pins 12 (`method_cam_cb`, `method_dice_cb`,
`method_cards_cb` in `main/kiss_setup.c`) and 24 exists only on RESTORE, to
match paper an owner already has.

So the one screen whose entire job is demonstrating how this device turns
entropy into seed words was demonstrating it with a seed the device cannot
produce, and the obvious lesson for a reader was that 24 is on offer. It is not.
The recipe takes `hash[0:16]` and the checksum is the top 4 bits of
`SHA256(hash[0:16])`, which is what BIP39 specifies for 128 bits.

What that gives up, recorded because it is a real loss: the audit no longer
demonstrates that the other 16 bytes of the hash went unused. An owner who
rederives still gets the same 12 words from the same file, and a page teaching
a capability the signer does not have was the worse of the two.

The recipe is written in four places and they move together or the audit stops
verifying:

- `main/kiss_proof.c` and `WPROOF_ENTROPY_BYTES` in `main/kiss_proof.h`
- `docs/verify.html`, embedded into `main/verify_page.c` by
  `tools/gen_verify_page.py` and written onto the card beside every frame, so a
  card carries the checker that matches it and older cards keep verifying
- `tools/verify_proof.py`
- this file

Three independent implementations are pinned to one vector: `sim/test_proof.c`
(libwally), `tools/check_verify_page.mjs` (the page's own JS under Node) and the
Python above.
