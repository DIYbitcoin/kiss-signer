# KISS Signer 0.1.0-beta11

Beta firmware for ESP32-P4 boards, one image per board: the Guition JC4880P443C (Guition 4.3in), the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (Waveshare 3.5in) and the Guition JC1060P470C (Guition 7in). Take the files for your board: each image carries only its own board's display driver.

> **Beta warning:** do not trust this release with meaningful funds yet.

## Download

Download these assets from this release into one folder:

- `kiss-signer-0.1.0-beta11.bin` and `kiss-signer-0.1.0-beta11-update.bin`: Guition 4.3in, merged image for USB and the same firmware as an SD card update (see FIRMWARE below)
- `kiss-signer-0.1.0-beta11-ws35.bin` and `kiss-signer-0.1.0-beta11-ws35-update.bin`: Waveshare 3.5in, merged image for USB and the same firmware as an SD card update (see FIRMWARE below)
- `kiss-signer-0.1.0-beta11-jc1060.bin` and `kiss-signer-0.1.0-beta11-jc1060-update.bin`: Guition 7in, merged image for USB and the same firmware as an SD card update (see FIRMWARE below)
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

Main firmware SHA256, per board:

- Guition 4.3in: `0078184abe4021b1dcecbf80b8f2092d866a5ecb8494342e40a441c3f664abeb`
- Waveshare 3.5in: `7bf8ff09826a71c3ce8fa758aa6d9276d80c1c4df9fbf365300167500c02a2f4`
- Guition 7in: `e2c4cb72ab4faa5ff3591667abfffff028ea4f211f66330d45e81baa3855ffa5`

Release commit:

`9f63c65c`

## Install

**Four ways in, easiest first.** Each is written out with pictures on the
install page and in the guide — this is the map, not the manual.

| | Route | Cable? | Who it suits |
| --- | --- | --- | --- |
| 🖱️ | **[Install page](https://diybitcoin.github.io/kiss-signer/)** | yes, USB | anyone. Plug in, pick your board, tick the box, press the button |
| 💾 | **[SD card update](https://diybitcoin.github.io/kiss-signer/guide.html#sdupdate)** | no | already on beta8 or later, and no computer to hand |
| 📦 | **[Offline zip](https://diybitcoin.github.io/kiss-signer/guide.html#offline)** | yes, USB | the machine you flash from has no internet |
| ⌨️ | **[Command line](https://diybitcoin.github.io/kiss-signer/guide.html#esptool)** | yes, USB | Safari or Firefox, or you would rather use a terminal |

The SD card route is the only one that needs no computer at all, and it keeps
your keys and settings: put `kiss-signer-0.1.0-beta11-update.bin` or `kiss-signer-0.1.0-beta11-ws35-update.bin` or `kiss-signer-0.1.0-beta11-jc1060-update.bin` (your board's) in the root of a card, then SETTINGS →
FIRMWARE on the device and hold to install. Use that file, not the merged image.

> ⚠️ **Coming from beta7 or earlier?** SD card updates did not exist yet, so you
> have to use the install page, and **that erases the whole chip — including
> your keys.** Have your seed words and passphrase on paper first. Anyone on
> beta8 or later can ignore this.

## Changelog

Two more boards. The same signer now runs on the Waveshare 3.5in and the
Guition 7in as well as the Guition 4.3in, from one set of screens scaled to
each glass, and the install page asks which one you have. Around that, a
signature that could not get out is kept instead of lost, the sign screen says
which outputs are yours, a send to yourself shows how much of it the fee takes,
the device refuses an update built for another board, and Hungarian joins as
the twenty second language.

Updating a beta10 device from the card: take your own board's `-update.bin`.
Beta10 does not check which board a file is for; if the screen stays dark after
a wrong one, power it off and on and it goes back to beta10.

| | |
| --- | --- |
| 🔒 **Security** | 3 changes |
| ✨ **New features** | 12 changes |
| 🐛 **Bug fixes** | 13 changes |
| 💫 **Improvements** | 52 changes |
| 🧰 **Under the hood** | 119 changes |

### 🔒 Security

- Align ECDSA signing with the BIP461 draft
- Refuse an update built for the other board
- Show what each source gave the seed

### ✨ New features

- Lay out every screen for the 3.5in's canvas
- Add a setting that turns the screen upside down
- Keep a signature that could not get out
- Give PAIR COORDINATOR tabs and one card
- Say which outputs are mine on the sign screen
- Add Hungarian as the twenty-second locale
- Add the 3.5in board profile
- Show the camera preview on the 3.5in
- Add the Guition 7in board
- Run the camera on the 7in
- Show the battery on the 3.5in's home
- Show the fee share for a send to self

### 🐛 Bug fixes

- Drop the QR widgets when their screen goes
- Shut the explainer on BACK, not the page
- Land RECEIVE past what is used, not what was read
- Stop the pair note breaking onto a lone full stop
- Fit the Norwegian loop hint at reading size
- Put the pairing QR back to its full size
- Leave nothing on the card when a write rolls back
- Retry the channel that failed, not the source
- Read the card again from a firmware refusal
- Tell a failed check from an empty search
- Keep three controls on one band clear of each other
- Give the arrow action the gap it draws
- Start a swipe where the finger lands

### 💫 Improvements

- Walk both ways off a held signature
- Make the receipt address a target a finger can hit
- Frame the code the held screen is about
- Group addresses from the right
- Fold addresses on the card's own blocks
- Cut two captions that explain nothing
- Give the caution bar's ack a finger-sized target
- Cut the arrival motion down to a ribbon
- Call the eight characters a signature code
- Choose a body's lines so no stop can lead one
- Re-render the generated files over the wrap change
- Let a QR card leave its enlarge cue behind
- Cut the SIGNED screens back to what helps
- Measure the pairing claim off the control beside it
- Take the telephone off the pairing flow
- Render the frames the tap gate compares
- Settle the pairing screen's marks and spacing
- Bound the step copy to the row it reads
- Stop the BlueWallet note repeating the screen
- Say only what the watch only line can promise
- Take the watch only sentence off the pairing screen
- Say what watch only means on the BlueWallet tab
- Say each thing once on the pairing screens
- Give each scanner door its own words
- Stop typing the search depth into 21 translations
- Name the remedy when an image fails its signature
- Cut the remedy line to the half that acts
- Spend the accent only on the rows that are mine
- Move the Guition bring-up out of main.c
- Turn the 3.5in's panel and touch to landscape
- Set the 3.5in's type at three fifths
- Draw the 3.5in's art at its own size
- Hint the 3.5in's small type to whole pixels
- Send the 3.5in's panel its factory tuning
- Fit every screen to the 3.5in in all languages
- Turn the 3.5in's camera picture with the flip
- Show more of the 4.3in's camera in its preview
- Read the touch panel on its own clock
- Test panel capabilities, not board names
- Check that no board test falls through
- Draw the 7in's art at 1024x600
- Set the 7in's type one rung up
- Ask the port forecast from the 4.3in only
- Fit the 7in's larger type where it clipped
- Keep the 7in's flush out of a camera frame
- Size square QR cards by the smaller axis
- Scale the 7in's throws to its screen
- Keep the 7in's game on the wall clock
- Tell owners to write the passphrase down
- Say an encrypted backup holds no passphrase
- Say what an opened backup gave back
- Explain the fee share on a send to self

### 🧰 Under the hood

- Re-point the decisions index at moved lines
- Give the sign fixture an address that can fail
- Drop the retired slot from the text fit gate
- Let the address-mark gate see the bug it exists for
- Re-render the pictures over today's screens
- Rebuild the published simulator and the index
- Land the walk's arrival stop on the whole overlay
- Stamp the pictures at the wrap change
- Clear the repo root of what does not belong
- Make the ignore rules survive a fresh clone
- Judge a backlog entry across the whole sweep
- Make the attribution lane read what it scans
- Give issues and pull requests a form
- Record the four accepted risks in the plan
- Name the locales nobody has read
- Make every link in the docs resolve
- Stop an unused apt source failing the sim build
- Keep CI hash lists out of the working tree
- Stop the unsigned marker crying wolf
- Point the walk at the pairing page it has now
- Ask git, not the disk, where a link points
- Re-render the pictures over the new screens
- Write the review index a link that resolves
- Re-render the pictures over the mark change
- Re-render the pictures over the pairing marks
- Rebuild the web simulator over the pairing screen
- Cut the simulator page to what it is for
- Stamp the pictures at the build fix
- Give the pages the kiss mark as their tab icon
- Name the files that fail the clean-tree guard
- Put the simulator page on the theme colour
- Leave the QR panel one way in
- Set the side panel at a size people can read
- Take the notices paragraph off the simulator page
- Close the gap the notices paragraph left
- Stand the title beside its lead, not above it
- Say the entropy caveat in one line
- Fit the lead on the title's own line
- Say what the three panels do in Bitcoin words
- Stamp the pictures at the watch only line
- Call it a coordinator and call them seed words
- Stamp the pictures after the commit, not before
- Reach the simulator's language picker on a phone
- Collapse the docs and install page on a phone
- Shorten the storage table's column heads
- Stop the pairing note defining its own term
- Retire the allowance for "online wallet"
- Say why a browser tab is not a signer
- Trace the unlock gesture over the menu
- Show the animated QR instead of describing it
- Point at the four numbers worth checking
- Send readers to the simulator from four topics
- Mark the topics a reader has already opened
- Say in the footer that the site contacts nobody
- Check the links and anchors in the HTML too
- Assert the HTML side answers git, not the disk
- Draw where the fingerprint and the address come from
- Give the guide the width a wide screen has
- Stop reserving a column for a hidden element
- Put the search in the header, centred
- Lower thirteen locale ceilings to the measurement
- Give the docs one column and a reading face
- Give the release check page the type floor
- Let a digest wrap on the release check page
- Drop the slogan from the install footer
- Say what the signer is and what it does
- Keep the footer to what the header lacks
- Restamp the docs pictures after the copy change
- Refuse a push to main with stale doc pictures
- Drop the install page footer
- Cut the simulator lead to one line
- Set the guide in the firmware face again
- Fit the install page on one screen
- Show the on-this-page column on more topics
- Say the browser problem once, not twice
- Drop the slot no reader can reach
- Name the two halves in the hero caption
- Publish the website from develop
- Mark the last heading at the foot of a topic
- Name two topics in bitcoin terms
- Colour the round trip step numbers with the theme
- Put the real term in five headings
- Say each thing once in three topics
- Print the whole guide, not one topic
- Show the randomness audit and the storage ask
- Say the browser problem once, as the fix
- Match the offline page to the install page
- Answer to /docs and /install
- Put the chat link in the simulator header
- Render the pictures at the screens they show
- Check a real bech32 checksum in the simulator
- Stamp the pictures at the screens they show
- Gate Hungarian in the two locale lanes
- Count twenty-two languages across the docs
- Rebuild the browser simulator with Hungarian
- Add a build-time board profile
- Drop the unused esp_lvgl_port dependency
- Add the ST7796 and FT5x06 drivers
- Build both boards in the firmware lane
- Say which board the tree is being built for
- Build the simulator and gates for either board
- Remove the plans and notes nothing reads
- Release firmware for the Waveshare 3.5in too
- Rebuild the browser simulator
- Refuse a board the build files do not list
- Fail the overlap gate without its ceilings
- Run preflight's board lanes from one list
- Keep the build off the registry's newest versions
- Let the wall threshold grow with the type
- Regenerate the decisions index
- Rebuild the browser simulator
- Regenerate the decisions index
- Rebuild the browser simulator
- Regenerate the decisions index
- Record where silent payments were proven
- Update security-plan.md
- Regenerate the decisions index
- Warn on commit messages over 50/72 characters
- Name every board in the board header

Since v0.1.0-beta10.
