# SD seed storage: the third place a wallet can live

Status: specified, not built. Decided 2026-07-27.

Today KISS has two of the three storage choices, and they are only offered
once, on the first screen of the setup wizard: KEEP (words in NVS) and NOTHING
SAVED (amnesic, words in RAM for the session). `WSEED_MODE_*` has exactly those
two constants. `wallet_seed_set_mode()` exists and is verified, but nothing in
`main/` calls it, so the choice cannot be changed after setup without a wipe.

This adds the third: the words live on the SD card, encrypted to a key that
only this device holds.

## What it guarantees, and what it does not

**The card alone is useless.** Lose it, leave it in a drawer, have it taken at
a border: it is ciphertext with no key on it.

**Card plus device depends on the build.** A release built with
`tools/build_encrypted_release.sh` has flash encryption RELEASE and NVS
encryption, so the device key is protected there. A dev build keeps it in
plaintext NVS, recoverable from a flash dump. The three options must not look
equally safe on a dev build: say so on screen.

No code to remember, ever. The card is a second factor, the way Specter-DIY
does it, not a Krux style user chosen key. The price is portability: if the
device dies the card dies with it, so paper stays the only backup.

The device key lives in NVS behind a single `device_key()` function so it can
be moved into an eFuse HMAC key later without changing the card format. That
upgrade closes the card-plus-device case and is the only reason the indirection
exists.

## Card format

One file at the card root, `kiss-seed.enc`:

```
off  len  field
  0    8  magic "KISSSD01"
  8   16  iv, random per write
 24    4  ciphertext length, little endian
 28    N  AES-256-CBC(k_enc, iv) over the mnemonic, PKCS7 padded
28+N  32  HMAC-SHA256(k_mac) over bytes [0, 28+N)
```

Encrypt then MAC. Verify the MAC before decrypting anything.

```
k_enc = HMAC-SHA256(device_key, "kiss-sd-enc-v1")
k_mac = HMAC-SHA256(device_key, "kiss-sd-mac-v1")
```

`wally_aes_cbc` and `wally_hmac_sha256` are already linked, so device and
simulator run identical code and every path gets a native test. The iv comes
from `esp_fill_random` on device and `/dev/urandom` in the simulator.

`device_key()` reads a 32 byte NVS blob, generating and committing it on first
use. In the simulator it is a file beside the other sim state.

## API

- `#define WSEED_MODE_SD 2`
- `storage_read` / `storage_write_keep` / `storage_erase` gain an SD backend and
  dispatch on the current mode. The NVS and simulator file backends are
  unchanged.
- New `wallet_seed_move_to(int mode)` for the Settings toggle. This is the only
  entry point that migrates an existing wallet.

## Moving a wallet between modes

One rule governs all of it: **write and verify the destination before erasing
the source.** A failure must leave the words in exactly one place, never zero.

KEEP to SD:

1. read the words from NVS and validate them
2. encrypt, write `kiss-seed.enc`
3. read the file back, decrypt, compare byte for byte
4. only now, erase the NVS words and set mode SD in a single commit

Any failure before step 4: delete the partial card file, leave NVS untouched,
report the failure, mode unchanged.

SD to KEEP: decrypt the card, write words plus mode KEEP in one commit, verify
the readback, then delete the card file. If that last delete fails, say so
loudly: the wallet is now in two places and the user needs to know.

Anything to AMNESIC: erase NVS and the card file, verify both are gone, then set
the mode.

AMNESIC to KEEP or SD: only possible while a session is unlocked, because that
is the only time the words exist at all. Offer it there and nowhere else.

## Unlock

SD mode with no card in the slot is not a failure, it is a prompt: insert the
card. No fallback to typing the words, that would defeat the point.

## WIPE

WIPE must erase the NVS words, the card file, and the device key.

Erasing the key is the important part. If the card is not in the slot when the
user wipes, the file on it survives, and destroying the key is what makes that
stray copy permanently undecryptable. The current WIPE copy says "the words go
from this device now", which stays true only because of this.

## UI

The wizard's storage screen goes from two pills to three and needs a relayout:
the current 150 / 264 spacing plus a BACK at 404 has no room for a third. Three
340 wide pills at roughly y=120 / 226 / 332 with their notes to the right.

New English copy:

- button: `ON THE SD CARD`
- note: `your words live on the card. it only works in this KISS.`

Picking SD gates on confirming a paper backup, and points at the restore
rehearsal (`wallet_setup_open_verify`) to actually test it. Losing either the
card or the device means restoring from paper, so that has to be true before
the user can choose this.

Settings gains the same three way choice, wired to `wallet_seed_move_to()`,
which is the part that does not exist at all today.

Three new i18n keys. `gen_i18n.py` hard errors on a key missing from any of the
21 locales, so this either ships with 21 translations or behind the
`i18n_get_lang() == I18N_EN` pattern already used in `wallet_recv.c`, to be
unwound during the locale pass.

## Tests, native first

- encrypt and decrypt roundtrip
- a wrong device key fails the MAC and leaks no plaintext
- corrupt or truncated files rejected, fuzzed at every offset
- all six mode transitions, each with an injected failure at each step,
  asserting the words are never lost and never silently in two places
- WIPE erases the card file and the key, and an old card no longer decrypts
- power cut simulation: abort between every pair of steps and assert recovery

## Device test

Real card required.

1. Move a wallet KEEP to SD to KEEP, and confirm the words match at the end.
2. Boot in SD mode with no card: prompt, not a crash.
3. Pull the card during a write.
4. WIPE with the card out, then reinsert it: the file must no longer decrypt.
5. Fill the card, then try to move a wallet onto it.
