# Changelog

All notable, user-facing changes to KISS Signer. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer, so entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

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
