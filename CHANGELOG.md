# Changelog

All notable, user-facing changes to KISS Signer. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer, so entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

## [0.1.0-beta11], 2026-09-09

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

## [0.1.0-beta10], 2026-09-09

The device finishes its sentences. Beta9 measured the text and found dozens of
lines that no longer fit their box in 21 languages; this release rewrites them
instead of trimming them, so nothing an owner reads ends in a shrug.

| | |
| --- | --- |
| 📝 **Sentences that fit** | around forty strings rewritten to their lane rather than cut off mid-word |
| 💸 **Transaction, not payment** | one word for the thing you sign, in every language |
| ✅ **A restore counts as a backup** | proving your words on the device credits the backup you just proved |
| 🧰 **A release that checks itself** | the build refuses to ship if its randomness or its signatures are not what the recipe says |

### 🔒 Security

- **A release build now proves where its randomness came from.** The check that
  catches a host with no entropy ran on the ordinary release lane only. Both
  release lanes run it now, and neither will produce a binary without it.
- **The encryption row tells a rehearsed board from a finished one.** Three
  corners of Settings each asked the chip their own question, and the one they
  asked is answered yes by a board that is still open over the cable. One
  reader answers for all three, and the calm state means what the recipe says.

### ✨ New features

- **A restore now counts as a backup.** Reading your seed words back into the
  device proves the paper is right, so the backup reminder stops asking for
  something you have already done.

### 💫 Improvements

- **Payment became transaction** on every screen and in every language. The two
  words were mixed, and only one of them is what a signer actually handles.
- **Around forty lines were rewritten to fit**, across all 21 languages: seed
  explainers, the pairing note, the passphrase intro, the lock screen, SD card
  notes, row labels and the coordinator articles. Each one now says the whole
  thing at the size the screen can show.
- **The screen that says what a restore recovered writes seed words out in
  full**, instead of naming them in shorthand.

### 🐛 Bug fixes

- **An abandoned slide rewinds** to where it started, rather than jumping there.
- **Controls sit above their own band's fill**, so a tap lands on the control
  and not on the paint behind it.
- **A translation that promised more than its English does** is caught by the
  gate now, rather than by a reader.

### 🧰 Under the hood

- The release recipe reads the chip's fuses before the step that burns them,
  reads each signature block back against the configuration that will judge it,
  and puts the bootloader back into the flash list where it belongs.
- The reproducibility proof now covers arm64 as well as x86.
- The changelog has a generator: it drafts the skeleton from the commits and
  refuses a release whose entry was never written.
- Four separate push workflows became one CI run.
- The burn recipe in the documentation says what the build actually does, and
  the pictures in the guide were re-rendered at the current build.

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

Earlier releases are in [CHANGELOG-ARCHIVE.md](CHANGELOG-ARCHIVE.md).
