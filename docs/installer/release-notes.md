# KISS Signer 0.1.0-beta8

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta8.bin`: merged firmware image, for flashing over USB
- `kiss-signer-0.1.0-beta8-update.bin`: the same firmware as an SD card update (see FIRMWARE below)
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `kiss-signer-0.1.0-beta8-offline.zip`: the install page, the firmware and the signed hashes in one file
- `kiss-signer-0.1.0-beta8-offline.zip.asc`: GPG signature for the zip

## Verify

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify kiss-signer-0.1.0-beta8-offline.zip.asc kiss-signer-0.1.0-beta8-offline.zip
```

Main firmware SHA256:

`d96e57b4f83dbf7187ca82933c26b1c2d37096a17adb568ccab7020cdcbced9d`

Release commit:

`v0.1.0-beta7-973-g65e1375a`

## Install

For beta releases, flash this exact verified `.bin` using the README install steps.

To flash from a browser instead, unzip `kiss-signer-0.1.0-beta8-offline.zip`, run the serve file inside it (`serve.command` on macOS, `serve.bat` on Windows, `./serve.sh` on Linux) and open the address it prints. It serves to that one computer only and reaches nothing else, so the machine you flash from can be offline the whole time. `00-START-HERE.txt` inside the zip walks through it.

Both browser routes, the hosted page and this zip, need Chrome, Brave or Edge on desktop. Safari and Firefox cannot flash ESP32 devices over Web Serial. The hosted page also stays off whenever a release is staged; the zip does not, because it is the release.

After flashing, unplug the device, wait about 3 seconds, then plug it back in.

### Coming from beta7 or earlier: this one erases your keys

This release splits the flash into two firmware slots, which is what lets every
release after it arrive on an SD card instead of a USB cable. A partition table
cannot be replaced by the thing it defines, so the crossing itself has to be
done over USB, once.

**Your recovery words do not survive this flash.** `nvs`, where they live, moves
from `0x9000` to `0x11000` in the new layout, so the crossing takes them with
it. The browser and offline routes erase the whole chip; flashing the pieces by
hand leaves the old words at an address this firmware no longer reads. Either
way the keys are gone from this signer until you restore them.

So treat it as a restore, not an update. Have the recovery words in your hand on
paper, check them against the device before you unplug it, and expect to restore
from that paper once the new firmware is running. If those keys control coins and
you cannot find the words, do not flash.

After this, Settings has a FIRMWARE button. Put `kiss-signer-0.1.0-beta8-update.bin` in the root of an SD
card, hold to install, and the device checks the signature against the key built
into it before anything is written. If the new firmware fails to start, the
device goes back to this one on its own.

Use that file and not the merged image: the merged one starts with the
bootloader, and the device looks for the application header instead, so it
reports nothing to install.

## Changelog

Every screen was redrawn, the device speaks 21 languages again, and the fee it
shows is now one it can prove.

| | |
| --- | --- |
| 🎨 **The whole interface** | one system instead of a page-by-page layout: no boxes, one confirm gesture, a **?** on everything |
| 💸 **The fee is proven** | coin amounts are read from the transactions that created them, not taken on trust |
| 🌍 **21 languages** | all 20 translations rewritten against the shipping English, then cut to fit |
| 🔐 **Two signatures on updates** | ECDSA plus post quantum, both checked on the device |

### 🔒 Security

- **The fee on screen can no longer be understated.** A signature over a SegWit
  coin only covers *that* coin's amount, so a coordinator could lie about the
  others and send the difference to a miner. Amounts are now proven from the
  previous transactions whenever they are attached, and said plainly when they
  are not. Found and fixed first by odudex in Krux 26.08.0; the reading of the
  attack and the wording are theirs.
- **Seed words no longer survive in freed memory** after a QR is decoded, and
  neither does the silent payment scan key after its screen closes.
- **Taproot signatures are plain BIP340**, so any conforming signer reproduces
  them. The old house nonce rule meant the only thing that could check this
  signer was another one of the same build.
- **A firmware update needs two signatures**, ECDSA and SLH-DSA-SHA2-128s, both
  verified before anything becomes bootable. It does not let you spend with a
  post quantum key; nothing can yet.
- **Guessable passphrases, empty seed words and unrolled dice are refused**,
  not warned about. USE ANYWAY is gone from all of them.

### ✨ New

- **TERMS** — ten words this device has to explain, written once and shown
  wherever a screen owes you one. Settings > DEVICE > TERMS, with a count of
  what you have not read.
- **Coin flips**, beside the dice. 128 flips, judged the same way, and the
  string you typed is the preimage: hash it on any computer.
- **Blind draw** — cut up a BIP39 word list, draw 11 or 23 blind, and the
  device computes only the checksum word. No machine randomness in your keys.
- **How your keys were made** — Settings > AUDIT names the path that produced
  the seed this signer holds, and keeps naming it.
- **RECEIVE can be told what your coordinator sees.** USED answers from what
  this signer witnessed; UNUSED waits for a coordinator rather than guessing.
- **The offline installer** — one ~6 MB zip with the page, the firmware and the
  signed hashes, so you can flash with the network unplugged.
- **FIRMWARE says which version replaces which**, and takes updates from the SD
  card with no cable.
- **A caution when change lands past your coordinator's window**, where the
  coins come home to an address nobody is watching.

### 🎨 Changed

- **One confirm gesture everywhere.** Press, drag, lift. It replaces every
  900-2000 ms hold; a brush against the bar completes nothing.
- **Erasing takes two strokes, in opposite directions.** It is the one action
  with no undo, and a thumb in a pocket satisfied the old hold just as well.
- **Seed words are shown as a grid you read aloud**, twelve to a sheet, behind
  an amber gate, forward only.
- **The scan key has one door and it is KEYS.** A private key export with two
  entry points is two consent flows to keep in step.
- **The words match the rest of bitcoin.** This is a signing device, what it
  holds is keys, and a wallet is what your coordinator watches.
- **The BIP39 test vector is accepted**, and nothing else empty. It is the only
  seed the test transactions here are built against. Never put coins on it.

### 🐛 Fixed

- **The sign screen could crash while you read it** — opening the glossary
  closed the page underneath before the slide bar had finished with it.
- **A time locked payment said nothing about being time locked** in Dutch and
  Russian: the badge was dropped whenever the title line ran out of room. The
  network chip yields instead.
- **Five blank boxes** where Russian should have said how the keys were made.
  That card asked for the face this device keeps for things you compare
  character by character, and it carries no Cyrillic, accents or CJK.
- **The BACKUP VERIFIED screen was in English** in nineteen languages.
- **A Vietnamese heading was the same words as the button beside it.**
- **German named software that does not exist** — BlueKoordinator, where the
  product is BlueWallet.
- **A refused erase left half the gesture spent**, so one more stroke finished
  it.
- **RECEIVE had two address pickers** that disagreed about which one you meant.

### 🌍 Languages

All 20 translations were rewritten against the English that ships, then cut
again where a string was wider than the space it had: about 400 places where
the device clipped text at the edge or replaced its second half with dots. The
confirm bars were the worst of it, since SLIDE TO INSTALL is three words in
English and five in Polish.

### ❌ Removed

- **Restoring from a SeedQR.** The square holds the seed with nothing over it,
  so anyone who photographs it has your keys. Type the words, or open an
  encrypted backup, which is the same paper with a password over it.
- **The camera audit.** It made a real, spendable set of words out of a
  completely unjudged photo. The RANDOMNESS AUDIT is untouched.
