# Changelog

All notable, user-facing changes to KISS Signer. Dates are ISO (YYYY-MM-DD).
This is a Bitcoin signer, so entries are written so a non-developer can tell what
changed and why it matters. Versions follow the firmware tags.

## [0.1.0-beta8], 2026-08-31

Every screen on this device was redrawn. Beta7 was a set of pages that each
solved their own layout; this is one system. Words are set in one typeface and
the things you check character by character in another, a fact is a line that
opens its own explanation where it stands, an action is an arrow on the band at
the bottom, and nothing is drawn inside a box any more. Every page with
something to teach carries a "?" in the same corner, and the ten terms this
device has to explain are written once and read from one table.

The other half of the release is the fee on the screen. A signature over a
SegWit coin only covers *that* coin's amount, so a coordinator can understate
what the other coins are worth, the device subtracts and shows a fee lower than
the one that will actually be paid, and the difference goes to a miner. The
signer now proves those amounts whenever the transaction lets it, and says so
plainly when it cannot. Found and fixed first by odudex in Krux (release
26.08.0); the reading of the attack and the wording of the warning are theirs.

### Removed

- **Restoring from a SeedQR is gone.** Both restore screens used to offer it:
  point the camera at a printed square and the words were in. The square holds
  the seed itself with nothing over it, so anyone who photographs it — across a
  room, over a shoulder, out of a cloud backup of your camera roll — has the
  wallet. This signer never wrote one, so the feature only ever read somebody
  else's, and the sensible thing to do with the paper it came from is to keep
  it away from cameras rather than to teach a device to eat it.

  What replaces it is what was already there: type the words, or open an
  **encrypted backup**. The backup is the same QR shape and the same paper, with
  a password over it, and this signer does write those. The scan door on both
  screens now takes an encrypted backup and nothing else, so a QR alone can no
  longer put keys on this device — a password always stands in between.

  If you are holding a SeedQR and nothing else, type the twelve or twenty four
  words in. They are the same words.

### Fixed

- **The amount of every coin is now read from the transaction that created it**,
  whenever your coordinator sends that transaction along. That previous
  transaction has to hash to the exact coin being spent, so its amount cannot be
  anything other than the truth. Before, a native or nested SegWit coin was taken
  at the coordinator's word even when the proof was sitting in the same file.
- **A coordinator that contradicts itself about a coin is refused**, rather than
  the signer picking whichever number it read first.
- **Recovery words no longer sit in freed memory after a QR is read.** The QR
  decoder's scratch space holds whatever it just decoded, and what it just
  decoded can be a seed. That block went back to the allocator without being
  wiped, so the words survived in freed memory for as long as nothing else
  claimed it. It is zeroed now, before the block is handed back. Found in an
  audit of this repository.
- **The silent payment scan key no longer sits in freed memory after its screen
  closes.** The same shape one page over, with a private key in it: the export
  screen handed the key to a label, and the label's own copy was freed unwiped
  when the screen went away. Every copy is zeroed now, on both ways out,
  including the idle lock.
- **A refused erase no longer leaves half the gesture spent.** Erasing takes two
  strokes. When the erase itself failed, the message saying so was drawn over
  the confirm screen, and dismissing it uncovered a slide still holding the
  first stroke, so one more finished the job. A refused attempt costs both
  strokes again. It was found by a sweep in Polish; in English the walk's second
  stroke happened to land on empty glass.
- **RECEIVE had two address pickers and they disagreed about which address you
  meant.** A short list under the caption and the full ALL ADDRESSES page kept
  separate positions, so paging one and coming back to the other showed the
  wrong index. There is one picker now, and the page it opens on is computed
  from the address you are looking at.

### Changed

- **KEYS, RECEIVE, SETTINGS, SIGN, the words screen and FIRMWARE were rebuilt on
  one system.** KEYS opens with the fingerprint across the top in two blocks of
  four, which is how a person reads eight characters aloud, and names who to
  check it against. RECEIVE is three tabs and the address unfolds where it
  stands. SIGN's DETAILS became one subject per tab. The teaching lines in
  settings are sentences at a size you can read rather than fine print, which
  was the single most repeated complaint about the old screens. Nothing about
  how a key is derived or a transaction is signed changed with any of it.
- **A passphrase the device thinks is guessable is refused, not warned about.**
  Creating one used to put USE ANYWAY under a card explaining that a short
  passphrase is easier to guess, selected, at the moment in setup you are least
  inclined to read carefully. Now the card sends you back to lengthen what you
  typed. This applies only where a passphrase is being MADE. Logging in never
  judges what you type and never could: every passphrase is valid and opens some
  wallet, so a signer that turned one down would be turning down a wallet.
  Choosing to have no passphrase at all is still one tap, and is still a real
  choice with its own screens. The password on an encrypted backup is refused on
  the same terms, because whoever finds that file can guess at it offline
  forever.

- **Recovery words that carry no secret are refused when you bring them in.**
  Typing "abandon" eleven times and "about" is a valid BIP39 phrase with a
  correct checksum, printed on essentially every page that explains what a seed
  is, and worth nothing: anyone can type it and spend from it. The signer used
  to accept it by hand and out of an encrypted backup. Now both turn it down,
  along with a short run of words repeated to fill the phrase. Only that class is refused. A real wallet whose words happen to
  cluster or repeat still restores, because those are judgements about a draw
  and an existing wallet cannot be redrawn.

- **A restore that does not work out now lands in one place**, whether a word is
  wrong, the phrase does not add up, or the words carry nothing. There were
  three screens with the same name; there is one.

- **The signer folds one more source into every seed it makes.** Alongside the
  camera, the chip's noise and your taps, it now measures the tiny delays in its
  own workings, which run on a different circuit from the chip's noise generator
  and so survive that generator failing. The "?" on the randomness screen names
  all four and still points a doubter at DICE, which is the only source here you
  can check away from the device. The Settings footer names both physical
  sources rather than one.

- **A firmware update that does not start is no longer the end of the device.**
  The bootloader now keeps the version you were running and returns to it if the
  new one never comes up. Nothing stops you installing an older build on
  purpose: this is a way back, not a lock.

- **Words you drew yourself are now checked before they become a wallet.**
  BLIND DRAW lets you cut up your own copy of the BIP39 word list, draw from it
  blind and type what you get. Until now the device accepted whatever you typed.
  Type one word eleven times, or one short run over and over, and it now
  refuses: those carry no secret at all, and anybody could type them. Draws that
  are merely weaker than you think, words sitting side by side on the list,
  words in list order, or more repeats than 2048 words would give, are also
  turned down. Whichever screen you see, it shows your own draw as a row of
  bars, so the shape being questioned is the one on screen, and START OVER
  reopens the keyboard.

  Every one of them refuses now. Three of the five used to offer a USE ANYWAY
  instead, and the argument for removing it is the one the dice settled first: a
  draw you are making right now can be made again for the price of a redraw, so
  there was nothing on the far side of that warning worth keeping.

  Sorting the pieces before typing them is worth calling out, because it looks
  harmless: the order you picked them in was part of the secret, and putting
  them in order throws about 25 bits away.

  A "?" on the intro screen now explains where the words come from: the list is
  public and the same 2048 words in every wallet, the secret is which ones a
  blind pick lands on, and the last word is arithmetic rather than a choice.

  What this cannot see is a set that merely looks random: a memorised phrase, a
  line of a song, or words you picked while feeling unpredictable. Only a blind
  pick is random.

- **A dice roll the device does not believe was rolled is now refused, not
  nudged.** The check on your rolls has always been there and has always been
  right; it just used to offer a USE ANYWAY next to it, one tap away at the
  moment in setup you are least inclined to read carefully. Now a run that fails
  is turned down. Nothing about the check itself changed, and nothing was made
  stricter: the bar sits exactly where it did, and roughly one honest session in
  a million ever reaches it. ROLL MORE is the way through and it keeps every
  roll you have already entered, so you never start over unless you want to.

- **The key that locks your seed to an SD card no longer comes from one place.**
  It used to be 32 bytes straight from the chip's random number generator. Your
  seed was never in that position, because three sources are folded into it and
  a hash is as strong as its best input, but the card key had no second source
  to fall back on, and this chip's noise generator is one the firmware has to
  switch on itself. So the card key and the random value in front of every
  sealed card are now mixed with a measurement of the device's own timing,
  which does not run through that generator and so cannot fail with it. Cards
  written before this still open: a device keeps the key it already minted.
- **One confirm gesture, everywhere.** Every press and hold became a slide:
  press the bar, drag right, the fill follows your finger, and the action fires
  when you lift at the far end. It replaces timers of 900 to 2000 milliseconds
  on showing the words, showing the scan key, moving storage, erasing,
  installing firmware, changing the duress word, writing an encrypted backup,
  deleting a file, and SLIDE TO SIGN. What counts is how far the finger
  travelled rather than where it landed, so a brush against the end of the bar
  completes nothing.
- **Erasing takes two strokes, in opposite directions.** It is the one action on
  this device with no undo, and it was guarded by a two second hold, which a
  thumb resting in a pocket satisfies exactly as well as a decision does. The
  first stroke runs left to right and parks, the word becomes ONCE MORE, the
  second comes back the other way, and the erase happens when you lift.
- **The recovery words are shown as a grid you can read aloud.** Twelve to a
  sheet in three columns of four, the number small and the word carrying, which
  is the shape of the thing you are copying onto paper one line at a time.
  Getting there is an amber gate like every other caution on the device, the
  sheets go forward only, and DONE appears on the last one, so leaving happens
  after you have seen every word. The band says who can spend these coins for as
  long as they are up.
- **The scan key export has one door, and it is KEYS.** It used to be reachable
  from RECEIVE as well, and an export of a private key with two entry points is
  two consent flows to keep in step. RECEIVE still tells you the export exists
  and where it lives. Sharing it now stands behind the same amber gate as the
  words.
- **Taproot signatures are plain BIP340, so somebody else can reproduce them.**
  Firmware that chooses its nonces badly can leak the seed through the
  signatures it produces, and the defence, a nonce fixed by the key and the
  message, is only worth something if you can check it: sign the same
  transaction on a signer you trust independently and compare the bytes. This
  signer used a nonce rule of its own, so the only thing that could check it was
  another KISS, on the one path where an outside check is worth the most. It now
  uses the standard rule with the standard empty auxiliary input, so any BIP340
  signer holding the same key produces the same signature. Silent payment
  signatures come out with different bytes than before; no key, no address and
  no transaction is affected.
- **The BIP39 test vector is accepted, and nothing else that carries no secret.**
  "abandon" eleven times and "about" is printed on every BIP39 page there is, so
  keys made from it are keys the whole internet can spend. It was refused along
  with every other empty seed, and that left this device unable to hold the one
  seed every test transaction in this repository is built against, on the one
  flow no simulator can check. The exception is exactly one phrase wide: it is
  compared as a string, and 32 zero bytes, the 0x80 vector and a phrase of one
  repeated word are refused exactly as before. Never put coins on it.
- **The words on screen were made to match the rest of bitcoin.** This is a
  signing device, what it holds is keys, and a wallet is the thing your
  coordinator watches. The screens say that consistently now, a coordinator is
  called a coordinator wherever one is mentioned, and the backup is recovery
  words rather than a phrase.
- **The game behind the cover is a game again.** It dealt the same round on every
  power cycle, the best score died with the power, and the blade cut where the
  finger was rather than where the stroke went. All three are fixed, and there
  are waves, a gold fruit worth three seconds of frenzy, and a combo banner.
- **The DETAILS line about sighash ALL no longer overclaims.** It said signatures
  cover every amount above. They cover every destination and its amount; the
  amounts going *in* are the thing this release is about.

### Added

- **A firmware update now needs two signatures, and one of them is post
  quantum.** The key that decides whether firmware installs on this signer was
  an elliptic curve key, and elliptic curve keys are what a quantum computer
  breaks. Whoever could forge that one could hand every KISS signer an image it
  would install and trust. Update images now carry a second signature over the
  same bytes — SLH-DSA-SHA2-128s, the hash based scheme NIST standardised as
  FIPS 205 — and the device checks both before anything becomes bootable. It is
  a second lock on the same door, never a replacement for the first.

  Checking it costs a few milliseconds, because the P4's SHA accelerator is held
  for the whole operation rather than picked up and put down for each of the two
  thousand hashes involved.

  **This does not let you spend bitcoin with a post quantum key, and nothing
  can yet.** No consensus rule accepts a hash based signature. What it protects
  is the firmware that holds your keys.

  One consequence to know about: a release published before this existed carries
  no post quantum signature, so this firmware refuses to install one. The screen
  says which of the two signatures was missing rather than telling you the
  signature failed, because that release is genuine and a corrupt download is
  the wrong thing to go looking for.

- **The signer now tells you how your keys were made, and keeps telling you.**
  Settings > AUDIT > HOW YOUR KEYS WERE MADE names the path that produced the
  seed this device is holding: the camera and taps, dice, your own blind draw,
  or words brought in from somewhere else. It is the one fact about a wallet you
  cannot recover by looking at the words, and until now the device knew it for
  the length of one screen and then forgot it. Where the signer did the mixing
  itself, the screen draws it: the sources that went in, folding into your keys.
  A wallet made before this arrived says so rather than guessing. Erasing the
  wallet erases this with it.

- **THE INSTALLER NOW FITS IN ONE FILE, so you can flash with the network
  unplugged.** Every release carries `kiss-signer-VERSION-offline.zip`, about
  4 MB: the install page, the firmware and the signed hashes together. Download
  it once on a machine that has a network, check the signature, carry it to the
  machine that does not, unzip, and run the serve file inside. It shows the page
  to that one computer and reaches nothing else, so nothing about the flash
  depends on the network behaving on the day you do it. Prompted by Krux
  Installer, which stopped downloading anything at flash time in its v0.0.23 by
  embedding the firmware at build time; the argument that fewer moving parts
  between you and your device is worth the packaging work is theirs.

- **BLIND DRAW: a seed you drew by hand, with the device only doing the math.**
  A third way to create a wallet, beside the camera and the dice: make a
  physical copy of the BIP39 word list — cut it up, or 3D print it as tiles —
  mix the pieces, draw 11 (or 23) blind, and type them in. The last word of a
  seed phrase is part checksum, so it cannot be drawn — the device computes
  every word that completes your draw (128 for a
  12 word seed, 8 for a 24 word one), shows them all, and you pick one. No
  machine randomness enters the seed, and the result checks out on any BIP39
  tool. A full page explains the checksum on the way: same words, right or
  wrong last word, tick or cross — and a wrong word can never hide, which is
  why a typo in a restore is always caught.

- **The dice screen now judges the rolls, not just counts them.** Before, fifty
  presses of one key made a "valid" seed: the hash whitens whatever goes in, so
  the words always look perfect and nothing downstream can tell. Six live
  columns now grow under the six keys with a line at the level a fair die
  levels out to, the exact count sits under each column, and a chip says
  UNEVEN or PATTERN when the rolls do not look rolled — including 1 2 3 4 5 6
  typed over and over, which counts as perfectly level and is still not
  random. The warning explains itself, keeps your rolls, and offers ROLL MORE;
  it never blocks, because the whole point of dice is that you are the source
  the device must not overrule. The bar is set so honest dice trip it about
  once in a million sessions, and the roll ceiling rose from 120 to 180 so
  ROLL MORE has room to actually fix a flagged run. Inspired by Krux's entropy
  measurement, with the threshold derived from exact enumeration instead of
  copied (theirs flags half of all honest sessions).
- **A warning when the amounts cannot be proven.** With two or more coins and no
  previous transactions attached, the fee shown can be lower than the fee paid,
  and no amount of checking inside one transaction can rule it out. The signer
  says so and lets you decide, because signing the same transaction twice is
  perfectly normal when you hold more than one key. Coins spent from a Taproot
  address are not affected, and neither is a single coin spend.
- **DETAILS marks each coin**: a tick where a previous transaction vouched for
  the amount, a struck through eye where it was only claimed.
- **TERMS: the ten words this device has to explain, written once.** PSBT,
  descriptor, fingerprint, entropy and six more. Each one is written in a single
  place and shown wherever a page owes the reader that word, so no two screens
  can explain the same thing differently. The caption is the real term the rest
  of bitcoin uses, never a plain words substitute invented here, and the plain
  words are the sentence under it. Settings > DEVICE > TERMS lists all ten with
  a count of how many you have read, and an unread one carries a dot until you
  close it. What you have read survives an erase: learning is not a secret, and
  making somebody read all ten again because they wiped a seed is the worse
  outcome. A decoy session does not show the list.
- **Coin flips, beside the dice.** The creation path you enter by hand was dice
  only, for no better reason than the six keys on the screen, and nobody owns a
  d6 while everybody owns a coin. 128 flips, recorded as ones and zeroes, judged
  by the same arithmetic the dice rolls are judged by. The string you typed is
  the preimage, so hashing it on any computer reproduces exactly what the screen
  shows.
- **RECEIVE can be told what your coordinator can see.** A signer has no view of
  the chain, so UNUSED under an address was a claim this device had no evidence
  for, on the screen whose whole job is to be trusted. USED still answers from
  what this signer witnessed, since it signed a spend from that address and
  spending proves the address was funded. UNUSED now waits for a coordinator:
  show the coordinator's screen to the camera through RECEIVE > VERIFY, and what
  it claims is kept beside what the device witnessed rather than merged into it.
  With nothing to go on, no chip is drawn at all.
- **A caution when change lands past where your coordinator will look.**
  Somebody holding only your account xpub can hand back a change address that is
  genuinely yours but thousands of indices ahead, past the window every
  coordinator scans. Nothing takes the coins, but they come home to an address
  nobody is watching, and you are told your money is gone until somebody thinks
  to rescan deeply. It is a caution rather than a stop for that reason. Found by
  running the psbt_faker suite against this signer's own parser.
- **FIRMWARE says which version is replacing which.** Five screens that answer
  the same question with the same object: the version on the device and the
  version on the card together, above the offer, above the confirm and above the
  verdict. "Already running" stopped being an amber warning, because it is the
  one refusal where you did nothing wrong.

## [0.1.0-beta7], 2026-07-28

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

## [0.1.0-beta6], 2026-07-27

The plausible-deniability release. Drawing KISS now opens a real signer that is
not your main one, and your main one hides behind a stroke only you know.

### Added: a signer you can hand over

- **A decoy signer, behind the gesture everyone already knows.** Drawing KISS
  opens a working signer with no passphrase: its own fingerprint, pairs with a
  coordinator, signs real transactions. It is not a fake: it is your seed with
  an empty passphrase, which is a genuinely different wallet. Put a small amount
  in it so it is not suspiciously empty.
- **Your real wallet hides behind one extra stroke.** Draw KISS, then add an
  underline, overline, strike, slash, circle or check. Only that opens the
  passphrase prompt. Plain KISS always keeps working and always opens the decoy,
  so nobody can lock themselves out of their own device by forgetting a stroke.
- **Set during setup, changed only from the real wallet.** SETTINGS → WAYS IN
  does not exist in a decoy session, so the decoy never reveals that a second
  signer is configurable.
- **What a seed actually is.** The setup wizard now explains it: 12 words in the
  BIP39 list, that the words plus your passphrase *are* the wallet, and that the
  same words restore into Sparrow, BlueWallet or any BIP39 wallet.
- **A way out of WRITE THESE DOWN.** That screen had no exit at all. If you had
  no paper to hand, the only way out was pulling the power.
- **A warning before CREATE NEW SEED.** It walked straight into the wizard with
  nothing said. WIPE has always gated itself; now so does this.

### Fixed

- **Settings and SCAN QR appeared to freeze, and only a power cycle got out.**
  They were never frozen. Opening the decoy skipped the login screen, and the
  login screen was the only thing that switched on touch input for wallet
  screens, so the screen drew perfectly and ignored every tap. The game kept
  working because it reads the touch panel directly. This affected anyone
  unlocking with plain KISS. **This is the reason to update.**
- **The extra stroke was unreachable.** KISS was recognised the instant you
  lifted the last S, so the decoy opened before you could draw anything after
  it. The device now waits briefly for a stroke.
- **Circles had to be drawn small.** The bigger you drew the loop, the more
  precisely it had to close. Now a big, loosely-closed circle reads correctly.

### Changed

- **KISS is easier to draw.** The shape check was tuned when it guarded the
  passphrase prompt, where a false match was a giveaway. It now opens the decoy,
  where a fumbled shape costs nothing. Two S's are still required.
- **Creating a seed is always 12 words.** 128 bits is not brute-forceable, and
  24 words doubles the length of the one step where mistakes actually happen:
  copying them to paper. RESTORE still accepts 12 or 24.
- **ERASE SEED, not WIPE WALLET.** Erasing this device does not erase your
  wallet: your paper and passphrase still restore it. Calling it "wipe wallet"
  told you your coins were gone, which is the opposite of true and a bad thing
  to believe while deciding whether to press it.
- **The PSBT explainer says what the letters mean.** Partially Signed Bitcoin
  Transaction: a transaction built but not yet signed. The coordinator builds it
  and holds no keys; KISS holds the keys and stays offline. It no longer calls a
  PSBT a "file", because over QR there is no file anywhere.
- **A wallet with no passphrase stops describing one.** The fingerprint and
  warning screens used to talk about a passphrase you did not have, and the
  stroke setup was offered even though both ways in reached the same wallet.

## [0.1.0-beta5], 2026-07-27

Tagged but never published to the installer; its changes ship here in beta6.

- **Silent Payments (BIP352).** Receive at one reusable address without address
  reuse, with scan-key export for watch-only, and spending from silent-payment
  coins (BIP374 DLEQ, BIP375 PSBT outputs), tested against the reference vectors.
- **Amnesic mode.** Load a seed, sign, power off. Nothing is stored.
- **Restore from a SeedQR** instead of typing 12 or 24 words by hand.
- **Every explainer readable at arm's length** in all 21 languages, measured
  rather than guessed, with copy cut roughly in half.
- **Seeds can be sealed to an SD card** rather than kept in flash.

## [0.1.0-beta4], 2026-07-19

The speaks-your-language release: the entire wallet now works in 19 languages
(21 regional variants), with Bitcoin terms as native speakers actually use
them, not word for word translations.

### Added: the wallet in your language

- **21 languages/variants, one firmware.** English, Čeština, Dansk, Deutsch,
  Español (España / México), Français, Hrvatski, Italiano, Nederlands, Norsk
  Bokmål, Polski, Português (Brasil / Portugal), Svenska, Tiếng Việt, Türkçe,
  Русский, 日本語, 한국어, 中文. Every wallet screen, including the home
  tiles, switches instantly. No reflash, no reboot.
- **Language picker with flags.** SETTINGS → LANGUAGE lists every language in
  its own name and script, alphabetically, with the Spanish and Portuguese
  variants side by side. Each name renders in its own regional font, so
  Japanese and Chinese keep their correct character shapes.
- **Choose your language before creating a wallet.** First boot offers the
  picker on the setup screen, so you never create a wallet in a language you
  can't read.
- **Terminology that sounds native.** Each language follows a reviewed
  glossary anchored to Bitcoin Core and established wallet conventions. The
  passphrase is never called a "password" in any language, because it adds a layer
  of security; it does not lock the words.
- The fruit game stays English on purpose. It is the cover story.

### Changed

- **Unlock gesture is more forgiving.** Long KISS swipes no longer drop the
  trailing letters; the check now tolerates a blurred S while still requiring
  the full four-letter shape.
- **Firmware layout changed** (the app partition grew for the new fonts). The
  release image handles this automatically. As with every beta, flashing
  erases the wallet stored on the device. Restore it afterwards from your
  written words and passphrase.

## [0.1.0-beta3], 2026-07-15

The safety-and-clarity release: KISS now explains itself as you use it, and
speaks up in plain words before you sign something you might regret.

### Added: you see and understand more

- **Verify your backup before funding.** WALLET → BACKUP WORDS → **VERIFY MY
  COPY** lets you type your written words from paper; the device confirms they
  rebuild this exact wallet and never shows the stored words. A wrong or missing
  word is reported by position ("word #N"), never revealed. It never alters the
  seed, a safe dry run you can repeat any time.
- **Learn-as-you-go explainers.** A **?** next to any unfamiliar term (RBF,
  fingerprint, coordinator, dust) opens a short plain-words card. Where a picture
  helps, the card carries a small diagram built from the app's own style:
  `WORDS + PASSPHRASE → FINGERPRINT` for the deniability model, and
  `ONLINE APP ←QR→ KISS OFFLINE` for the airgap.
- **Tap the home fingerprint** to learn what that code is (same wallet = same
  code; a different passphrase = a different wallet).
- **Verify a receive address by scanning it.** Receive → **VERIFY** scans the
  address your coordinator shows and confirms on-device whether it is really
  yours, the defense against malware swapping addresses on your computer.
- **Pair Coordinator screen** picks DESKTOP (descriptor, Sparrow-style) or
  MOBILE (zpub, BlueWallet-style), with a nudge to prove the pairing by verifying
  the first address, and to practice with a tiny amount before real coins.
- **EASY SCAN** on the signed-QR screen: bigger dots, slower loop, for phone
  cameras that will not lock onto the default animation.

### Added: warnings before you sign (all soft cautions you acknowledge)

- **High fee.** The fee turns amber if it is a large share of what you send (the
  rule Krux uses) or an outsized sat/vB rate.
- **Dust / privacy.** Spending a tiny coin, or leaving tiny change, is flagged:
  both can be used to link and track your addresses. Change below the network
  dust limit is flagged louder (it may also make the transaction unrelayable).
- **Reused address.** Receive lands on a fresh address and warns if you page back
  to one already used, with a **FRESH** shortcut. Reusing an address links your
  payments together.
- Several cautions **stack** into one summary; a **?** opens a WHY FLAGGED card
  explaining each and the fix (freeze or label the coin in your coordinator).
  When any caution is present, signing is gated behind an **I UNDERSTAND** tap.

### Changed

- The Sign verify screen leads with **YOU ARE SENDING** (amount + fee), shows
  every output with change re-derived on-device, and no longer clutters the main
  view with `locktime 0`. The raw value and a plain words note live in DETAILS.
- Explainer cards now enter with a staggered fade-and-rise animation.

### Safety notes

- KISS has no view of the chain, so the reuse guard and fee/dust warnings are
  best-effort education, not guarantees. Your coordinator remains the source of
  truth for balances and the going fee rate.
- The passphrase is still typed fresh every unlock and never stored; there is no
  "wrong passphrase" error by design.

## [0.1.0-beta2], 2025 (previous release)

- First public beta: the fruit game with the hidden signer behind it, BIP39/BIP84
  single-sig, animated-QR (BC-UR) and SD-card PSBT transport, on-device PSBT
  verification, radio held in reset. Signed release artifacts with GPG + SHA256.
  Not for meaningful funds.
