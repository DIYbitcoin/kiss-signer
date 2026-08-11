# Flash encryption rollout: how the one way burn gets earned

Status: build profiles complete, hardware acceptance not started. Written
2026-08-06.

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
   second one never happens.
2. **A shipped encrypted device can be updated, over SD, with a signed image.**
   It cannot be reflashed over the cable and it cannot be downgraded below
   whatever the secure boot pass eventually enforces, but today an older SIGNED
   build installs: `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is deliberately off
   until then. So the encrypted lane is no longer gated on "firmware we are
   willing to freeze" -- it is gated on the signing key being one we can keep.
3. **Every irreversible choice inside the profile has to be settled first**,
   because the burn is where the argument ends. Still true: the eFuse burn is
   permanent even though the app on top of it is not.

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

## Stage 2, settle the irreversible choices

Do this before any board is burned, because none of it can be revisited.

- **AES-128 versus AES-256.** `sdkconfig.encrypted` sets
  `CONFIG_SECURE_FLASH_ENCRYPTION_AES128=y` on a part whose
  `CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES_256` is available. Inherited rather than
  chosen, as far as the config history shows. Decide it deliberately and write
  the reason down here.
- **Secure boot key custody.** Secure boot v2 signs with a private key that
  must outlive every device it signs for. Where it lives, who holds it, what
  happens if it is lost. A lost signing key with `CONFIG_SECURE_BOOT_V2` burned
  means no future firmware for any device that trusts it.
- **Key revocation.** `CONFIG_SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS=3`, so three
  digests can be burned and revoked independently. Decide whether to burn one
  or reserve spares.
- **ROM download mode.** The release lane sets
  `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y` rather than disabling download
  mode outright. Confirm that is the intent with secure boot alongside it.

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
