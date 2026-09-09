# Flash encryption rollout: how the one way burn gets earned

Status: build profiles complete INCLUDING the secure boot pass, hardware
acceptance not started. Written 2026-08-06; secure boot v2 folded into the
release recipe 2026-08-26; key custody and the digest slot model settled
2026-09-09, after a review found the recipe could sign an RSA build with the
ECDSA key and this spec promised spare slots the chip revokes.

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
- **Secure boot key custody: settled 2026-09-09, builder-owned, three keys.**
  This once read "the existing update key, one root" -- the same secp256r1
  key the SD update story rests on, its digest anchored in eFuse, no second
  key to lose. That is not available on this chip. Hardware secure boot with
  ECDSA is errata'd on the ESP32-P4: IDF's own bootloader Kconfig defaults
  `SECURE_BOOT_V2_ECDSA_INSECURE` to y for this target, "not functional for
  certain input vectors", and offers the scheme only behind
  `SECURE_BOOT_INSECURE` + `SECURE_BOOT_V2_FORCE_ENABLE_ECDSA` -- a known
  vulnerability, switched on by hand, on a board that can never be reflashed.
  Not on a signing device.

  So hardware secure boot here is **RSA-3072**, and the recipe follows the
  chip rather than the key. The three questions that had to be answered
  before a burn, and the answers:

  1. **Whose root.** The builder's. Whoever runs the release recipe mints
     three RSA-3072 keys (`KISS_SB_KEYS`; the recipe prints the command) and
     the board trusts those and nothing else, forever. This is the DIY answer
     to the question `security-plan.md` left open: a signer whose owners
     build and flash their own firmware cannot be told to run only ours. It
     is the shape the post quantum key already has, and a lost root costs
     what it always did: every board burned with it frozen on its last
     firmware.
  2. **The SD update lane.** Under secure boot the app's signature block IS
     the update check, judged against the burned digests rather than the
     running app's own block. So a burned board's update is its builder's own
     output of the release recipe, signed with key 0 of the same root, and
     nothing published for the beta installs on it. `kiss_fw.c`'s
     `CONFIG_SECURE_BOOT` branch already anchors there.
  3. **`docs/installer/kiss_ota_pub.pem`** stays exactly what it is, the beta
     lane's key. No RSA public key is published, because there is no single
     one to publish.

  How the ECDSA mistake was found is worth keeping: forcing
  `CONFIG_SECURE_SIGNED_APPS_ECDSA_V2_SCHEME=y` looked like it worked -- the
  symbol was set, the app and the bootloader were signed with the secp256r1
  key, and every signature check in the build printed PASS. Each of those
  checks verifies an image against the key it was signed with. Not one asked
  what the FIRMWARE expects, and the built config said RSA. A board burned on
  that pair refuses the exact images that produced it, on first boot. It
  nearly happened twice: the variable meant to hold the RSA key only lifted
  the UNSIGNED guard, and the OTA key signed on. `tools/check_sig_scheme.py`
  now reads the block after every signature and holds it against the recipe,
  and that variable is refused outright.
- **Key revocation: burn three digests, none spare.** This used to promise
  "burn one, keep two slots spare" for a later rotation, and the promise was
  false: IDF revokes every digest slot the bootloader's signature sector did
  not fill, on the same first boot, and the only Kconfig that keeps a slot
  open (`SECURE_BOOT_ALLOW_UNUSED_DIGEST_SLOTS`) lives behind
  `SECURE_BOOT_INSECURE`, which this recipe asserts off. "Aggressive key
  revoke stays off" is a different knob and preserves nothing. So rotation is
  pre-provisioned: the bootloader carries all three keys, first boot burns all
  three digests, the app carries key 0, and a rotation is an update signed
  with the next key that revokes the one before it
  (`esp_ota_revoke_secure_boot_public_key`). Three is what the chip holds and
  it is the whole rotation budget for the life of the board. The recipe
  asserts three RSA blocks on the bootloader and the slot option off.
- **Two lanes, because the burn and an update need different keys.** The
  recipe used to demand the whole root on every run, including a routine
  release that only re-signs the app. That put the two spares on the build
  machine every time, and the spares are the only thing that recovers a fleet
  whose everyday key leaked: a rotation is an update signed with the next key.
  A backup kept on the machine it is meant to survive is not a backup.

  So `KISS_ENC_UPDATE=1` builds the app alone, with the key named by
  `KISS_SB_KEY_INDEX`, and keys 1 and 2 stay offline. That lane signs no
  bootloader, prints no flash or eFuse recipe, and deletes the unsigned
  bootloader from its build directory. The deletion is the point, not
  tidiness: a bootloader signed with one key burns one digest, revokes the
  other two slots on the same first boot, and leaves a board that can never
  rotate again — the exact failure the three-key root exists to prevent, now
  reachable from a lane that holds one key. The build asserts the file is
  gone.

  The index only means something if the slot order is fixed, and nothing
  recorded it: three key files on one disk, where a restore or a rename could
  quietly change which key the fleet calls key 1 with no check noticing. The
  burn now writes the order down once, as fingerprints of the public halves,
  and both lanes refuse to sign as key *N* with a key that record does not
  call key *N*.
- **ROM download mode: the secure subset, confirmed.** Enough for a stranger
  to erase a board and prove it erased, never to read or reprogram it. Was an
  inherited default; the build script now forces it so no IDF default can
  unmake it.
- **Anti rollback: armed at secure version 0.** The check has to live in the
  burned bootloader from birth or it never exists; at version 0 it burns
  nothing and refuses nothing, until a release deliberately bumps the
  version. The app half already existed: `kiss_fw_mark_valid` confirms a
  trial slot only after the signer proves it can sign.

  **The number is a tracked file, and moving it is its own decision.** It
  used to be a literal in the recipe, which meant the machinery was armed and
  could never be used: no release could raise it, and a bootloader under
  secure boot cannot be replaced later to fix that. It now comes from
  `SECURE_VERSION` at the repo root, and the recipe refuses a value that went
  down against the last release tag.

  It moves only to retire a firmware version with a hole worth closing
  permanently, and never as part of a routine release. The counter burns the
  first time a higher version BOOTS, on every board that runs it, and it does
  not come back: every earlier release is refused from then on, including the
  one an owner wanted to roll back to. The eFuse field is a fixed width, so a
  chip has only so many of these, ever. Raising it is a commit of its own,
  whose message says which release it retires, and it is a DEVICE TEST before
  it ships.

## Stage 3, the burn, on a dedicated board, both together

One board, one pass, flash encryption release mode and secure boot v2 in the
same image. The partition table already left bootloader headroom for exactly
this, which is why the table sits at `0x10000` rather than `0x8000`.

Treat this board as the acceptance article, not a development board. It will
never take another build. Before it: `tools/check_efuse_fresh.py --fresh` must
PASS on this board and FAIL on the rehearsal board, because erasing flash
proves nothing about fuses and the recipe used to say it did. Test on it:
first boot burns and comes up, Settings shows encryption calm (the rehearsal
board shows it amber: encrypted, not locked), the wallet survives a power
cycle, a sealed card opens, a wipe works, the builder's own next release
installs from a card while the beta's signed image and an unsigned one are
refused, a power cut mid-update rolls back, and a deliberate attempt to
reflash it over the cable fails the way the header promises.

That installed release must be one the UPDATE lane built: signed with key 0
alone, with no bootloader beside it. It is the artifact every release after
the burn will be, and the burn lane's output is not a substitute for it in
this test. Nothing on a desktop can answer whether a board whose bootloader
carries three keys accepts an app carrying one.

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
