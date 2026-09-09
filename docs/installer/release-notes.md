# KISS Signer 0.1.0-beta11

Beta firmware for the Guition JC4880P443C ESP32-P4 device.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta11.bin`: merged firmware image, for flashing over USB
- `kiss-signer-0.1.0-beta11-update.bin`: the same firmware as an SD card update (see FIRMWARE below)
- `SHA256SUMS`: firmware hashes
- `SHA256SUMS.asc`: GPG signature for `SHA256SUMS`
- `kiss_signer_pgp.asc`: KISS release public key
- `release.json`: release metadata for tools

Flashing on a machine with no network? Take these two instead:

- `kiss-signer-0.1.0-beta11-offline.zip`: the install page, the firmware and the signed hashes in one file
- `kiss-signer-0.1.0-beta11-offline.zip.asc`: GPG signature for the zip

## Verify

```sh
gpg --import kiss_signer_pgp.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
# expect fingerprint: 166ACBF37786FCEAA69496DE886F1BFEB84EF1C0

shasum -a 256 --ignore-missing -c SHA256SUMS
# Linux: sha256sum --ignore-missing -c SHA256SUMS

# the offline installer carries its own signature
gpg --verify kiss-signer-0.1.0-beta11-offline.zip.asc kiss-signer-0.1.0-beta11-offline.zip
```

Main firmware SHA256:

`7f38de2c84ebf287693fa8de0a987cbeaf46a88aa69ee5a3715415f80b6285c4`

Release commit:

`b2a0c90a`

## Install

**Four ways in, easiest first.** Each is written out with pictures on the
install page and in the guide — this is the map, not the manual.

| | Route | Cable? | Who it suits |
| --- | --- | --- | --- |
| 🖱️ | **[Install page](https://diybitcoin.github.io/kiss-signer/)** | yes, USB | anyone. Plug in, tick the box, press the button |
| 💾 | **[SD card update](https://diybitcoin.github.io/kiss-signer/guide.html#sdupdate)** | no | already on beta8 or later, and no computer to hand |
| 📦 | **[Offline zip](https://diybitcoin.github.io/kiss-signer/guide.html#offline)** | yes, USB | the machine you flash from has no internet |
| ⌨️ | **[Command line](https://diybitcoin.github.io/kiss-signer/guide.html#esptool)** | yes, USB | Safari or Firefox, or you would rather use a terminal |

The SD card route is the only one that needs no computer at all, and it keeps
your keys and settings: put `kiss-signer-0.1.0-beta11-update.bin` in the root of a card, then SETTINGS →
FIRMWARE on the device and hold to install. Use that file, not the merged image.

> ⚠️ **Coming from beta7 or earlier?** SD card updates did not exist yet, so you
> have to use the install page, and **that erases the whole chip — including
> your keys.** Have your seed words and passphrase on paper first. Anyone on
> beta8 or later can ignore this.

## Changelog

The device stops stranding you. A signature that could not be saved sent you
back to the home screen, an empty card slot named a camera it would not open,
and a signed receipt showed an address too short to read back; each of those
now leads where it says it does.

| | |
| --- | --- |
| 🔒 **Security** | 1 change |
| 💫 **Improvements** | 10 changes |
| 🧰 **Under the hood** | 52 changes |

### 🔒 Security

- Gate the PQ vectors, and check the card by its source

### 💫 Improvements

- Cut the Spanish seed words row to its lane
- Say the card is not a backup at the choice
- Stop the signing screens naming one coordinator
- Send BACK on a failed signature to the list
- Open the camera from the empty-card lane
- Rename the key that no longer names Sparrow
- Record what the failure screen cannot recover
- Open the address scan from the pairing page
- Open the full address from the signed receipt
- Stop unselected tabs reading as disabled

### 🧰 Under the hood

- Cut the README to a front page
- Correct three releases and archive the old ones
- Name the sections the way every wallet does
- Say which way to install, on each system
- Rebuild the site theme on the firmware's tokens
- Cut the install page to a single screen
- Reorganise the docs into topics you navigate
- Put the site header on the simulator page
- Give the release check page the firmware look
- Match the offline install page to the hosted one
- Show the repo as a mark, not a tab
- Give the mark the size a logo deserves
- Let the lockup carry the header
- Hyphenate the wordmark to match the project name
- Give Safari and Firefox a way to flash
- Show the browser warning where Safari can see it
- Drop the simulator's back link
- Point every link at the DIYbitcoin org
- Explain the offline install instead of just linking it
- Stop the install page calling the release bad
- Read the fuse's protection, not just its value
- Offer the SD card route, and show the digest again
- Prove the three blocks carry three keys
- Make the rollback counter a release input
- Say signer and seed words, not wallet and words
- Point the README at the rendered docs
- Generate the social card, and gate it
- Split the encrypted release into two key lanes
- Stop pointing operators at unmatchable hashes
- Stamp the pictures at the Spanish row label
- Let a card hold the key that signs updates
- Check the root before spending a compile
- Build the update lane on every release push
- Cut the guide back to one fact, one home
- Watch the baked art and the scripts that bake it
- Catch published notes drifting from the changelog
- Name the two coordinators, promise no others
- Make the release notes a map, not a manual
- Say what a coordinator has to do, not who it is
- Re-render the screens four commits moved
- Rank the install routes, easiest first
- Re-anchor the decisions index after three commits
- Rebuild the published simulator from this tree
- Record the card rehearsal, and correct one claim
- Re-stamp the pictures the render left identical
- Cut the pairing note to what a reader has to do
- Break the long sentences a newcomer stalls on
- Re-anchor the decisions index after three commits
- Drop the frame that cost two locales their ceiling
- Re-anchor the walk's decision link
- Re-render the screens three commits moved
- Number today's work beta11

Since v0.1.0-beta10.
