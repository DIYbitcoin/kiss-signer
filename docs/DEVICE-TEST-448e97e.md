# Device test — develop @ 448e97e

The security audit merge. Every test below exists because something in that
merge cannot be checked without a board: the simulator compiles no camera, no
SD, no NVS erase and no real task stacks.

**Do not flash the encrypted lane.** F16 pins XTS AES-128 and the key size is
burned into eFuse on first boot. Dev lane only.

Nine tests, about 40 minutes. Each says what to do, what you should see, and
what a failure looks like — if you see the failure text, stop and report it
rather than continuing.

---

## Before you start

Prepare the card on the Mac:

```bash
# testnet fixtures the walk uses (multi-recipient, unproven input, silent payment)
bash sim/mk_sd_psbts.sh /Volumes/NO\ NAME

# plus a plain pay + a must-refuse one
bash sim/build_test.sh && /tmp/kisstest /Volumes/NO\ NAME
```

That leaves six `.psbt` files on the card. Then build and flash:

```bash
# in the Docker idf image
idf.py build

# from the host esptool venv
esptool.py -p /dev/cu.usbmodem1101 write_flash 0x0 build/kiss-signer.bin
```

Set the device to **testnet** before signing anything (Settings → Network).

---

## 1. Signing is still deterministic  ⭐ most important

Blinding was added to both secp256k1 contexts, and 9.5 KB of PSBT buffers moved
off the stack. Both touch the signing path. If determinism broke, everything
else is noise.

1. Load `kiss-pay.psbt` from the card, sign it, save the signed file.
2. Lock the device. Unlock. Load and sign **the same file** again, save under a
   different name.
3. Copy both back to the Mac: `cmp file1 file2`

**Pass:** the two files are byte identical.
**Fail:** any difference at all. Report both files.

## 2. A silent-payment input signs

kiss_sp.c has its own secp context that was never blinded before this merge, and
the SP input path had the proven-tick fix.

1. Load `zsp-SPAY.psbt`. Sign it.

**Pass:** it signs, and the signature verifies in your coordinator.
**Fail:** refusal, wrong amount on screen, or a signature the coordinator rejects.

## 3. Amounts that are not proven say so

1. Load `zzz-UNPRV.psbt` and open DETAILS.

**Pass:** the input row shows the crossed-eye mark, not a tick. A tick means an
amount is proven by a previous transaction; this one is not.
**Fail:** a tick, or a green "proven" mark on any row of this file.

## 4. The transaction id is all 64 characters

`wt_group4` was dropping the last character.

1. Sign anything, then open DETAILS.
2. Read the TRANSACTION ID aloud in blocks of four.

**Pass:** sixteen blocks of four. The last block has **four** characters.
**Fail:** the last block has three. That is the bug returning.

## 5. The compared characters match on both lines

1. On the verify screen for a single-recipient payment, look at the recipient
   panel: the full address on one line, the shortened one below it.

**Pass:** the **same last eight characters** are bright on both lines. The
`bc1q` and the block after it are grey.
**Fail:** different characters lit on the two lines.

3. Load `zzzz-MANY.psbt` (many recipients). Scroll to the end of the list.

**Pass:** each address shows its last eight characters larger than the rest of
the address. HOLD TO SIGN stays dead until you have scrolled to the bottom.
**Fail:** HOLD TO SIGN works while recipients are still below the fold.

## 6. A refused transaction shows no total

1. Load `kiss-stop.psbt`.

**Pass:** the refusal reason fills the screen. **No sats figure anywhere.**
BACK returns to the file list.
**Fail:** any amount on screen, especially `0 sats`.

## 7. The camera  ⭐ most changed, least testable elsewhere

The QR parser was in no test binary at all before this merge.

1. Scan a **SeedQR**. It should restore normally.
2. Scan a **multipart PSBT** over UR. It should complete and verify.
3. Scan a **version 25-Q** QR code (generate one at ~1200 bytes, ECC level Q).
   This could never decode before.
4. Scan an **alphanumeric-mode** QR (uppercase letters and digits only).

**Pass:** all four decode. Nothing reboots.
**Fail:** the screen goes black and the device restarts mid-scan — that is the
alignment-spiral watchdog. Note which code did it and keep the image.

## 8. Locking forgets which keys were open

1. Unlock with your passphrase. Note the fingerprint in the corner.
2. Lock (or wait for auto-lock).
3. Open the decoy with the cover word.

**Pass:** the decoy shows **its own** fingerprint, or none. Never the one from
step 1.
**Fail:** the fingerprint from step 1 appears. Stop and report — that is the
decoy admitting the real keys exist.

4. Repeat with the SD card **removed** after unlocking. Same expectation.

## 9. Firmware update from SD

1. Put a correctly signed image on the card, open the firmware screen.

**Pass:** the row reads **"signature first"** with a lock. Not a green tick, not
"checked here" — nothing has been verified at that point.

2. Install it. It should write, reboot, and come up on the new version.
3. Now corrupt one byte of the image and try again.

**Pass:** it refuses at write time and the device still boots the old firmware.
**Fail:** it accepts a corrupted image, or the device does not come back.

4. Pull the card mid-write.

**Pass:** it fails and boots the previous slot.

---

## While you are there: stack headroom

Stack canaries went on for every function in `main/`, which adds to every frame.
The two tight tasks are the LVGL task (20 KB) and the camera task (6 KB).

After a session that has signed and scanned, check the high-water marks over
serial. If either is under about 1 KB free, report the number — the canaries
plus the moved PSBT buffers changed the arithmetic and no gate here can see it.

---

## Reporting

For each numbered test: pass, or the exact screen text plus a photo. Test 1 and
test 8 are the two where a failure means stop and do not continue.
