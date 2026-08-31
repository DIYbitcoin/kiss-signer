# Decisions

Reversals: something was tried on this device, it was wrong, and this is why
what ships is what ships. Generated from `// DECIDED:` comments by
`tools/gen_decisions.py` -- the comment beside the code is the source of truth
and this is an index of it, so a stale entry here is impossible by
construction.

**Read this before filing a defect from a screenshot.** A frame gives you no
route to the comment that answers it, which is how seven findings were filed
and withdrawn in one review pass.

16 decisions.

## `main/kiss_duress_ui.c`

### this family calls it the DECOY, not the spare

It was twelve strings of "spare" against four of "decoy", and "spare" reads as a BACKUP of the real keys -- which is the one thing it is not, and a dangerous thing for an owner to believe about keys they are about to hand over. See i18n/GLOSSARY.md. The key names still say SPARE; only what an owner reads changed. See kiss_duress_ui.h. Four acts: teach it, say to fund the spare, pick your stroke, draw it twice. The draw screens ask for the MODIFIER only, over a printed reference word, rather than the whole "KISS + stroke". Two reasons. The word is not the part being chosen, so rehearsing it teaches nothing new; and a printed reference gives the classifier a known box to measure against, which is the same thing it will measure against in the game (the bounding box of what was drawn). What the owner rehearses here is exactly what has to work later: the stroke, placed relative to the word.

[`main/kiss_duress_ui.c:1`](../main/kiss_duress_ui.c#L1)

## `main/kiss_info.c`

### the KEYS page has no tab strip, and the COORDINATOR element beside its title is a breadcrumb rather than a lone tab

ONE segment. It was "KEYS / COORDINATOR" and the second half restates the title this page already carries.

[`main/kiss_info.c:505`](../main/kiss_info.c#L505)

## `main/kiss_recv.c`

### the ADDRESS #N caption opens the list and is not a duplicate of NEXT ADDRESS; it replaced a popover that drifted out of step with it

There was a popover here: five rows, its own pager, its own remembered page, opened by a chevron that bounced forever under the caption. It was a second address picker standing beside ALL ADDRESSES, which is a list of the same hundred indices with a different pagination (3, not 5) and a different remembered position -- and the two drifted apart by construction. A swipe through the list moves s_list_base and never touches s_idx, so an owner who paged the list back to #0 and returned found the popover built around #5. That is the bench's report, and no amount of fixing the popover's memory makes two pickers on one screen agree about which one the reader meant. So the caption IS the way in, and the list is the picker. Tapping it lands on ALL ADDRESSES with the page holding s_idx already under the finger and that row already ticked; row_tap_cb picks and comes straight back. One list, one pagination, and the position is computed from the selection every time, so there is nothing left to drift.

[`main/kiss_recv.c:1017`](../main/kiss_recv.c#L1017)

## `main/kiss_settings.c`

### the amber dots on the tab strip are the attention chip's ROUTING and cannot be deleted as a duplicate of the count

The first tab carrying one, in strip order, so the chip lands on the leftmost mark and the owner works rightwards. It used to be hard coded to BACKUP on the strength of a comment saying both counted conditions lived there; that stopped being true the moment duress joined the count, and a chip that jumps past a lit dot is worse than one that does not move.

[`main/kiss_settings.c:1744`](../main/kiss_settings.c#L1744)

### these tabs keep their NOUNS and are not renamed after the jobs they hold

The proposal was HOW IT SIGNS / WHAT IT KEEPS / HOW IT PROVES / WHAT IT IS, and it does not fit: the five-up strip is 620px, the shipped labels measure 612 of it, and those four plus NO UNDO measure 920. Three hundred pixels over is not a layout to tune. The only form that fits is single verbs -- SIGNS / KEEPS / PROVES / IS, 500px -- and "IS" is not a word to put on a tab an owner is looking for something in. Verbs without subjects read worse than the nouns they would replace, for a reader who bought their first signing device last week. The miscategorisation the proposal was built on is also gone: it argued that storage sat on SECURITY while recovery words sat on BACKUP, so checking a backup crossed two tabs. STORAGE is on BACKUP beside SEED WORDS, which is one job on one tab.

[`main/kiss_settings.c:2219`](../main/kiss_settings.c#L2219)

### language and theme live on the BAND, moved there out of the DEVICE tab

The band's centre: LANGUAGE and THEME, out of the DEVICE tab. The language control needs no caption -- its label IS the active language's own name, stripped of the regional qualifier ("ESPANOL (ESPANA)" -> "ESPANOL") because the picker's flag carries the variant. A WORD ACTION with a GLOBE, not an arrow action. It was a forward arrow, which is the mark the SCREEN'S OWN action wears -- so the one control on the band that picks between 21 languages was signed exactly like a "go on", and came back from the bench as "should have some icon better than an arrow, no?". A globe says what the control is before its word is read, in every one of those 21 languages at once.

[`main/kiss_settings.c:2280`](../main/kiss_settings.c#L2280)

## `main/kiss_setup.c`

### the dice keys are drawn in wt_accent() over WT_DIV troughs, not in a hardcoded blue

They read stronger than the rest of the device only because six large fills carry the same accent that is hairlines everywhere else -- area against stroke, not a palette break. There is no hardcoded colour in this file outside one dim amber. The keys, each directly over the column it feeds: six for a die, two for a coin. A die's key is the FACE, because that is what is printed on the thing in the owner's hand and it is also the character recorded. A coin has no digits on it, and the keys used to say 0 and 1 -- which made the first act of the flow an invented convention the owner had to hold in their head for 128 taps, before they had done anything. They say HEADS and TAILS now: nothing to decide, nothing to remember, and the words on the keys are the words on the coin. The recorded character is still 0 and 1, so the preimage is still the bit string. The mapping that makes it checkable does not live in anyone's head either -- W_COIN_VERIFY_NOTE prints it directly above the hash, on the one line written for the reader who is going to recompute it.

[`main/kiss_setup.c:2274`](../main/kiss_setup.c#L2274)

### the first boot storage chooser matches the Settings one row for row on purpose, so neither may be reordered alone

Geometry and OBJECT from WT_CHOICE_* and wt_row_x, matching storage_chooser_screen() in kiss_settings.c row for row. The two screens present the identical choice and must not drift apart again, which is why the numbers live in kiss_theme.h and not in either file -- and now the shape does too, which is the drift that actually happened last time. Nothing is selected here. In Settings one of the three IS the current mode and wears the tick; this is first boot, there is no current mode yet, and a tick on FLASH would be the device answering its own question.

[`main/kiss_setup.c:3271`](../main/kiss_setup.c#L3271)

### the FIRST screen of setup has no CANCEL on a signer with no keys, because there is nothing to cancel to

This is the one place the "no screen without an exit" rule is deliberately not applied, and the rule's own case says why: it was written for the WORDS screen, where an owner mid flow could only go forward or pull the power. Here the two choices ARE the way on, and the language picker is in the corner. What CANCEL did instead was strand people. It closed the wizard onto the fruit game, and the only route back into a keyless signer is the KISS draw -- printed on a card in the packaging and nowhere on the glass. So an owner who backed out of setup, or drew the gesture before knowing what it opened, was holding a signing device that had become a game. The obvious fix is the one that must NOT be built: a way in on the cover itself. kiss_seed_exists() is false on an AMNESIC signer with no session loaded and on an SD signer with its card out, so a cover that offers setup whenever there are no keys wears a signer's name permanently on the two modes that need the cover most. That is the decoy, gone. With keys, CANCEL stays exactly as it was: the wizard is reached from Settings then, there is a device behind it, and going back is correct.

[`main/kiss_setup.c:3474`](../main/kiss_setup.c#L3474)

## `main/kiss_sign.c`

### the sign review band does NOT name the transaction's file; that line was cut rather than fixed

NO FILENAME HERE, and no "camera" either. Both were cut rather than fixed. The reader tapped that name on the file list one screen back, or watched the camera assemble the transaction, so the line restated what they had just done -- which is the copy rule's own example of a string to cut. And a filename is the COORDINATOR'S bookkeeping, not a fact about the payment: what says this is the right transaction is the amount and the destination, and both are on this screen at full size. A name in the corner never checked anything. What it cost to keep was the whole band. The line ran under the title beside the signed badge, the network chip and the fingerprint, and it was the only unlabelled string among them. It also carried two defects for as long as it existed: set in the smallest face on the device, and, once that was corrected, handing a translated phrase to a filename tail-fold so a scanned transaction read "scan...ansaction" on the screen where a payment is approved. fx survives because the badge and the chip above measure their lane against it: they may not run back under the title.

[`main/kiss_sign.c:1810`](../main/kiss_sign.c#L1810)

### this toggle's mark stays VISIBLE when it is off, dimmed rather than transparent

The two-state word action hides its mark elsewhere and that is right where a PAIR of them sits side by side -- the dice screen -- because the pair is the affordance. This one stands alone in a left aligned column, and hidden-but-still-occupying-space gave it no affordance at all AND pushed its word 33px inside the column's edge, so it read as a centred heading rather than a control. An inert mark says both things at once: there is a switch here, and it is not on.

[`main/kiss_sign.c:3207`](../main/kiss_sign.c#L3207)

## `main/kiss_theme.c`

### the icon grid's ladder floors at 21 and no longer has a font14 rung

THE FLOOR IS 21, NOT 14, which is the same floor wt_body_para has and for the same reason: font14 is for MARKS -- chip labels, unit suffixes, chevrons -- and every string in this grid is a SENTENCE an owner reads before signing. WHY FLAGGED is the case that proves it: five caution rows explaining why a payment was flagged, all of them at the size this device keeps for punctuation. mono21 only where the copy CAN be mono, which is what the body ladder asks too. Where it cannot, the rung stays 23 and the overflow is reported rather than shrunk away -- copy too long for its box is copy to cut, and a silent drop is what hid this for the grid's whole life.

[`main/kiss_theme.c:6219`](../main/kiss_theme.c#L6219)

## `main/kiss_theme.h`

### the destructive group is a TAB with its own tint and cross-fade, not a row buried on another page

[`main/kiss_theme.h:860`](../main/kiss_theme.h#L860)

## `main/main.c`

### the home's next-step hint is a CONTROL, not a caption

It wears LV_SYMBOL_RIGHT, which on this device means "this opens a screen" -- and it opened nothing, so the one mark whose whole job is to promise navigation was making a promise the label could not keep. It goes where it points now.

[`main/main.c:248`](../main/main.c#L248)

### a draw that can no longer become the word is cleared HERE, on the lift, not left to the 3s idle

The word is the FIRST stored.strokes strokes of the buffer, so once that many have been drawn without matching, no later stroke can change the answer -- and every attempt after it appended to the corpse instead of starting fresh. The device then answered to nothing at all until the owner put their hand down for a full three seconds, which is not what a person does between two tries. Reported from the bench as a signer that would not open to its own word. The log showed the strokes counting 1..17 across five attempts, 2.5s apart, and never resetting. The heuristic that catches an abandoned KISS is switched OFF whenever a word is stored -- it is built on the letters being drawn left to right, which a custom word is not -- so the idle was the only clear there was.

[`main/main.c:2310`](../main/main.c#L2310)

## `sim/sim_main.c`

### a frame saved too soon photographs the OUTGOING pane, and reads as a layout bug rather than a timing one

pump(30), not 20. The outgoing pane leaves on a per row stagger -- (n-1) * MO_OUT_STEP + MO_OUT_MS, which is 330ms for a six row detail pane against 320ms of pump -- so the old count photographed the previous screen still fading through this one. Invisible until overlapcheck learned to read spangroups: the ghost is a folded address, and a spangroup was not text to any check on the list.

[`sim/sim_main.c:4111`](../sim/sim_main.c#L4111)
