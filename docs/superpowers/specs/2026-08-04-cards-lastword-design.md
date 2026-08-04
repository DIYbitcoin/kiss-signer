# MY OWN WORDS — cards entropy with a computed last word

Date: 2026-08-04. Status: approved.

## Problem

The DIY paper seed: the owner cuts the BIP39 list into cards, shuffles, draws
blind, and wants a wallet made of those words. The last word of a BIP39
mnemonic is part checksum, so after 11 or 23 drawn words the final word cannot
be drawn — it must be computed. Krux and SeedSigner ship this; KISS Signer's
answer must not add machine randomness to the seed.

## Decisions (user approved)

- Both 12 and 24 word seeds (11 + 1 and 23 + 1).
- The device computes ALL checksum valid last words (128 for 12, 8 for 24)
  and the owner picks one. No TRNG bits enter the seed.
- A full page checksum explainer sits between word entry and the picker:
  marks before words (✓ / ✗ diagram rows), one concrete number
  ("8 of 2048 words fit yours"), two why blocks.

## Shape

Third row on the camera/dice fork → reused 12/24 chooser → intro (draw blind,
never from your head) → reused RESTORE keyboard for 11/23 words → checksum
explainer → candidate picker (paged grid, 16 per page) → the normal
reveal → quiz → store path.

Crypto is one pure module, `main/wallet_lastword.c`, mirroring `wallet_dice.c`:
try each of the 2048 list words in the last slot, keep those
`bip39_mnemonic_validate` accepts. 2048 SHA256s, run once on picker entry.
`s_count` stays 12/24 the whole flow; a helper (`entry_target()`) is the only
place that knows the keyboard stops one word early.

## Threat notes

- Words from memory or songs are guessable; the intro's warn block says so.
  The mode's security rests entirely on the physical draw being blind.
- A malicious device already sees the words in every mode; this mode removes
  the device as an entropy source, which is its whole point. The result is
  verifiable on any offline BIP39 tool, and the candidate set for a given
  prefix is reproducible off device.
- The sim stubs the candidate math; correctness lives in /tmp/kisstest vectors
  and the device test, exactly like dice.
