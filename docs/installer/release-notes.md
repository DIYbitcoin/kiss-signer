# KISS Signer 0.1.0-beta7

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta7.bin`: merged firmware image
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `kiss-signer-0.1.0-beta7-offline.zip`: the install page, the firmware and the signed hashes in one file
- `kiss-signer-0.1.0-beta7-offline.zip.asc`: GPG signature for the zip

## Verify

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify kiss-signer-0.1.0-beta7-offline.zip.asc kiss-signer-0.1.0-beta7-offline.zip
```

Main firmware SHA256:

`4573cf973ef5eb78a6fca9e9c5cc1702df882cb1eb699c366d0a56ce2baae11f`

Release commit:

`v0.1.0-beta6-82-g58ed74c`

## Install

For beta releases, flash this exact verified `.bin` using the README install steps.

To flash from a browser instead, unzip `kiss-signer-0.1.0-beta7-offline.zip`, run the serve file inside it (`serve.command` on macOS, `serve.bat` on Windows, `./serve.sh` on Linux) and open the address it prints. It serves to that one computer only and reaches nothing else, so the machine you flash from can be offline the whole time. `00-START-HERE.txt` inside the zip walks through it.

Both browser routes, the hosted page and this zip, need Chrome, Brave or Edge on desktop. Safari and Firefox cannot flash ESP32 devices over Web Serial. The hosted page also stays off whenever a release is staged; the zip does not, because it is the release.

After flashing, unplug the device, wait about 3 seconds, then plug it back in.

## Changelog

Look and feel. Nothing here changes how a key is derived or a transaction is
signed; it changes what the device looks like while you use it.

### Added

- **RECEIVE opens on a list of your addresses**, one per line, twenty to a page
  and a hundred in total. The four characters after the `bc1q` and the last four
  are lit, and those are the ones worth comparing, since every address starts the
  same way. Tap any one for its QR, its derivation path and VERIFY. That
  single address view is still there and keeps its arrows, so you never have to
  scroll to reach the next address.
- **A reminder to use a fresh address per payment**, on the screen showing the
  address you are about to hand over. KISS has no view of the chain and cannot
  know which addresses were paid to, so it gives the advice rather than marking
  individual addresses as spent.
- **Buttons answer a press.** This device has no vibration, so every pill now
  sinks slightly while held and releases a ring from its edge. It is the only
  kind of "I felt that" a screen can give you.
- **Icons on the buttons that do one specific thing**: a key on PAIR
  COORDINATOR, an incognito face on SCAN KEY, a QR on SCAN QR, a card on FROM SD
  CARD. Settings deliberately has none, because those are standard Bitcoin terms and
  read better as words.
- **The scanner shows it is still looking.** The viewfinder is a reticle now,
  double corners and edge ticks, that breathes and sweeps while searching, then
  stops dead and closes in green around the code it found. A dense QR can take
  several seconds, and a frozen screen during that looked like a crashed device.
- **The RBF card says which answer it is** before you read it: a replace arrow
  when the fee can still be raised, a padlock when it cannot.
- **Settings BACK is easier to hit.** Same button, bigger touch target.

### Changed

- **"silent payment" under SCAN KEY is readable.** It was rendering in the
  smallest type on the device, and it is the only thing on that screen saying
  which kind of address the key belongs to.
- **Opening RECEIVE no longer counts as showing an address.** Only opening a
  specific one does. Before, merely visiting the screen marked the address
  used.
- **Wording follows one rule now**: bitcoin arriving is a *payment*, bitcoin you
  build and sign is a *transaction*. Three strings disagreed.
- **The home screen says "encryption", not "enc"**, and the C6 radio readback
  moved to Settings, where the people who want it will look.

### Fixed

- **Address text no longer swallowed taps.** Anywhere an address was printed
  inside something tappable, the middle of it, the obvious place to press, did
  nothing.
