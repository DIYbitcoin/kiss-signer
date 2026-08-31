# KISS Signer — working rules

## Replies

**Hard cap: about five sentences.** A few sentences, and the picture when a
screen changed. No summaries of what was just done, no lists of what is left, no
restating the commit message back — it is all in the commit and the code. Detail
only when asked for it, and "asked" means asked, not inferred.

This rule was already here, in these words, and was broken on essentially every
turn anyway. It kept being overridden by a judgement call, so the judgement call
is gone: length is not a thing to weigh, it is a limit. Every one of these is a
rationalisation and none of them earns a longer reply:

- "this finding is important" — then it is a commit message.
- "I should explain why I chose this" — commit message.
- "I found a second bug while in there" — one sentence, no section.
- "I should flag what is still untested" — one sentence, or the device-test
  verdict, which has its own required form and does not license prose around it.
- "the owner will want the reasoning" — the owner asks when they want it, and
  asks constantly for the opposite.

Bullet lists, bold headers and section breaks in a reply are the tell that it
has already gone too long. The commit body is unlimited; the reply is not.

## Screen chrome

A content screen is **not** a title, a paragraph and a button. That shape has
shipped more than once and it is the thing being corrected. Every screen the
owner reads is built from the kit in `main/kiss_theme.c`; nothing here needs
inventing.

| What the screen needs | Use |
| --- | --- |
| the figure it is about, framed | `wt_value_card(scr, cap, val, x, y, w, big)` |
| a panel to group content | `wt_card(scr, x, y, w, h)` |
| a relationship, drawn | `wt_diagram_row` + `wt_chip` + `wt_diagram_op`, or `wt_diagram_fp` / `wt_diagram_verify` / `wt_diagram_pair` |
| two claims, not one paragraph | `wt_facts(scr, y, facts, n)` — a caption, a mark and a one-line value each |
| a page that explains itself | `wt_explain(scr, headline, para, facts, n)` — headline, one paragraph, the rows above |
| a list of settings or facts | `wt_row` / `wt_row_x` / `wt_row_head` |
| the camera | `wt_viewfinder` |
| actions | `wt_arrow_action` on the band, `wt_word_action` in a row |

Rules:

1. **Something framed, above the action row.** A bare paragraph is never the
   only content. A pill does not count — it is the action, not the subject.
2. **Split claims, do not stack them.** A claim is a `wt_facts` row: a caption
   in the accent, a mark before it, and a value on ONE line at font28. Two or
   three of them under a headline and a single paragraph is `wt_explain`, and
   that is the shape of every screen an owner reads.

   **The pair of ruled blocks is gone, and so is the code.** `wt_why_block`
   drew two columns of grey with a coloured bar down the side of each, at
   `x = 48` and `x = 408`, `w = 344` — the shape this table used to send people
   to. It came back off the bench three separate rounds, ending with *"basically
   any page with those vertical lines on the side"*, and the last sixteen
   screens wearing it were rebuilt in one pass. `wt_why_block`, `wt_why_body`,
   `wt_body_font2` and `wt_body_font2_head` were deleted with it; do not
   reintroduce a third arrangement.

   What survives is the MEASURING, in `wt_body_para` / `wt_body_para_to`: the
   largest rung the whole body fits at, one label per paragraph, and a report
   through the FIT sink when even the floor will not hold it. Never subtract a
   guessed heading height from a budget — four call sites did, three passing
   `BH - 46 - 8` and one a bare `112`, giving away 54px of a 166px budget in
   all 21 locales. A third of the room is the difference between font23 and
   font14.

   **font14 is metadata: chip labels, unit suffixes, chevrons. MARKS. Nothing
   an owner has to READ is ever font14.** It has now been reported from the
   bench four separate times, and the third was not a body budget at all
   — it was the SIGNED screen's "what to do next" line, the single most
   important sentence on that screen, set through `wt_note` in a 48px box it
   could not fit at any larger size.

   `wt_note_fit` picks the biggest font that FITS. That makes
   it silent: hand it a long string in a small box and it drops to font14
   and report nothing, so the string never looks like a bug in the source. **A
   fit helper landing on font14 means the copy is too long for the space, not
   that the space is too small — cut words first.** The SIGNED line named the
   filename that is already on screen in font28 directly below it, which the
   copy rule says to cut anyway; cutting it took the line from font14 to font28
   with no layout change at all.

   **"row sublines" used to be on that exempt list and it was the fourth
   report.** Every teaching line on the settings page is a row subline — "not
   real bitcoin", "opens your real keys", "amount in sats or BTC" — so the
   carve-out exempted the page's entire body copy, in `wt_row_wide` and in the
   FIT gate both, and it came back from the bench as *"no more tiny text
   anywhere"*. A subline is a SENTENCE and sits at font23.

   What goes wrong at font23 is not a rung, it is an **ellipsis**: a subline is
   pinned to one line with `LV_LABEL_LONG_DOT`, so copy too long for its lane
   loses its second half and says nothing about it. LVGL rewrites the label's
   own text to insert the dots, so a gate walking the finished tree finds a
   string that measures exactly one lane wide and no evidence at all — which is
   why `overlapcheck`'s **CUT** check measures in `kiss_theme.c` as the label is
   built and reports through a sink, the same shape FIT uses. An ellipsis there
   means cut the copy: the lane is what the label's 250px cap and the value chip
   leave behind, and both of those are load bearing.

   **font14 is a BUG in a body, not a translation being long.** It was reported from the bench as "WHY IS THE TEXT
   SO SMALL, LITERALLY, I KEEP ASKING" — about the confirm screen for replacing
   the unlock drawing, a screen with a 2000ms hold on it, whose two claims were
   set at font14 under 165px of empty glass. Nothing was long; the budget had
   been thrown away in code.

   Two habits that catch it, in order:

   - **Look at the frame.** Every one of these was visible at a glance and none
     of them was caught by a gate. `bash sim/build_sim.sh && /tmp/fruitsim` and
     open the .ppm — the house rule about shipping a picture exists for exactly
     this and it is the only check that sees type size.
   - **Count the empty band first.** A screen whose blocks start at `y = 232`
     with nothing above them has 165px doing nothing and a body starving in the
     rest. Move the blocks up, or put the thing the screen is ABOUT in that band
     (rule 1 wants it there anyway). Do not shrink the type to fit a layout that
     was never full.
3. **A blank line costs a whole line of type.** `exp_height` puts one full
   `lv_font_get_line_height` between paragraphs, so at font28 each blank line is
   ~38px. A three paragraph body pays it twice. **When a screen renders smaller
   than it should, count its paragraphs before you cut words**: merging two is
   usually worth more than any rewrite, and it is what finally moved the seed
   explainer off font14. Instrument the ladder in `wt_body_para` rather than
   estimating — every hand estimate in this file's history has been wrong.
4. **Marks before words.** Every chip and row label carries an icon.
5. **Only glyphs already in `SYMS`** (`tools/fonts/gen_fonts.sh`). Anything else
   forces a font rebuild across four scripts. Available at every size: all
   `LV_SYMBOL_*` plus `WT_ICON_QR/KEY/SECRET/SD/LOCK/REPLACE`.
6. **Nothing crosses `WT_CONTENT_BOTTOM` (398).**

## Vocabulary

**THIS IS A SIGNING DEVICE. IT IS NOT A WALLET, AND NOTHING IN IT IS "THE
WALLET".** That is the owner's own sentence, shouted, after it was got wrong
again. The home page says *offline bitcoin signing device* and every other
surface answers to that: the box is a **signing device** or a **signer**, what
it holds is **keys**, and what it remembers is **what this signer has seen**.

So **"wallet" is not an available word for anything this device is or does.**
Not the box, not a screen, not a setting, not a stored fact. These were all
written and all had to be pulled back out:

| written | write instead |
| --- | --- |
| "settings, and what this wallet has seen" | "…what this signer has seen" |
| "wallet history needs encrypted flash" | "what this signer has seen needs…" |
| "erases the wallet history stored now" | "erases what this signer has seen" |
| "moving an AMNESIC wallet to storage" | "…an AMNESIC signer" / "these keys" |

The ONE surviving use is the coordinator's object: a **wallet** is the key set
plus its coins that Sparrow or Nunchuk watches. Even there, prefer **keys**
whenever the sentence is about this device — "your passphrase opens different
keys", not "a different wallet". If a string can be written without the word,
write it without the word.

Mirror the words Bitcoin signing devices already use; never invent a house term
a reader has to unlearn the first time they read anything else.
`i18n/GLOSSARY.md` is the authority and has the 21 locale anchors — check it
before naming anything.

The three that got tangled, and cost a full sweep to untangle:

| | |
| --- | --- |
| **signing device** / **signer** | this box. Holds keys, signs offline. The whole device, never one screen or one tile. The product's own name for itself. |
| **wallet** | ONLY the coordinator's object: a key set and the coins it controls, watched by Sparrow or Nunchuk. Never this device, never anything stored on it. Prefer **keys** anywhere the sentence is about the box. |
| **keys** | what the signer holds and what the fingerprint identifies. The default noun. Use it wherever "wallet" is tempting. |

Recovery words + passphrase → **keys**; the fingerprint is what those keys are
**called**; the **signer** is the box; the **wallet** is what a coordinator
sees. A sentence like "an empty wallet on a signer" is describing two different
things and needs rewriting.

### The thing has a name. Write the name.

**"seed words".** Not "your words", not "the words", not "paper words". The
glossary already said so — *bare "words", used as if it named the thing,
reads as a house term and has to be unlearned the first time an owner opens
anything else* — and the LOCKED BACKUP explainer was written **"A locked copy
of your words"**, corrected to **"Keep your paper words too"**, and only
reached "seed words" after the owner asked for it twice, shouting the second
time. Two invented terms in a row, on the screen where an owner decides
whether a second copy of their seed words gets made.

The failure is not carelessness about one word, it is a habit: reaching for a
shorter phrase because the line has to fit. **When the name does not fit, cut
another word, never the name.** "Keep your seed words on paper too" did not
fit in two lines; "Seed words still go on paper" does, and says the same
thing. The name was never the part to give up.

So `tools/check_vocab.py` now reads `i18n/en.json` and fails on it, wired into
`desktop-tests.yml` beside the orphan check. Four rules — BARE-WORDS,
COINED-WORDS (any modifier bolted onto `words`), BARE-SEED, WALLET — each with
the ALLOW list of uses that are correct and why (Sparrow's own menu path, the
sender's wallet), and a shrink-only BACKLOG holding the six strings GLOSSARY.md
already lists as unconverted. `VOCAB_SELFTEST=1` asserts every rule still fires
on the string it was written for AND stays quiet on the string that fixed it,
and the gate refuses to report at all if a rule is dead. Both strings above
fail it.

Same rule for everything else on screen: storage is **storage**, not "where
your words live". If a mainstream signer has a word for it, use that word.

### "card" is the SD card. Never a thing on the screen.

The device has a slot. Forty odd strings say **card** and mean the thing in
that slot — `S_NO_SD`, `W_SD_MISSING_B`, `G_FW_ON_CARD`, `W_PROOF_R_S`, all of
them. So the word is spoken for, in the UI and in every reply about it.

It leaked anyway. The tap entropy screen read **"tap anywhere on the card. any
rhythm."** and meant the panel under the finger, on a screen reached by owners
who have just been told to insert an SD card — in font14, as the only
instruction on the screen. In twenty locales it is still *Karte*, *tarjeta*,
*カード*. That is the whole failure: a reader looks at the slot.

- On screen, name the thing: **the box**, **the panel**, or better, "in here",
  "below", "on the left". There is no ready-made string for it any more:
  `GD_WORD_DRAW_S` shipped "write it on the panel" in 21 locales and was
  deleted with the other 32 keys nothing referenced.
- **The blind draw is a draw from a word list, not "cards".** `W_CARDS_*` is a
  key prefix and nothing more; no English string in that flow says card, and
  none may start.
- In replies, "the checksum card" means nothing to the owner — say "the
  explainer", "the panel", or the screen's own title.

## Copy

**Ordinary Bitcoin and computer words, said the way somebody would say them
out loud.** The reader is somebody who bought their first signing device last
week. They are standing up, reading once, deciding something. They are not
reading for pleasure and nothing on the glass is worth being clever in.

**The test: if the owner can reply "what does that mean?", it is wrong.** That
is not hypothetical, it is what happened to *"Optional. Never instead of
paper."* -- five words, no jargon, and it still had to be explained, because
an instruction phrased as the negation of something else makes the reader do
the work. It says *"Optional. Seed words still go on paper."* now.

- **Say what to DO.** Not what not to do, not what it is not instead of.
- **A sentence is under fourteen words**, because that is what anybody says
  in one breath.
- **No metaphor.** Data does not sit, live, travel or sleep anywhere.
  "It can sit where plain words could not" was written on this screen and is
  two failures in one line.
- **No four syllable word** that Bitcoin or a computer did not already make
  the reader learn. `coordinator` and `derivation` earn their length;
  nothing else on this device does.
- **The name in full, every time** -- see the Vocabulary rule above. When the
  line will not fit, cut a different word, never the name.
- No hyphens in English screen or explainer text.
- Cut any string that restates the title, or a value sitting next to it.
- Headings in a pair are parallel: "not stored" / "not recoverable".
- Prefer a mark to a word wherever the mark is unambiguous.

`python3 tools/check_vocab.py` fails on the mechanical half of this --
APHORISM, LONG-SENTENCE, LONG-WORD alongside the naming rules -- and every
one of those three rules is there because a string in this session tripped
it. The half it cannot see is whether the sentence sounds like a person, and
that is what reading it aloud is for. Do that before the commit, not after
the owner asks.

The record of one screen, because the shape of the failure repeats: the
LOCKED BACKUP explainer went

| | |
| --- | --- |
| shipped | "A QR only your password opens." | 
| then | "A locked copy of your words." |
| then | "Optional. Never instead of paper." |
| landed | "A locked copy of your seed words." / "Optional. Seed words still go on paper." |

Four rounds, three of them spent on the owner asking for plain words and the
right name. Every one of the first three was shorter, cleverer, and worse.

## Gates

Run before claiming anything works. None of them can see a hardware problem.

**`SIM_LANG=en` on the ones that sweep locales**, for the reason the i18n
section gives, and written into the commands rather than left as a rule to
remember. Without it `kissosd` is RED on an Italian string at font28 that is
1037px wide against its 1000px comparison canvas -- a translation waiting for
the sweep, exactly as `sim/osdcheck.c` says beside its own SIM_LANG filter. A
gate that is red for a reason nobody is acting on is a gate nobody reads, and
this block was the last place still telling people to run it that way.

```bash
bash sim/build_test.sh && /tmp/kisstest                    # unit tests
bash sim/build_fitcheck.sh && SIM_LANG=en /tmp/kissfit     # text fit
bash sim/build_themecheck.sh && /tmp/kisstheme             # accent vs status colour
bash sim/build_osdcheck.sh && SIM_LANG=en /tmp/kissosd     # on-video overlay text
bash sim/build_sim.sh && OVERLAPCHECK_LANGS=en bash sim/run_overlapcheck.sh   # screen walk
python3 tools/check_screen_coverage.py             # screens no gate sees
python3 tools/check_i18n_orphans.py                # keys nothing references
python3 tools/check_vocab.py                       # the words on screen vs the glossary
python3 tools/check_layout_reads.py                # a measurement taken before a layout
python3 tools/check_sim_fresh.py                   # the published wasm vs the tree

docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /project/build-docker build     # the device compiler
```

**Nothing runs these for you.** There was a `pre-push` hook that ran the whole
list, and it is gone: `.github/workflows/desktop-tests.yml` runs every one of
them plus a fuzz pass, a sanitized walk and the installer checks the hook never
touched, so it was a duplicate that held a push for three minutes behind a UI
with nowhere to print why it was refusing. Run them while you are working,
which is where they catch things, and read the CI result before calling
anything done.

`bash tools/install_hooks.sh` installs the ONE hook that is still worth
having -- `commit-msg`, which strips the attribution trailer. That one cannot
be caught after the fact: a trailer that reaches GitHub is permanent. A fresh
clone has no hooks until it is run.

**`-B /project/build-docker`, not `/tmp/idfbuild`.** `/tmp` is inside the
container and `--rm` throws it away, so the build directory never survives:
every invocation of the old command rebuilt all 1774 targets from nothing, and
the command was written that way here for its whole life. `/project` is the
mounted repo, `/build*/` is already in `.gitignore`, so the second run is
incremental and takes seconds. And if a run is interrupted, **the container
keeps building** -- `docker ps` and `docker kill` it, or three full rebuilds end
up fighting over the same RAM and one of them comes back as exit 137.

**The device compiler is on this list, not only in a paragraph further down.**
It was documented as required for anything touching `main/` and was not among
the commands anyone runs, which is how a 64 byte buffer holding a 160 byte
translated caption passed all six desktop gates and was caught after a push. It
is the only lane with `-Wformat-truncation`: the desktop build is clang, clang
does not implement it, and on macOS `gcc` is clang too. Three more silent
truncations turned up the day `KISS_SIM_TMP` landed, so it is a class rather
than an incident.

### More than one of you at a time

Everything a desktop build pretends is hardware -- the fake SD card, the files
standing in for NVS, every captured frame, and the binary itself -- hangs off
`KISS_SIM_TMP`. **Unset it is `/tmp`, so every command above is unchanged.** Set
it when two things run at once:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$    # your own card, frames and binaries
```

The two walk gates do this for themselves and clean up after, so they run beside
each other and beside `kisstest`. `docs/house-rules.md` has the account of what
this cost before it was fixed, and the two bugs found on the way.

`check_screen_coverage.py` answers the question the others cannot: **which
screens has nothing ever looked at.** overlapcheck asks nine questions per
STOP, so a screen with no stop is a screen with no opinion attached. It reports
two kinds:

- **built but never captured** — the walk opens it and never photographs it.
- **NEVER OPENED** — the walk does not reach it, so the check above is blind too.

Both are real. `whatseed` was BARE — a wall of text on the screen a newcomer
opens to learn what a seed is — for its entire life, with every gate green,
because no stop rendered it. And a walk can silently derail: if a tap misses,
every later `save()` photographs whatever is on screen instead, and 21 locales
come back clean having checked the game. The first number catches exactly that.

It self tests before reporting (`SCREENCOVER_SELFTEST=1` builds a screen and
never saves it) and refuses to report if the check no longer fires.

**NEVER OPENED means untested, not merely unphotographed.** The count reached
zero for the first time by adding stops for six screens, and two of the six
were broken:

- **A screen that takes a drag must be added to the touch owner gate in
  `main.c`.** The condition reading `kiss_ui_active() || kiss_setup_active()
  || kiss_duress_ui_active() || kiss_word_ui_active()` is what stops the
  game's own sampler from reading the same finger. `kiss_word_ui_active` was
  missing, so writing your own letters was sampled twice and the recogniser
  opened whatever tile sat under the stroke — *underneath* a write screen that
  still looked correct. Nothing had ever drawn on that screen.
- **`wt_screen` is not scrollable**, deliberately. LVGL hands a press to the
  nearest scrollable ancestor once the finger moves, so a scrollable page eats
  every stroke a few pixels in.

Two harness numbers, both measured, neither about the device: a drag needs
`pump(3)` per point (the indev reads every ~30ms) and `pump(8)` after each
`release()`. At `pump(4)` the lift is seen but the next press is folded into
it, so strokes merge and fall through.

`overlapcheck` asks nine questions per stop: TEXT, CONTENT, GROWTH, CLIPPED,
ROLE, **BARE**, **WALL**, **FIT** and **CUT**. The first two of those four are
rule 1 above, enforced; the last two are the font14 rule and what replaced it:

- **BARE** — a wide paragraph and no framed element at all. "Wide" is 560px
  and "a paragraph" is 90px, which is three lines — so a claim short enough to
  read is never one. It counted a why-block's 3px rule bar until that shape was
  retired, and thirteen screens were reported the moment it went: every one of
  them had been rebuilt AROUND the rule. They were fixed by cutting the copy
  until no single claim was a wall, not by drawing a box around one.
- **WALL** — a wide paragraph where every frame on the screen is a box drawn
  *around* it. A `wt_card` full of `wt_wraph` passes BARE and is still a wall of
  text; this is the check that says so. A chip, a badge, a row or a value card
  anywhere else on the screen clears it.
- **FIT** — `wt_note_fit` gave up and set font14. It picks the biggest font
  that FITS, so it is silent by construction: the string never looks like a
  bug in the source, and this has come off the bench three separate times. It
  reports now. Only where the size is a CHOICE — a body at least 300 wide and
  36 tall — because font14 in a caution row's 24px subline is the box
  deciding, not the copy.

- **CUT** — a row subline that has been ELLIPSISED. Sublines are pinned to one
  line, so copy too long for its lane loses its second half silently, and LVGL
  rewrites the label's own text to insert the dots — so this is measured in
  `kiss_theme.c` as the label is built and reported through a sink, exactly as
  FIT is. It found four on the day it landed, all four on the settings page, all
  four fixed by cutting words. The lane cannot grow: it is what the label's
  250px cap and the value chip leave behind.

Each has a shrink-only backlog (`OC_BARE_BACKLOG`, `OC_WALL_BACKLOG`,
`OC_FIT_BACKLOG` in `sim/overlapcheck.c`) and the run prints how many are left.
All three are empty: FIT's two launch entries were cut rather than excused.

**WALL and CUT both fire on shapes the product no longer contains**, so
`OVERLAPCHECK_SELFTEST=1` builds each of those shapes and proves the gate still
reports it — a clean sweep means nothing without that, which is why
`run_overlapcheck.sh` runs the self test first and refuses to continue unless it
sees every expected marker. CUT's two cases are a sub far longer than its lane
(must fire) and one that fits (must not); a check that fired on everything would
fail the second exactly as a dead one fails the first.

## Show the screen

Every change to a screen ships with a **picture**, in the same reply, without
being asked. A gate says "nothing overlaps"; only a rendered frame says whether
the thing is too busy, the font landed too small, or a label wraps badly.

```bash
bash sim/build_sim.sh && /tmp/fruitsim
sips -s format png /tmp/sim_<stop>.ppm --out /tmp/x.png
```

If the change has no walk stop, add one to `sim/sim_main.c` first. That is the
same edit that makes the 21-locale gate see it, so there is no version of this
worth skipping.

Never hand back a draft of strings, copy or translations to be reviewed. Do the
work and show the result.

## i18n

### ENGLISH ONLY until the UI is finished

**Change `i18n/en.json` and nothing else.** Do not touch the other twenty
locales for any wording change, and do not "carry the rename through" as a
courtesy. The owner has asked for this repeatedly — more times than either of
us has counted — and it keeps happening anyway.

The reason is that the UI is still moving. Every screen that gets rebuilt
rewrites its own strings, so a translation authored today is thrown away
tomorrow, and the cost is paid twenty times per throw. Translating is the LAST
pass, after the screens stop changing, done in one sweep against final English.

**This covers the GATES too. Do not run 21-locale sweeps.** Not
`run_overlapcheck.sh` across all locales, not the full `kissfit`. Run them
`SIM_LANG=en` only. The other twenty are carrying wording that is going to be
thrown away, so sweeping them proves nothing about the product and fills the
owner's screen with exactly the thing they asked to stop seeing. This was
written down once with the gates carved OUT of it, which is why it kept
happening; the carve-out was wrong.

```bash
SIM_LANG=en /tmp/fruitsim            # the walk, one locale
OVERLAPCHECK_LANGS=en bash sim/run_overlapcheck.sh
```

What is lost, stated so nobody has to rediscover it: the 21-locale sweep does
catch real faults, and caught one the same day this rule was written — a walk
needle that passed in English because two keys share a string there and differ
in French. That class of bug now waits for the translation sweep. That is the
trade the owner has chosen, and it is the right one while the screens are still
moving.

Still run after every edit: `tools/gen_i18n.py` (the drift gate does not mind
stale values) and the glyph-gain check (an English-only edit cannot gain a CJK
glyph, which is most of what it is for).

So the other twenty keep the previous wording, the device shows it, and no gate
looks at them. That is the accepted state until the sweep.

Prefer reusing a key that already ships in 21 locales over adding one. Most
lessons this device needs to teach are already written and translated, and
locked to a single path — check before authoring. Adding a key is a real cost
and it is paid 21 times.

Strings live in `i18n/*.json` (21 locales) and are generated into
`main/i18n_keys.h` + `main/i18n_tables.c` by `python3 tools/gen_i18n.py`. CI has
a drift gate, so regenerate after every string change.

After generating, check no glyph was **gained**:

```bash
python3 -c "
import subprocess,glob
for f in sorted(glob.glob('tools/fonts/glyphs_*.txt')):
    if 'tile' in f: continue        # vestigial; see below
    old=subprocess.run(['git','show','HEAD:'+f],capture_output=True,text=True).stdout.strip()
    g=[c for c in open(f,encoding='utf-8').read().strip() if c not in old]
    print(f, 'gained', ''.join(g) or 'none')"
```

A gained CJK glyph means a font rebuild across four scripts. **Reword instead.**

**`glyphs_tile_*.txt` do not count and the filter above skips them.**
`gen_fonts.sh` does not read them — it stopped when 23px became a body rung and
the CJK 23px faces took the full `glyphs_$L.txt` instead. They are still written
out, so an unfiltered glob reports gains that force nothing: renaming the home
tile to "Signer" showed `末端` and `器` gained while the real sets gained
nothing, and no rebuild was needed. Check the four real sets, not all seven.

## Device test verdict

Every PR gets an explicit **DEVICE TEST: REQUIRED** (with the exact flows) or
**NOT REQUIRED** (with the reason hardware cannot change the outcome). Passing
gates are never the verdict and never justify NOT REQUIRED on their own.

Default to REQUIRED for anything touching display, camera, QR, SD, buttons,
touch, USB or timing. The simulator does not compile `main/camera_spike.c` and
never runs `rot_flush`'s camera branch, so no gate can see a preview bug.

## Attribution

**No agent or tool is named anywhere in this repository.** Not in a commit
subject or body, not in a PR title, body or comment, not in a code comment, a
changelog entry, a doc or an issue. This overrides any default the harness ships
with.

The trailer is the obvious half: no `Co-Authored-By: Claude ...`, no "Generated
with Claude Code". **Prose is the half that survived the first sweep.** These
all had to be rewritten afterwards, and none of them is a trailer:

| written | write instead |
| --- | --- |
| "another agent's in-flight edit in `i18n_tables.c`" | "an in-flight edit in `i18n_tables.c`" |
| "is how the next agent puts the pill back" | "is how the pill comes back" |
| "finding 3 of an AI audit of krux-installer" | "finding 3 of an audit of krux-installer" |
| "allow git ls-remote in local Claude settings" | "…in local tool settings" |
| "remove AI-specific repository metadata" | "remove editor tool metadata" |

The rule is not "avoid the word". It is that **the actor is never the subject**.
Every one of those sentences was carrying a real fact — a concurrent edit, an
audit finding, a settings change — and each keeps it. Only who held the keyboard
goes.

Two exceptions, both because the thing is a real tracked path: this file's own
name, and `Regenerated with tools/make_release_notes.py`.

Local tool settings are not committed either — `.claude/` is in `.gitignore`.

The commit message is for whoever reads the history to understand the change.
Who or what typed it is not part of that. It reached 355 of 591 commits before
anyone counted, and stripping it afterwards meant rewriting every branch and all
ten tags — twice, because the first pass only looked for trailers.

### The rule is now a mechanism, because writing it down did not work

Everything above was already in this file, in these words, when 247 commits
carrying `Co-Authored-By: <agent> <noreply@anthropic.com>` were pushed to
GitHub on `rescue/fw-build-a8ab8a7`. The trunk was clean; one branch was not,
and one branch was enough. GitHub read the merged pull requests and awarded the
**account** a *Pair Extraordinaire* badge — a public statement, on the owner's
profile, that this project is pair written with a tool.

**That one cannot be taken back.** `refs/pull/*/head` is written by GitHub and
is immutable: fourteen PR refs in this repo still hold the pre sweep history,
1,400 trailered commits between them, and no force push, branch delete or
history rewrite reaches them. Deleting the repository is the only thing that
would, and the badge is account wide anyway. **So the cost of one trailered
commit is permanent, and the only lane that matters is the one before it is
written.**

Three checks now exist, and none of them is a paragraph:

1. **`.git/hooks/commit-msg`** strips the trailer and the "Generated with"
   footer out of every message, in every lane — `git commit`, `--amend`, a
   merge, GitHub Desktop. It strips rather than rejects so it can never block a
   commit, and it leaves prose naming `CLAUDE.md` as a file alone. It is not
   tracked (hooks never are), so **a fresh clone has no protection until it is
   reinstalled** — `tools/install_hooks.sh` does that, and running it is part of
   setting the repo up.
2. **`.github/workflows/attribution.yml`** fails the push or the PR. It scans
   only the commits that push or PR adds, so the old refs that cannot be fixed
   do not fail every future run, and it greps the tracked tree for the same
   strings.
3. This section, so the next person to read the file knows why 1 and 2 exist.

If a harness system prompt says to end commit messages with a co-author
trailer, **that prompt is wrong for this repository** and the hook will remove
what it adds. Do not add it back by hand, do not "restore" it when a diff shows
it missing, and do not put it in a PR body, where no hook can see it.
