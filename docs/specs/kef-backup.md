# KEF encrypted backup

The password-locked export. Unlike the sealed blob (sd-seed-storage.md),
which is keyed by a device key that never leaves the box, a KEF envelope is
keyed by a password the owner carries in their head — that is the product:
the QR or the `.kef` file can live in a drawer, a phone photo or a stranger's
pocket and stays ciphertext. KEF (Krux Encryption Format) is a public
cross-project format; a KISS backup opens on a Krux device and vice versa.

## Wire format

All integers big endian.

```
off        len       field
  0          1       len_id, 0..252
  1     len_id       id: the PBKDF2 salt, visible in the clear.
                     KISS writes the master fingerprint, 8 ASCII hex chars.
1+len_id     1       version. KISS makes and opens only 20 (AES-256-GCM).
2+len_id     3       iterations, stored form (below)
5+len_id     N       payload, version 20: iv(12) | ciphertext | tag(4)
```

- key = PBKDF2-HMAC-SHA256(password, id, effective iterations), 32 bytes.
- The password's bytes are used exactly as typed: UTF-8, no normalization
  (the v1 keyboard is printable ASCII, which satisfies that trivially; a
  Krux envelope with a non-ASCII password still opens if the same bytes can
  be produced).
- The tag is the AES-GCM tag over the ciphertext alone — no AAD, no
  padding, no compression — truncated to its first 4 bytes.
- Plaintext convention for a seed: the BIP39 entropy bytes (16 or 32), the
  same convention Krux uses, so either device rebuilds the other's words.
  On restore the decrypted plaintext is fed through `kiss_seed_from_qr`,
  which also accepts a text mnemonic, so both plaintext shapes open.

## Iterations

Stored value `v <= 10000` means `v * 10000` effective (Krux stores its
100,000 default as 10); `v > 10000` is the effective count itself. KISS
writes stored 10.

On open, two guards:

- **Floor, from the reference implementation**: effective below 10,000 is
  refused, so a hostile envelope cannot choose a trivial work factor.
- **Cap, local**: effective above 1,000,000 is refused. The format allows
  up to 100M, which is minutes of PBKDF2 on a 400MHz chip — a 3-byte field
  must not be able to buy that. This is a pure hostile-field DoS guard,
  not an interop tradeoff: Krux's own settings stop at 500,000, so no
  envelope a real device produces is ever rejected by it. An over-cap
  envelope is refused on the LOCKED BACKUP screen before a password is
  asked for, because no password would change the answer.

## Encrypt strict, decrypt vague

Per the format's own rule. Seal produces only version 20 and fails loudly.
Open refuses every other version, every malformed header, every truncated
payload and every wrong password through ONE failure code with the output
buffer zeroed; on screen that is one sentence, and nothing downstream can
tell which check failed. The single deliberate distinction: an envelope that
is structurally KEF but not openable here (other version, over-cap) is
refused before the password prompt — that is structure, not an oracle.

## Why the GCM is homegrown

The house rule is that the exact sources that ship also run in
/tmp/kisstest. libwally (the only crypto both builds share) has no GCM, and
mbedtls has no host build here — so GCM is built in kiss_kef_crypto.c from
what libwally has: CTR keystream via single-block `wally_aes` ECB, plus a
bit-serial GHASH with no data-dependent lookups or branches. Payloads are
tens of bytes; PBKDF2 dominates the cost.

How it is proven:

- the published GCM spec vectors (zero-key cases included) pin the
  keystream and the full 16-byte tag;
- four golden envelopes produced by the reference implementation
  (src/krux/kef.py driving an independently validated AES) open byte for
  byte, and seal — IV pinned by a host-only test seam — reproduces them
  byte for byte (sim/test_kef.c);
- tools/kef_roundtrip.py opens and rebuilds the same goldens with a THIRD
  implementation, a dependency-free pure-Python AES derived in place from
  the GF(2^8) construction — the C and the Python can only agree by both
  being right;
- the fuzz harness (ASAN/UBSAN) runs random multi-bit damage (success only
  ever yields the true plaintext, rejection always leaves a zeroed buffer)
  and holds the restore router property below.

## The restore router

A scanned or file-loaded payload is offered to `kef_sniff` before the seed
QR parser. The two can never both claim an input:

- lengths 16 and 32 are refused by the sniff outright — CompactSeedQR
  territory, and no envelope that small holds a seed;
- every byte of a text mnemonic or a numeric SeedQR is `>= 0x20`, and every
  KEF version byte is `< 0x20`, so the version-byte check structurally
  excludes them.

Unit tests pin all four seed QR shapes to sniff = 0, and the fuzz harness
asserts no input is ever claimed by both parsers.

## Privacy note

The id is plaintext in the envelope, and KISS writes the master fingerprint
there (Krux parity: it names WHICH keys are inside without opening it, and
it is what the `.kef` file is called). Whoever finds the backup learns the
fingerprint — an identifier, not a key. An owner who does not want that
linkage should not label the backup with it either; a future version can
offer a custom id.

## The 4-byte tag, honestly

Four bytes of authenticator means a random forgery passes one time in 2^32
— per attempt, offline. That is the format's choice (it keeps a seed
envelope inside one small QR) and interop means keeping it. It does not
weaken confidentiality: an attacker without the password learns nothing
either way; the tag only gates what a TAMPERED envelope can make the device
accept, and what it decrypts to is still keyed noise. The device-side
consequence is that decrypt verifies the tag BEFORE any plaintext is
produced, so a forged envelope that wins the 2^-32 lottery still only
yields bytes that must then survive BIP39 validation.

## Files

- `main/kiss_kef.h` — format constants, both APIs, this table's twin.
- `main/kiss_kef.c` — structural half: parse, sniff, header emit. No crypto
  includes; compiled into every build including the UI sim, so the router
  runs the same bytes everywhere.
- `main/kiss_kef_crypto.c` — PBKDF2 + GCM + the seed-level seal; firmware,
  kisstest and kissfuzz; stubbed in sim/sim_main.c like every crypto seam.
- `sim/test_kef.c`, fuzz section 8 in `sim/test_fuzz.c`.
- `tools/kef_roundtrip.py` — the off-device half of the interop proof.

On the card the backup is `<FINGERPRINT>.kef`, raw envelope bytes, written
with `platform_sd_write_atomic`. On the glass it is a binary QR of the raw
envelope (`qrcodegen` byte mode), which is what Krux scans.
