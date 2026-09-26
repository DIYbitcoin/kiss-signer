# Changelog

All notable, user-facing changes to KISS Signer. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer, so entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

## [0.1.0-beta11], 2026-09-26

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

## [0.1.0-beta10], 2026-09-09

The device finishes its sentences, and stops stranding you. Beta9 measured the
text and found dozens of lines that no longer fit their box in 21 languages;
this release rewrites them instead of trimming them, so nothing an owner reads
ends in a shrug. Behind those lines it closes the dead ends: a signature that
could not be saved sent you back to the home screen, an empty card slot named a
camera it would not open, and a signed receipt showed an address too short to
read back.

| | |
| --- | --- |
| 🔒 **Security** | 3 changes |
| 💫 **Improvements** | 31 changes |
| 🧰 **Under the hood** | 82 changes |

### 🔒 Security

- Gate the release on the RNG's provenance
- Read the lock state once, calm only when locked
- Gate the PQ vectors, and check the card by its source

### 💫 Improvements

- Cut six translated strings to their lanes
- Cut three more translated strings to their lanes
- Cut the tap note and the signature promise to their lanes
- Cut the passphrase intro and the lock screen body
- Cut the pairing note's second line to fit its box
- Cut three seed explainer bodies to their lanes
- Cut five more strings, two of them row labels
- Cut four more strings, including the seed words possessive
- Finish the sd note and the seed words possessive
- Cut five more keys, and fix three coordinator articles
- Cut eight more keys to their lanes
- Cut the last fourteen keys to their lanes
- Catch a translation that says more than its English
- Say transaction where the screens said payment
- Lift a band's controls above its own fill
- Sweep payment out of the Latin locales
- Say which way the erase gate's second leg goes
- Finish the payment sweep in the last four locales
- Rewind an abandoned slide instead of teleporting
- Credit the backup a restore just proved
- Write seed words in full on the restored chip
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

- Write the UNSIGNED marker from inside the container
- Write the UNSIGNED marker through one helper
- Call the four push workflows from one CI run
- Draft the changelog and refuse a missing entry
- Prove the release build reproduces on arm64
- Name the commit, not the tag before it
- Lower sixteen locale ceilings to the measurement
- Say which verdicts held, not which flag was unset
- Re-render the docs pictures at the current tip
- Ask the frame whether a tapped control is drawn
- Read the signature block back against the recipe
- Read the fuses before the step that burns them
- Sign the release lane with a three key RSA root
- Make the burn recipe read fuses before erasing
- Regenerate the images before signing them
- Put the bootloader back in the flash recipe
- Say what the burn recipe does in the docs
- Gate the encrypted lane on RNG provenance
- Cut the beta10 entry and bump the version
- Re-render the home screens at beta10
- Define the two names the badge bake needs
- Bake the version badge at beta10
- Release the card from gpg before signing
- Check the release scripts before release day
- Publish the beta10 install page
- Re-point the decisions index at its lines
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
- Write the beta11 changelog section
- Measure the contrast no other gate can see
- Publish beta11 to the install page
- Re-sign beta11 with a terminal attached


Since v0.1.0-beta9.

## [0.1.0-beta9], 2026-09-08

Everything on screen got bigger. The last release fit 21 languages onto the
glass; this one makes sure you can read them standing up.

| | |
| --- | --- |
| 🔎 **No more tiny text** | every sentence an owner reads is measured, and the ones that had quietly shrunk were rewritten instead |
| 👁 **The passphrase eye** | a mark instead of a word, because nine languages could not fit HIDE |
| 🗂 **Settings rows moved** | each row now sits on the tab that describes what it does |
| 🧰 **New toolchain** | built on ESP-IDF v6.1 |

### 🔒 Security

- **A host with no randomness now stops instead of inventing some.** On desktop
  and simulator builds, a failed read from the system random source was topped
  up with a counter, which looks like randomness to every test and is identical
  on every run. It aborts now. The device itself never used that path.

### ✨ Better

- **Text is sized by measurement, not by guess.** Bodies, explainer grids and
  fact rows all pick the largest size that genuinely fits, and four separate
  places that had been throwing away a third of their space were corrected.
  Where the copy was simply too long, the copy was cut.
- **A fact's value wraps.** It used to be pinned to one line sized for English,
  which is why the same row overflowed in fifteen other languages.
- **The passphrase toggle is an eye.** Nine languages had no short word for
  HIDE, so the word is gone.
- **Word suggestions are big enough for a thumb.**
- **Settings reads in the order you look for things.** The theme picker sits
  above the **?**, PERSIST moved to the tab that says how the signer behaves,
  and the DEVICE tab holds firmware, build and terms.
- **Cautions keep one colour**, and NO UNDO no longer shouts over the control
  it belongs to.

### 🧰 For builders

- **The encrypted release recipe now carries secure boot v2.** Both eFuse burns
  have to happen in one first boot or the second can never happen at all. It
  refuses to produce a flashable image until the signing key is settled: secure
  boot on this chip is RSA-3072, because ECDSA secure boot is errata'd on the
  ESP32-P4.
- **A gate now measures every screen against the 3.5in board** nobody is
  holding yet, so the work that board needs is a list rather than a surprise.
- **Eighteen checkers prove they still fire.** Six had no self test at all, and
  one of those was the check that had just caught a real failure.

Earlier releases are in [CHANGELOG-ARCHIVE.md](docs/CHANGELOG-ARCHIVE.md).
