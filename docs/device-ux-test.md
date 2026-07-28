# Device UX acceptance test

Run this on the physical device with a participant who has not seen the UI.
Do not explain controls or terminology while a task is in progress.

## Setup

- Use a disposable wallet and both MONO and one colour theme.
- Have a coordinator wallet, phone camera and blank SD card available.
- Start each UI task from the wallet home screen. Storage setup and reboot tasks
  state their own starting point.
- Record completion time, first wrong tap, requested help, and the participant's
  answer in their own words.

## Tasks and pass criteria

1. **Receive normally.** Ask the participant to show an address for a new
   payment and explain what they should do for the next payment. Pass when they
   choose an address without coaching and say that reuse links payments.
2. **Use Silent Payments.** Ask them to open the Silent Payment address, reveal
   the complete text, fold it again, and scan its enlarged QR with the phone.
   Pass when both text controls are found on the first attempt and the QR scans
   in under five seconds at arm's length.
3. **Explain SCAN KEY.** Before revealing anything, ask what the coordinator can
   and cannot do. Pass only when the answer includes: finds payments, cannot
   spend, and keeps seeing those payments. A tap or short hold must not reveal
   the key; the deliberate hold must.
4. **Zoom every QR.** Check the receive, Silent Payment, pairing, private
   scan-key, and signed-transaction QRs. Pass when each opens from the QR itself,
   closes predictably, and returns to the exact prior state.
5. **Check non-colour cues.** In MONO, ask which address characters should be
   compared and identify the privacy reminder and every anonymous help target.
   Pass when underlines/icons carry the meaning without relying on colour and
   each target responds on the first tap.

## Normal beta7 storage acceptance

Use only the normal, unencrypted beta firmware and a disposable seed. These
tests deliberately do not claim that FLASH protects secrets from physical
extraction.

6. **Understand the three choices.** Open the storage chooser once during setup
   and once through **SETTINGS → STORAGE**. Ask where the words live for FLASH,
   SD CARD and AMNESIC. Pass when the participant says: internal storage,
   encrypted device-bound card, and current session only. The current mode must
   be visibly selected in Settings.
7. **Keep a FLASH wallet.** Create or restore a disposable wallet in FLASH,
   record its fingerprint, lock, power-cycle and unlock again. Pass when the
   same fingerprint returns and the UI does not imply that normal beta flash is
   encrypted.
8. **Move FLASH to AMNESIC.** From the disposable FLASH wallet, choose AMNESIC
   and complete its destructive confirmation. Lock and return through the KISS
   gesture. Pass when KISS asks to load the wallet, no stored seed silently
   reappears, and loading the same words and passphrase restores the recorded
   fingerprint.
9. **Persist a loaded AMNESIC session.** While that restored amnesic session is
   still unlocked, move it to FLASH, then lock and power-cycle. Pass when the
   same fingerprint unlocks from FLASH. In a second run, lock the amnesic
   session before moving it: KISS must ask to load the wallet again rather than
   claim it migrated words that are no longer in RAM.
10. **Take a wallet to the card, and prove the card alone is inert.** Visit the
    chooser with no card inserted: choosing SD CARD must fail with a readable
    reason and must not change the current mode. Insert a blank card, move the
    disposable wallet to SD, then lock and power-cycle. Pass when the same
    fingerprint unlocks with the card in, and when unlocking with the card
    removed asks for the card rather than silently falling back to another mode.
    Then read `kiss-seed.enc` on a computer: pass only when nothing in it
    resembles the recovery words.
11. **Read the last line.** On every screen the participant reaches, ask them to
    read the last line of content aloud. Pass only when every glyph is fully
    visible. Fail on any half rendered row, any text overlapping other text, and
    any list whose final row is sliced by the screen edge. Repeat on the sign
    screen with a PSBT that fires two cautions at once.

## Deferred encrypted-SD acceptance

SD storage itself is part of beta7 acceptance, in task 10. What is deferred is
the flash-encrypted lane on top of it, which is what protects the device key at
rest and so closes the case where someone holds both the device and the card.
Do not flash irreversible RELEASE-mode encryption for this checklist. Its
interrupted writes, wipe with the card absent, corrupt files, and flash-dump
checks require a dedicated no-funds device running the DEVELOPMENT encryption
rehearsal. Follow
[`specs/sd-seed-storage.md`](specs/sd-seed-storage.md) only when that separate
hardware test is authorized.

Repeat failed tasks after glare, off-axis viewing, and a different phone camera.
Do not call the normal beta7 UI accepted until tasks 1 to 11 pass without
coaching.
