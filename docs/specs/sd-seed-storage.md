# SD seed storage: the third place a wallet can live

Status: beta7 integration implemented behind a real-device security gate;
encrypted-hardware acceptance still pending. Updated 2026-07-28.

Beta7 names all three choices in setup and in a dedicated Settings chooser:
FLASH, SD CARD and AMNESIC. FLASH keeps words in NVS; AMNESIC keeps them in RAM
for the unlocked session; SD CARD uses the sealed file described below. The
Settings chooser marks the current mode and migrates only while a valid source
is available.

SD CARD is offered on every build. It was previously gated behind flash
encryption, which was backwards: the card already holds the words as a blob
sealed to a device key, so a lost card is inert without any help from flash
encryption, while FLASH mode stored the words in the CLEAR on the same firmware.
Gating the safer option while shipping the weaker one made no sense, so
`wallet_seed_sd_supported()` became unconditionally true in `a9303d0`.

What the encrypted lane adds on top is protection of the device key at rest,
which closes the remaining case: someone holding BOTH the device and the card.
That is a stronger threat than the card loss SD mainly guards against, and it is
still pending hardware acceptance.

Implementation now includes the authenticated sealed-blob layer, SD storage
backend, mode-aware read/write/erase paths, setup and Settings UI, missing-card
unlock prompt, and migration entry point. This is not the same as hardware
acceptance: the encrypted rehearsal matrix at the end of this document must
pass before SD storage is described as ready for funded use.

## What it guarantees, and what it does not

**The card alone is useless.** Lose it, leave it in a drawer, have it taken at
a border: it is ciphertext with no key on it.

**Card plus device depends on the build.** A build made with
`tools/build_encrypted_release.sh` has flash encryption RELEASE and NVS
encryption, so the device key is protected there. A dev build keeps it in
plaintext NVS, recoverable from a flash dump. Therefore the normal firmware
does not merely warn about SD mode: it refuses to select it. Card presence must
never bypass that gate.

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

## Integrated API

- `#define WSEED_MODE_SD 2`
- `storage_read` / `storage_write_keep` / `storage_erase` gain an SD backend and
  dispatch on the current mode. The NVS and simulator file backends are
  unchanged.
- `wallet_seed_move_to(int mode)` is the Settings entry point that migrates an
  existing wallet.
- The setup wizard stages the selected mode and commits it only after the
  complete setup ritual succeeds.
- SD unlock with no card opens INSERT SD CARD / RETRY. It never falls back into
  first-boot setup and never silently changes the selected mode.

## Moving a wallet between modes

One rule governs all of it: **write and verify the destination before erasing
the source.** A failure must leave the words in exactly one place, never zero.

FLASH to SD CARD (`WSEED_MODE_KEEP` to `WSEED_MODE_SD`):

1. read the words from NVS and validate them
2. encrypt, write `kiss-seed.enc`
3. read the file back, decrypt, compare byte for byte
4. only now, erase the NVS words and set mode SD in a single commit

Any failure before step 4: delete the partial card file, leave NVS untouched,
report the failure, mode unchanged.

SD CARD to FLASH: decrypt the card, write words plus mode KEEP in one commit,
verify the readback, then delete the card file. If that last delete fails, say
so loudly: the wallet is now in two places and the user needs to know.

Anything to AMNESIC: erase NVS and the card file, verify both are gone, then set
the mode.

AMNESIC to FLASH or SD CARD: only possible while a session is unlocked, because
that is the only time the words exist at all. Offer it there and nowhere else.

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

Setup and Settings use the same names and explanations:

- **FLASH**: words persist in internal storage
- **SD CARD**: encrypted, device-bound card storage
- **AMNESIC**: words live only for the current session

Settings shows the current selection. On normal unencrypted firmware the SD
row stays visible, disabled and explicitly says that encrypted firmware is
required. Hiding it made the storage model undiscoverable; enabling it would be
unsafe. The other two modes remain usable.

Changing mode is a security action, not a preference toggle. The destination
must be written and verified before the source is removed, failures stay on the
old mode, and destructive moves require deliberate confirmation. Moving an
unlocked amnesic session to persistent storage is possible while its words are
still in RAM. After lock or power-off, the wallet must be loaded again first.

All storage strings ship in all 21 locale files. `gen_i18n.py` hard-errors on a
missing key and the fit checker covers the three-choice screens.

## Automated contract

- encrypt and decrypt roundtrip
- a wrong device key fails the MAC and leaks no plaintext
- corrupt or truncated files rejected, fuzzed at every offset
- all six mode transitions, each with an injected failure at each step,
  asserting the words are never lost and never silently in two places
- WIPE erases the card file and the key, and an old card no longer decrypts
- power cut simulation: abort between every pair of steps and assert recovery
- SD is selectable on normal firmware and creates `kiss-seed.enc` sealed to the
  device key, with the device key itself unprotected at rest until the
  encrypted lane ships
- missing-card SD boot prompts for insertion and retry, never setup
- simulator exercises all three modes without weakening the device gate

Automated tests validate mechanics, not the eFuse/NVS security claim.

## Encrypted real-device acceptance gate

This entire section is deferred from the normal beta7 acceptance run. A real SD
card and a dedicated no-funds board flashed with the DEVELOPMENT encryption
rehearsal are required:

```sh
KISS_ENC_REHEARSAL=1 tools/build_encrypted_release.sh
```

That profile remains serial-reflashable, but its first boot still burns a
flash-encryption key permanently. The board can never return to plaintext
flash. Do not use the final RELEASE profile for this test.

1. Confirm Settings reports flash encryption active and the build includes NVS
   encryption before the SD control becomes selectable.
2. Move a wallet FLASH to SD CARD to FLASH, and confirm the words and
   fingerprint match at the end.
3. Exercise FLASH to AMNESIC, loaded AMNESIC to FLASH, SD CARD to AMNESIC and
   loaded AMNESIC to SD CARD.
4. Boot in SD mode with no card: prompt, not a crash or setup wizard.
5. Pull the card during a write; the verified source must remain authoritative.
6. Fill the card, then try to move a wallet onto it.
7. Corrupt and truncate `kiss-seed.enc`; no plaintext or partial wallet may be
   accepted.
8. WIPE with the card out, then reinsert it: destroying the device key must make
   the surviving file permanently undecryptable.
9. Dump flash/NVS from the rehearsal board and confirm neither the mnemonic nor
   the SD device key appears in plaintext.

Only after every item passes may the encrypted firmware enable SD CARD on a
real device. The normal unencrypted beta must continue to show it disabled.
