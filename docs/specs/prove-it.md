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
    words = BIP39(hash)              // 24 words, standard checksum, English list

and shows both, having first written `frame` byte for byte to `kiss-proof.bin`.
The owner then checks, on any computer they trust:

1. `shasum -a 256 kiss-proof.bin` equals the hash on the screen.
2. Any BIP39 tool fed that hash as entropy produces the same 24 words.
   `tools/verify_proof.py` does both steps in one command.

If both hold, the device's SHA256 and its BIP39 wordlist and checksum are
honest for camera bytes, end to end — the proof path calls the same
`wallet_seed_from_entropy` the real wizard calls, so a device that passes the
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
                -> one frame frozen on screen -> SD write (atomic, verified)
                -> hash + check/burn cards -> 24 words -> back to the wizard

The frame frozen on the preview IS the captured frame: capture pauses the video
on exactly the buffer that was copied, so what the owner saw is what got
hashed. The SD write is `platform_sd_write_atomic`, whose read back and byte
compare is the file half of the proof property.

The real wizard is untouched: three sources, same mix, same screens.

## Testing

**Host.** `sim/test_proof.c`, against a pinned vector computed independently
(python hashlib + the vendored wordlist file):

- a fixed 1,875,328 byte pattern hashes and derives to pinned values through
  the real libwally;
- the file on the (host) card is byte identical to the input and re-hashes to
  the same digest;
- SD write and rename faults surface as errors and leave no target file;
- null or empty frames are refused.

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
   `kiss-proof.bin`.
5. The 1.83MB PSRAM proof buffer allocates with a wallet open.
