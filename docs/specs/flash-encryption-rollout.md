# Flash encryption rollout: how the one way burn gets earned

Status: build profiles complete INCLUDING the secure boot pass, hardware
acceptance not started. Written 2026-08-06; secure boot v2 folded into the
release recipe 2026-08-26, when the Stage 2 choices below were settled.

Two eFuse burns stand between the beta and a signer that protects a seed at
rest: flash encryption and secure boot v2. Both are permanent. This spec is the
order they happen in and what has to be true before each one, because the usual
mistake is not doing them wrong, it is doing them in a sequence that cannot be
walked back.

## The constraint everything else follows from

After the first boot, serial reflash is IMPOSSIBLE. The cable and the web
installer never work on that board again.

**This section used to say more than that, and the extra part is no longer
true.** It read: the table carries `nvs`, `phy_init`, `factory` and `nvs_key`,
there is no OTA partition, so the board accepts no further firmware from anyone.
`partitions_encrypted.csv` has carried `otadata`, `ota_0` and `ota_1` since the
SD update work landed, there is no `factory` partition, and
`build_encrypted_release.sh` asserts
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` precisely so the slots it does
have only accept images signed with our key. The layout said updatable while
this spec said frozen.

So the constraint is narrower than it was written, and only one of the three
consequences survives unchanged:

1. **Secure boot cannot be added later.** Unchanged, and it is the real
   constraint. Secure boot requires flashing a signed *bootloader*, and the
   board no longer takes a bootloader. Both burns happen in one pass or the
   second one never happens -- which is why the release recipe now carries
   secure boot v2 itself, rather than deferring it to a pass that could never
   reach a burned board.
2. **A shipped encrypted device can be updated, over SD, with a signed image.**
   It cannot be reflashed over the cable. `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`
   is armed at secure version 0: nothing is refused today, and the first
   release that bumps the version can permanently shut the door on the builds
   before it. The lane is gated on the signing key being one we can keep.
3. **Every irreversible choice inside the profile has to be settled first**,
   because the burn is where the argument ends. Still true: the eFuse burn is
   permanent even though the app on top of it is not. Stage 2 below records
   the settlements.

## Stage 1, rehearsal lane, repeat as often as needed

```bash
KISS_ENC_REHEARSAL=1 bash tools/build_encrypted_release.sh
```

`sdkconfig.encrypted-rehearsal` is DEVELOPMENT mode:
`CONFIG_SECURE_FLASH_UART_BOOTLOADER_ALLOW_ENC=y` and
`CONFIG_SECURE_INSECURE_ALLOW_DL_MODE=y`, so the board reflashes and this stage
costs nothing to redo. It exercises everything the release lane does except the
one way part.

What this stage is actually looking for, in rough order of how likely it is to
bite:

- **The partition table moved.** `0x10000` for the table, app at `0x20000`, to
  fit the flash encryption bootloader. Anything that assumed stock offsets
  breaks here and nowhere else.
- **NVS encryption and the `nvs_key` partition.** Plain flash encryption does
  not cover `nvs` data partitions and the words live in NVS, so this lane sets
  `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y`. Seed write, seed read, wipe
  and all three storage migrations go through a different code path than the
  beta uses.
- **The SD device key.** `dkey` lives in the same encrypted NVS. Seal a card,
  power cycle, open it.
- **Boot time and app size.** Encrypted reads are slower and the bootloader is
  bigger. Measure both against the beta before deciding they are acceptable
  forever.
- **The seed residue question**, which this lane answers for free. Run
  `tools/nvs_seed_check.sh` before and after a FLASH to SD CARD move. On the
  beta the old mnemonic is still readable on its NVS page (see
  `docs/security-plan.md`); on this lane the same dump must show ciphertext.

Exit criteria: all three storage modes migrate both directions, a sealed card
survives a power cycle, a wipe leaves nothing, and the residue dump is
ciphertext.

## Stage 2, the irreversible choices, now settled

Settled 2026-08-26, before any board was burned. The reasons live here; the
enforcement lives in `tools/build_encrypted_release.sh`, whose force list and
assertions now say each of these out loud.

- **AES-128 versus AES-256: AES-256.** The old 128 line called itself
  deliberate and deferred the real choice to the secure boot pass; this was
  that pass. The P4 has the 256-bit XTS eFuse scheme and key blocks to spare,
  the key is generated on-device either way, and the first boot fixes the
  size forever. No board was ever burned at 128. Both recipes, so the
  rehearsal rehearses the size that ships.
- **Secure boot key custody: NOT SETTLED, and it was thought to be.** This
  read "the existing update key, one root" -- the same secp256r1 key the SD
  update story rests on, its digest anchored in eFuse, no second key to lose.
  That is not available on this chip. Hardware secure boot with ECDSA is
  errata'd on the ESP32-P4: IDF's own bootloader Kconfig defaults
  `SECURE_BOOT_V2_ECDSA_INSECURE` to y for this target, "not functional for
  certain input vectors", and offers the scheme only behind
  `SECURE_BOOT_INSECURE` + `SECURE_BOOT_V2_FORCE_ENABLE_ECDSA` -- a known
  vulnerability, switched on by hand, on a board that can never be reflashed.
  Not on a signing device.

  So hardware secure boot here is **RSA-3072**, and the recipe follows the
  chip rather than the key. Three things have to be decided before a board is
  burned, and none of them can be revisited afterwards:

  1. **One RSA-3072 root**, generated and held the way the OTA key is. A lost
     key still costs the same and total: every burned board frozen on its last
     firmware.
  2. **Whether the SD update lane moves to it.** Under secure boot the app's
     signature block IS the update check, judged against the burned digest
     rather than against the running app's own block, so a burned board and a
     beta board stop agreeing about what a valid update looks like.
  3. **What `docs/installer/kiss_ota_pub.pem` becomes**, given 2.

  How this was found is worth keeping: forcing
  `CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME=y` looked like it worked -- the
  symbol was set, the app and the bootloader were signed with the secp256r1
  key, and every signature check in the build printed PASS. Each of those
  checks verifies an image against the key it was signed with. Not one asks
  what the FIRMWARE expects, and the built config said RSA. A board burned on
  that pair refuses the exact images that produced it, on first boot.
- **Key revocation: burn one digest, keep two slots spare.**
  `CONFIG_SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS=3`; the bootloader carries one
  signature block, so first boot burns one digest. The spare slots are what
  make a future key rotation possible at all on a board that can never be
  reflashed. Aggressive key revoke stays off.
- **ROM download mode: the secure subset, confirmed.** Enough for a stranger
  to erase a board and prove it erased, never to read or reprogram it. Was an
  inherited default; the build script now forces it so no IDF default can
  unmake it.
- **Anti rollback: armed at secure version 0.** The check has to live in the
  burned bootloader from birth or it never exists; at version 0 it burns
  nothing and refuses nothing, until a release deliberately bumps the
  version. The app half already existed: `kiss_fw_mark_valid` confirms a
  trial slot only after the signer proves it can sign.

## Stage 3, the burn, on a dedicated board, both together

One board, one pass, flash encryption release mode and secure boot v2 in the
same image. The partition table already left bootloader headroom for exactly
this, which is why the table sits at `0x10000` rather than `0x8000`.

Treat this board as the acceptance article, not a development board. It will
never take another build. Test on it: first boot burns and comes up, the wallet
survives a power cycle, a sealed card opens, a wipe works, and a deliberate
attempt to reflash it fails the way the header promises.

## Stage 4, what ships, and when

Not before the answer to "are we willing to freeze this firmware forever" is
yes. Until then the beta keeps shipping `sdkconfig.release`, and
`docs/security-plan.md` keeps saying plainly that a beta device is a hot wallet
with a good interface.

The honest intermediate position is the one the project already takes: offer
the encrypted lane as a build anyone can make and verify, run it on hardware we
control, and say exactly what is and is not protected on the build people
actually download.

## What this does not solve

Secure boot and flash encryption protect a device that is already in the
owner's hands. Neither one covers the release channel: what protects the
firmware between our build and their board is the signed `SHA256SUMS`, the
committed key, and the checks in `.github/workflows/desktop-tests.yml`. See
[`../installer/SIGNING.md`](../installer/SIGNING.md).
