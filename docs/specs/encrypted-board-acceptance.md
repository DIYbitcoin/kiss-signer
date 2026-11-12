# Encrypted board acceptance: the hardware steps

Status: written 2026-11-12, not run. No board has been through any step below,
so nothing here is a hardware result yet. Each step says what to record; a
step counts as passed only when its record says so.

This is the step by step form of Stages 1 and 3 in
[`flash-encryption-secure-boot-rollout.md`](flash-encryption-secure-boot-rollout.md),
which says why each step exists. It needs two boards of the same model:

- the **rehearsal board**, which takes DEVELOPMENT mode flash encryption and
  stays reflashable over the cable, though its flash encryption key is
  burned for good;
- the **burn board**, fresh, which takes RELEASE mode flash encryption and
  secure boot v2. Ordinary plaintext cable flashing is unavailable, but writes
  and erases remain possible. Previously valid device-specific ciphertext can
  still boot when restored at the same offsets if its signature and security
  version remain accepted.

Use a throwaway test seed on both, never one that holds or will hold funds.
Every command runs from the repo root. `PORT` is the board's serial port.

## Part A, the rehearsal board

A1. Fuses before anything is flashed.

```bash
python3 tools/check_efuse_fresh.py --reflashable -p PORT
```

Pass: exit 0. Record the printed summary.

A2. Build the rehearsal image from a clean, committed tree.

```bash
KISS_ENC_REHEARSAL=1 bash tools/build_encrypted_release.sh
```

Pass: every check line prints PASS, including "no GDB stub and no core
dump". Record the image hashes it prints.

A3. Flash with the exact command the build printed, then power cycle. Record
the boot time against the beta on the same board, and the app size line the
build printed.

A4. Settings shows encryption on but not locked (amber). Record what it says.

A5. FLASH mode. Create a wallet with the test seed, note its fingerprint,
power cycle. Pass: same fingerprint, no restore asked for.

A6. Seed residue against a flash dump.

```bash
bash tools/nvs_seed_check.sh before
```

Then move FLASH to SD CARD on the device, then:

```bash
bash tools/nvs_seed_check.sh after
```

Pass: neither run reports a recovery ID for the test seed. On the beta the
"before" run finds it; here the dump must be ciphertext. Record both outputs.

A7. SD CARD mode. Seal the test seed to a card, power cycle, open it. Then:
boot with the card out, wipe with the card out, and move between all three
storage modes in both directions. Pass: each one ends where the screen says it
did. Record any screen that differs.

A8. Updates on the rehearsal board. Put an image signed with the SD update key
on a card: it installs. Put an unsigned image on a card: it is refused and the
running firmware still boots.

Part A passes only when A1 to A8 all pass. Part B does not start before that.

## Part B, the burn board

B1. Fuses before the burn, on both boards.

```bash
python3 tools/check_efuse_fresh.py --fresh -p PORT
```

Pass: exit 0 on the burn board AND exit 1 on the rehearsal board. A rehearsal
board that passes means the checker is wrong; stop.

B2. Build the release image with all three secure boot keys present.

```bash
bash tools/build_encrypted_release.sh
```

Pass: every check line prints PASS, including three RSA blocks on the
bootloader, "no digest slot left open", "eFuse read protection locked after
the burn", "no GDB stub and no core dump" and "a panic reboots silently".
Record the hashes and the key fingerprints the burn writes down.

B3. Flash with the exact command the build printed, once, then power on.
Record the serial log of the first boot. Pass: it comes up to the game screen
and Settings shows encryption locked (calm, not amber).

B4. Fuses after the burn. espefuse cannot read a board in secure download
mode, so ask the ROM instead:

```bash
uvx --from esptool==5.3.1 esptool --port PORT get-security-info
```

Pass: secure boot enabled, flash encryption enabled, secure download mode
enabled. Record the whole output.

B5. The cable no longer reads flash.

```bash
uvx --from esptool==5.3.1 esptool --port PORT --no-stub read-flash 0x11000 0x6000 nvs.bin
```

Pass: esptool refuses. Record its message. Delete `nvs.bin` if anything was
written. This shows esptool knows the board is in secure download mode, which
B4 already showed; esptool refuses before it asks the chip, so it is not a
test of the ROM's own refusal.

B6. Repeat A5 and A7 on this board. A6 cannot run through secure download
mode because flash reads are unavailable; B5 only checks esptool's refusal.
The rehearsal board's A6 is the residue result.

B7. Updates on the burn board. Build the next release with the update lane,
key 0 alone:

```bash
KISS_ENC_UPDATE=1 bash tools/build_encrypted_release.sh
```

Pass, each on a card: that image installs; the beta's signed image is refused;
an unsigned image is refused; pulling power mid install leaves the previous
firmware booting.

B8. Last, because it can end the board: a cable write of a modified app. Only
run it once B1 to B7 are recorded. Pass: the board never boots the modified
image. Record what esptool and the board print.

## What this does not test

Glitching, decapping and side channels; the panic paths (the build asserts the
settings, nothing here crashes the board on purpose); and anti rollback past
secure version 0, which is its own device test when a release raises it.
