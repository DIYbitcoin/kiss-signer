# Decisions

Reversals: something was tried on this device, it was wrong, and this is why
what ships is what ships. Generated from `// DECIDED:` comments by
`tools/gen_decisions.py` -- the comment beside the code is the source of truth
and this is an index of it, so a stale entry here is impossible by
construction.

**Read this before filing a defect from a screenshot.** A frame gives you no
route to the comment that answers it, which is how seven findings were filed
and withdrawn in one review pass.

38 decisions.

## `main/kiss_duress_ui.c`

### this family calls it the DECOY, not the spare

It was twelve strings of "spare" against four of "decoy", and "spare" reads as a BACKUP of the real keys -- which is the one thing it is not, and a dangerous thing for an owner to believe about keys they are about to hand over. See i18n/GLOSSARY.md. The key names still say SPARE; only what an owner reads changed. See kiss_duress_ui.h. Four acts: teach it, say to fund the spare, pick your stroke, draw it twice. The draw screens ask for the MODIFIER only, over a printed reference word, rather than the whole "KISS + stroke". Two reasons. The word is not the part being chosen, so rehearsing it teaches nothing new; and a printed reference gives the classifier a known box to measure against, which is the same thing it will measure against in the game (the bounding box of what was drawn). What the owner rehearses here is exactly what has to work later: the stroke, placed relative to the word.

[`main/kiss_duress_ui.c:1`](../main/kiss_duress_ui.c#L1)

## `main/kiss_info.c`

### the KEYS page has no tab strip, and the COORDINATOR element beside its title is a breadcrumb rather than a lone tab

ONE segment. It was "KEYS / COORDINATOR" and the second half restates the title this page already carries. ...and its box STOPS at the head. wt_trail runs to 752 (or 12 short of a [ ? ]) so a short word still owns the strip, which is right on every other screen and wrong on this one: SHOW IT TO now shares the row, and the overlap gate reads boxes rather than glyphs.

[`main/kiss_info.c:596`](../main/kiss_info.c#L596)

## `main/kiss_psbt.c`

### the bar is ntap < n_in and NOT ntap == 0, which refuses more than the argument above strictly requires

One taproot input is in fact enough: BIP341 hashes every input amount into THAT input's sighash, so any lie about any amount invalidates its signature, and the two session attack cannot assemble a tx where every signature verifies. So a spend of one received silent payment beside one P2WPKH coin that carries its full previous transaction is provably honest and is refused anyway. Left strict on purpose. The cost of the strict form is that a rare transaction comes back to the coordinator to be rebuilt with the prev txs attached, which is what BIP174 asks for and what Core, Sparrow and Electrum already send. The cost of the loose form, if the reasoning above is wrong in one case nobody has thought of, is a fee the owner cannot see going to a miner. Those are not the same size, and this is the one gate in the file whose whole subject is a number that cannot be checked afterwards.

[`main/kiss_psbt.c:991`](../main/kiss_psbt.c#L991)

### this concatenated into a 4096 byte automatic and hashed once, with an overflow flag returning -1 if the signatures did not fit

The buffer was a fifth of the main task's 20KB stack, claimed in the frame that runs immediately after signing, where libwally is already deep -- and it was sized for a PSBT this device cannot be handed: the sign screen reads into QRT_MAX_PSBT (4096) TOTAL, framing included, so the signature bytes alone can never come near 4096 and the overflow branch was unreachable. Streaming spends 112 bytes, has nothing to overflow, and drops the branch. Byte-identical output, held by the golden vector in test_crypto.c.

[`main/kiss_psbt.c:1344`](../main/kiss_psbt.c#L1344)

## `main/kiss_recv.c`

### the ADDRESS #N caption opens the list and is not a duplicate of NEXT ADDRESS; it replaced a popover that drifted out of step with it

There was a popover here: five rows, its own pager, its own remembered page, opened by a chevron that bounced forever under the caption. It was a second address picker standing beside ALL ADDRESSES, which is a list of the same hundred indices with a different pagination (3, not 5) and a different remembered position -- and the two drifted apart by construction. A swipe through the list moves s_list_base and never touches s_idx, so an owner who paged the list back to #0 and returned found the popover built around #5. That is the bench's report, and no amount of fixing the popover's memory makes two pickers on one screen agree about which one the reader meant. So the caption IS the way in, and the list is the picker. Tapping it lands on ALL ADDRESSES with the page holding s_idx already under the finger and that row already ticked; row_tap_cb picks and comes straight back. One list, one pagination, and the position is computed from the selection every time, so there is nothing left to drift.

[`main/kiss_recv.c:1010`](../main/kiss_recv.c#L1010)

### VERIFY belongs to THIS ADDRESS, not to the whole page

It was on the band from every pane, including the [ ? ] explainer, on the argument that the explainer's own second sentence -- "check one here before you trust it" -- is what VERIFY does, so the screen was offering the thing it had just taught. The owner disagrees, twice: a control that has nothing to do with the pane under it reads as belonging to that pane. It follows the same rule NEXT ADDRESS already did. The band on the other panes is then BACK alone, which is correct here and not an EXIT finding: this screen has a tab strip, and the strip is the way between panes.

[`main/kiss_recv.c:1122`](../main/kiss_recv.c#L1122)

## `main/kiss_rngaudit.c`

### the pair read "SPREAD / 5000 numbers, 100 groups" and "CANNOT PROVE / software passes it too", and came off the bench as "what are you trying to say"

Both were the middle of a sentence: one named the method without saying what it measures, the other named a limit without saying what a pass would have meant. Three rows say the whole thing in order -- what runs, what a good result looks like, what it still cannot tell you -- and the third is where "spread" is earned, so SPREAD SCORE on the result page arrives with a meaning attached. The value lane is 438px at font28, about 25 characters, so none of these can grow into a sentence: that is the shape doing its job, and anything longer belongs in a paragraph, not a fact row.

[`main/kiss_rngaudit.c:136`](../main/kiss_rngaudit.c#L136)

## `main/kiss_settings.c`

### asked once per page OPEN, not once per card insertion

Keying the cache on the card being present looked cheaper and is wrong in the state that matters most: install the update and the same card is still in the slot holding the same image, now the running version. The scan would say WFW_ERR_SAME, the cache would still say 1, and the dot would sit there pointing at a FIRMWARE row offering nothing -- immediately after the one action that was supposed to clear it. So kiss_settings_open forgets the answer, and everything after it inside that one visit reuses it. A settings page rebuilds on every tab tap, so scanning per build would put an SD read behind a tab. The mark also goes when the card goes, which is what the probe below is for: a dot that outlived the card points at a row that says "no card".

[`main/kiss_settings.c:1838`](../main/kiss_settings.c#L1838)

### the amber dots on the tab strip are the attention chip's ROUTING and cannot be deleted as a duplicate of the count

The first tab carrying one, in strip order, so the chip lands on the leftmost mark and the owner works rightwards. It used to be hard coded to BACKUP on the strength of a comment saying both counted conditions lived there; that stopped being true the moment duress joined the count, and a chip that jumps past a lit dot is worse than one that does not move.

[`main/kiss_settings.c:1875`](../main/kiss_settings.c#L1875)

### ONE door onto AUDIT, and it is this one

The row was duplicated onto BACKUP because "how were these made" is a question about the seed and an outside reader guessed that tab; that argument is still true and did not survive what the pair cost. From the bench, "why the fuck now is there TWO audit button in SETTINGS screens" -- a settings page that lists the same row twice reads as a page that does not know what it holds, and a reader who cannot find a screen is not helped by finding it twice. It was moved to DEVICE for one commit on the reasoning that HOW YOUR KEYS WERE MADE and the RANDOMNESS AUDIT are facts about this box. Wrong tab: what an owner does with them is decide whether to TRUST the box, which is what SECURITY is for, and it is where the row has always been. DEVICE is FIRMWARE, THIS DEVICE and TERMS, and it is three rows again. No value: AUDIT has no state to report, so what it is FOR rides the sub lane. A phrase in the value lane wraps into the chevron -- the value never yields, so it has to be short or absent.

[`main/kiss_settings.c:2083`](../main/kiss_settings.c#L2083)

### the DECOY CARD hides in a decoy session, not the whole row

The row was absent whenever kiss_session_decoy() was true, on the sound-sounding reason that the list contains THE DECOY. What that predicate actually means is "opened with an EMPTY passphrase", which is every signer that has never configured one -- so the reference an owner is pointed at from four other screens was missing from the settings page on the devices most likely to need it, and the bench reported it as the row simply not existing. One card is the secret; the other ten are a glossary. terms_ids() drops that one.

[`main/kiss_settings.c:2212`](../main/kiss_settings.c#L2212)

### these tabs keep their NOUNS and are not renamed after the jobs they hold

The proposal was HOW IT SIGNS / WHAT IT KEEPS / HOW IT PROVES / WHAT IT IS, and it does not fit: the five-up strip is 620px, the shipped labels measure 612 of it, and those four plus NO UNDO measure 920. Three hundred pixels over is not a layout to tune. The only form that fits is single verbs -- SIGNS / KEEPS / PROVES / IS, 500px -- and "IS" is not a word to put on a tab an owner is looking for something in. Verbs without subjects read worse than the nouns they would replace, for a reader who bought their first signing device last week. The miscategorisation the proposal was built on is also gone: it argued that storage sat on SECURITY while recovery words sat on BACKUP, so checking a backup crossed two tabs. STORAGE is on BACKUP beside SEED WORDS, which is one job on one tab.

[`main/kiss_settings.c:2423`](../main/kiss_settings.c#L2423)

### the theme moved off the action band into the chrome column above [ ? ]

It spent a version as a wordless chip in the header, one as a row on the DEVICE tab, and one as a swatch on the band's centre. The band was the wrong shelf: BACK, NEED ATTENTION and LANGUAGE all take you somewhere, and this control repaints the page you are already standing on. That is chrome, the same job as the title and the [ ? ], so it goes in the same column. The band had three controls sharing its right half because of it, and the bench had already filed that once -- "too close to the language picker" -- which was answered by centring it, moving the crowding rather than the control. It is a KIT call and the geometry is not here, for the reason the first draft of it proved: built by hand on this page, the swatch followed the accent through its flag and the NAME beside it did not, so an in-place restyle drew a green swatch labelled MONO.

[`main/kiss_settings.c:2463`](../main/kiss_settings.c#L2463)

### LANGUAGE lives on the BAND, moved there out of the DEVICE tab

It travelled with the theme and the theme has since gone up to the chrome column; this one stays, because picking a language OPENS a picker, which is what every other control on this band does. The language control needs no caption -- its label IS the active language's own name, stripped of the regional qualifier ("ESPANOL (ESPANA)" -> "ESPANOL") because the picker's flag carries the variant. A WORD ACTION with a GLOBE, not an arrow action. It was a forward arrow, which is the mark the SCREEN'S OWN action wears -- so the one control on the band that picks between 21 languages was signed exactly like a "go on", and came back from the bench as "should have some icon better than an arrow, no?". A globe says what the control is before its word is read, in every one of those 21 languages at once.

[`main/kiss_settings.c:2502`](../main/kiss_settings.c#L2502)

## `main/kiss_setup.c`

### a signer with NO passphrase gets the full verdict and no disclaimer

This read s_verify_full alone, which is only true when the passphrase leg actually RAN -- and that leg is skipped outright when there is no passphrase to check. So a device that has never had one showed "SEED WORDS VERIFIED" under a subtitle reading "the passphrase is not part of this check", disclaiming something the owner does not have and cannot add to the check. kiss_rehearse_after_words is the seam that already knows: VERIFIED means the words alone ARE the whole backup. When they are, and they matched, the backup is fully verified and there is nothing to disclaim. The subtitle survives for the case it was written for -- a passphrase in use whose leg was cancelled or not offered.

[`main/kiss_setup.c:469`](../main/kiss_setup.c#L469)

### the dice keys are drawn in wt_accent() over WT_DIV troughs, not in a hardcoded blue

They read stronger than the rest of the device only because six large fills carry the same accent that is hairlines everywhere else -- area against stroke, not a palette break. There is no hardcoded colour in this file outside one dim amber. The keys, each directly over the column it feeds: six for a die, two for a coin. A die's key is the FACE, because that is what is printed on the thing in the owner's hand and it is also the character recorded. A coin has no digits on it, and the keys used to say 0 and 1 -- which made the first act of the flow an invented convention the owner had to hold in their head for 128 taps, before they had done anything. They say HEADS and TAILS now: nothing to decide, nothing to remember, and the words on the keys are the words on the coin. The recorded character is still 0 and 1, so the preimage is still the bit string. The mapping that makes it checkable does not live in anyone's head either -- W_COIN_VERIFY_NOTE prints it directly above the hash, on the one line written for the reader who is going to recompute it.

[`main/kiss_setup.c:2304`](../main/kiss_setup.c#L2304)

### the first boot storage chooser matches the Settings one row for row on purpose, so neither may be reordered alone

Geometry and OBJECT from WT_CHOICE_* and wt_row_x, matching storage_chooser_screen() in kiss_settings.c row for row. The two screens present the identical choice and must not drift apart again, which is why the numbers live in kiss_theme.h and not in either file -- and now the shape does too, which is the drift that actually happened last time. Nothing is selected here. In Settings one of the three IS the current mode and wears the tick; this is first boot, there is no current mode yet, and a tick on FLASH would be the device answering its own question.

[`main/kiss_setup.c:3311`](../main/kiss_setup.c#L3311)

### the FIRST screen of setup has no CANCEL on a signer with no keys, because there is nothing to cancel to

This is the one place the "no screen without an exit" rule is deliberately not applied, and the rule's own case says why: it was written for the WORDS screen, where an owner mid flow could only go forward or pull the power. Here the two choices ARE the way on, and the language picker is in the corner. What CANCEL did instead was strand people. It closed the wizard onto the fruit game, and the only route back into a keyless signer is the KISS draw -- printed on a card in the packaging and nowhere on the glass. So an owner who backed out of setup, or drew the gesture before knowing what it opened, was holding a signing device that had become a game. The obvious fix is the one that must NOT be built: a way in on the cover itself. kiss_seed_exists() is false on an AMNESIC signer with no session loaded and on an SD signer with its card out, so a cover that offers setup whenever there are no keys wears a signer's name permanently on the two modes that need the cover most. That is the decoy, gone. With keys, CANCEL stays exactly as it was: the wizard is reached from Settings then, there is a device behind it, and going back is correct.

[`main/kiss_setup.c:3514`](../main/kiss_setup.c#L3514)

## `main/kiss_sign.c`

### the code is a VALUE on the exit screens again, and it is accent

beta7 moved it into the panel and left "SIGNATURE" as a caption with nothing under it -- on the screens whose whole job is to hand back the one thing a second signer can be checked against. The panel was the right home for the TEACHING and was never the right home for the fact. The ring and the glyph take the accent with the code: they are one thing, and a grey chip beside an accent value read as two.

[`main/kiss_sign.c:753`](../main/kiss_sign.c#L753)

### the montserrat48 tick at y=236 and the padlock beside it are GONE, and the tick joins the title

SIGNED was claimed three times on this screen -- the page title, that 48px checkmark, and a note under it -- while the two facts an owner actually leaves with, which file and what to do next, had no room. One claim, once, on the row that already carries the word. The lock keeps its meaning beside it: where this can go is settled.

[`main/kiss_sign.c:916`](../main/kiss_sign.c#L916)

### the two-line note is gone

"put the card back in Sparrow, then broadcast" was the whole point of the screen set in a 14px note, and it ran three separate actions together in one sentence -- so a reader standing at the device had to work out which of them was theirs to do NOW. Numbered and split, with step 1 lit and the other two not, the strip says where this device's part ends without spending a word on it.

[`main/kiss_sign.c:1026`](../main/kiss_sign.c#L1026)

### any press anywhere ends the motion at once, and nothing ever waits on it

The exit screen is already built and DONE is already live underneath -- the motion is drawn OVER a finished screen rather than in front of one being prepared, so a skip is a delete and not a fast forward. Same rule the drift-home timer was deleted for: a filename an owner is reading back must never be mid scramble, and a screen that will not let go reads as a crash.

[`main/kiss_sign.c:1454`](../main/kiss_sign.c#L1454)

### the motion draws the REAL s_out base64 and the REAL s_sig_fp, never invented bytes

A motion that scrambles plausible looking characters and resolves to something else is a lie told by the one screen whose whole job is to hand back something checkable, and an owner who photographed the frame and compared it would find it. The first row genuinely reads cHNidP8B.

[`main/kiss_sign.c:1470`](../main/kiss_sign.c#L1470)

### the two change rows are exclusive and the dust one wins, even though kiss_psbt.c raises them PER OUTPUT and can therefore set both -- one change output under the dust floor and a second between the floor and 5000

Both bits set draws the dust row only, and caution_all_bits reads back through here, so the gate asks for the rows the owner can see and stays consistent. Splitting them was considered and dropped. It takes the stack to six on a page built for five (SG_ROW_MAX, and the ceiling argument above), and it buys a second row saying "tiny change" beside a row already saying "dust change" -- the same sentence about the same fault, for a two change output transaction almost no coordinator builds. What is lost is real and it is one line of a soft privacy caution, not a claim about where the money goes.

[`main/kiss_sign.c:2501`](../main/kiss_sign.c#L2501)

### the NETWORK badge yields its lane to the LOCKTIME badge, not the other way round

Both are placed out of the same right to left fr chain and both used to end in a bare lv_obj_delete when the line ran out -- and on a long title the one that lost was always the locktime, because it is placed last. That is backwards, and each block's own comment says so. The network drops "because the network is on the DETAILS deck as well, so dropping it here costs the reader a tap, not the fact". The locktime badge EXISTS because its deck row is "where a fact goes to be unread" -- it was added to get that fact off the third tab and onto the glass. So dropping the locktime costs the fact, and dropping the network costs a tap. Measured, not theoretical: on the Dutch and Russian sign screens the title, the network chip and the fingerprint left the locktime badge under its 12px clearance even in the degraded form below, so it took the else branch and an owner signing a time locked payment was never told. The German case in the comment further down is the same bug caught one locale earlier and fixed only as far as German needed. The trade only arises on TESTNET, because that is the only time a network chip is drawn at all, and it fails in the safe direction. A missing network chip is what MAINNET looks like, so an owner who loses it reads the screen as more serious than it is and is more careful, not less. A missing locktime is the opposite: the payment reads as spendable now, and it is not.

[`main/kiss_sign.c:2756`](../main/kiss_sign.c#L2756)

### the sign review band does NOT name the transaction's file; that line was cut rather than fixed

NO FILENAME HERE, and no "camera" either. Both were cut rather than fixed. The reader tapped that name on the file list one screen back, or watched the camera assemble the transaction, so the line restated what they had just done -- which is the copy rule's own example of a string to cut. And a filename is the COORDINATOR'S bookkeeping, not a fact about the payment: what says this is the right transaction is the amount and the destination, and both are on this screen at full size. A name in the corner never checked anything. What it cost to keep was the whole band. The line ran under the title beside the signed badge, the network chip and the fingerprint, and it was the only unlabelled string among them. It also carried two defects for as long as it existed: set in the smallest face on the device, and, once that was corrected, handing a translated phrase to a filename tail-fold so a scanned transaction read "scan...ansaction" on the screen where a payment is approved. fx survives because the badge and the chip above measure their lane against it: they may not run back under the title.

[`main/kiss_sign.c:2938`](../main/kiss_sign.c#L2938)

### this toggle's mark stays VISIBLE when it is off, dimmed rather than transparent

The two-state word action hides its mark elsewhere and that is right where a PAIR of them sits side by side -- the dice screen -- because the pair is the affordance. This one stands alone in a left aligned column, and hidden-but-still-occupying-space gave it no affordance at all AND pushed its word 33px inside the column's edge, so it read as a centred heading rather than a control. An inert mark says both things at once: there is a switch here, and it is not on.

[`main/kiss_sign.c:4645`](../main/kiss_sign.c#L4645)

## `main/kiss_theme.c`

### the amber lift asks WHAT it is colouring, and never lifts a mark

The rule wt_ink_for serves says it in its own words -- the caution GLYPH and the breathing dot keep the amber, and anything READ takes the accent -- but the function was only ever handed a colour, so it lifted both. A row whose VALUE is LV_SYMBOL_WARNING had its caution sign painted the theme's colour: SEED WORDS reported the paper unchecked in green on the green theme, which is the one row on the page where amber is the whole message. A caution's WORDS still take the accent. Only the mark keeps the amber.

[`main/kiss_theme.c:602`](../main/kiss_theme.c#L602)

### the mark is font23, not the font14 every other mark on this device wears

It shipped at 14 and came off the bench as too small to see and too small to aim at -- the same report the content tab LABELS got when they were 18, and this tab sits in the same 30px strip beside them. So the mark takes the tab rung, chrome23, and the brackets stay mono18 punctuation a rung below it exactly as they do on a content tab. Measured: the glyph goes 11x17 -> 17x25 in a strip 30 tall.

[`main/kiss_theme.c:4112`](../main/kiss_theme.c#L4112)

### the tab breathes whenever it has something UNREAD, not only until the first open ever

It pulsed once, on the first [ ? ] an owner ever met, and was still forever after -- so [ ? 3 ] drew the count and then sat there, which is a badge you have to be looking at to notice. The attention dot on a content tab has answered the same question since it was filed from the bench as "not pulsing", and it answers a GLANCE. This is the same statement, so it is the same motion: 100..255 over 1200ms ease in out, the values wt_dot_breathe uses, rather than the 71..230 this one had of its own. The size and translate halves of a dot's breathe do not come with it -- growing a tab in a 30px strip moves the brackets, and the pixels are spent. It stops on its own. The count comes from kiss_terms_unread at build, the explainer swaps the screen, and coming back rebuilds the tab with whatever is left; at zero there is no animation to delete.

[`main/kiss_theme.c:4178`](../main/kiss_theme.c#L4178)

### a caution value is lifted into the accent only where the row has a LAMP to carry the amber

The lift was unconditional, on the argument written here for its whole life -- "its lamp is the amber, and the lamp is what the eye lands on first anyway". That argument is the lamp's, not the value's, so a row with no lamp was borrowing a reason it did not have: SETTINGS > SIGNER lost its pulsing dot (a pulse means a tab needs attention, and being on signet does not), and the word SIGNET went on reading in the theme's own colour with nothing amber left anywhere on the row. Reported from the bench in those terms. So the condition is the lamp. No lamp, no lift, and the caution stays the caution's colour.

[`main/kiss_theme.c:4694`](../main/kiss_theme.c#L4694)

### a lone span is a break opportunity, so when the word before a stop ends near the edge the "

" wraps by ITSELF and the next line opens with a full stop. It looks like a typo in the string and it is not -- SIGN's refusal screen shows it on "information" / ". pair it again". Folding the stop back into the body run fixes it and was rejected: the accent stop is the design, it is what makes a wrapped body scan as sentences rather than as a block, and LVGL gives no way to hold a span to the one before it. The copy moves instead, which is what happened here -- the word at the edge changes and the stop follows it up.

[`main/kiss_theme.c:6766`](../main/kiss_theme.c#L6766)

### the icon grid's ladder floors at 21 and no longer has a font14 rung

THE FLOOR IS 21, NOT 14, which is the same floor wt_body_para has and for the same reason: font14 is for MARKS -- chip labels, unit suffixes, chevrons -- and every string in this grid is a SENTENCE an owner reads before signing. WHY FLAGGED is the case that proves it: five caution rows explaining why a payment was flagged, all of them at the size this device keeps for punctuation. mono21 only where the copy CAN be mono, which is what the body ladder asks too. Where it cannot, the rung stays 23 and the overflow is reported rather than shrunk away -- copy too long for its box is copy to cut, and a silent drop is what hid this for the grid's whole life.

[`main/kiss_theme.c:7116`](../main/kiss_theme.c#L7116)

### an output not on this page draws NO STRAND

It used to draw a dimmed one, on the reasoning that the shape of the transaction should not leave while its detail is read -- and what that produced was a line running to blank glass, because the row it aims at is hidden. There is nothing at the end of it and nothing that says why, so it reads as a destination the screen will not name: the one thing this graph exists to never do. It was reported from the bench as a strand "going to nowhere", twice, once about its colour and once about the strand itself. What is lost is the fan on a paged spend, and the counter on the caption line carries that instead -- 1/2 is on the glass beside WHERE IT GOES, and the read-to-the-end gate holds the slide until every page has been turned, so no signature can happen from one page's worth of strands.

[`main/kiss_theme.c:8156`](../main/kiss_theme.c#L8156)

## `main/kiss_theme.h`

### the destructive group is a TAB with its own tint and cross-fade, not a row buried on another page

[`main/kiss_theme.h:954`](../main/kiss_theme.h#L954)

## `main/main.c`

### the home carries NO next-step line

It said "check your paper against these keys" until the paper was checked, then "pair a coordinator, then verify an address" until one had spoken, and it came off the bench as a first-time-user walkthrough on the screen the owner looks at every day. The order it was teaching is in docs/walkthrough.md, which is where the owner asked for it to live -- the same argument that took the passphrase line off this screen, a few paragraphs down in kiss_home_build(). The tiles are the home. A signer that keeps suggesting the next thing is a signer that never finishes setting itself up. tile title string ids, in tile order (sign, receive, keys, settings). STR_H_TILE_WALLET is a legacy KEY NAME whose value has been "Keys" for a while; renaming the key would touch all 21 locale files for nothing.

[`main/main.c:248`](../main/main.c#L248)

### a draw that can no longer become the word is cleared HERE, on the lift, not left to the 3s idle

The word is the FIRST stored.strokes strokes of the buffer, so once that many have been drawn without matching, no later stroke can change the answer -- and every attempt after it appended to the corpse instead of starting fresh. The device then answered to nothing at all until the owner put their hand down for a full three seconds, which is not what a person does between two tries. Reported from the bench as a signer that would not open to its own word. The log showed the strokes counting 1..17 across five attempts, 2.5s apart, and never resetting. The heuristic that catches an abandoned KISS is switched OFF whenever a word is stored -- it is built on the letters being drawn left to right, which a custom word is not -- so the idle was the only clear there was.

[`main/main.c:2261`](../main/main.c#L2261)

## `sim/sim_main.c`

### a frame saved too soon photographs the OUTGOING pane, and reads as a layout bug rather than a timing one

pump(30), not 20. The outgoing pane leaves on a per row stagger -- (n-1) * MO_OUT_STEP + MO_OUT_MS, which is 330ms for a six row detail pane against 320ms of pump -- so the old count photographed the previous screen still fading through this one. Invisible until overlapcheck learned to read spangroups: the ghost is a folded address, and a spangroup was not text to any check on the list.

[`sim/sim_main.c:4526`](../sim/sim_main.c#L4526)
