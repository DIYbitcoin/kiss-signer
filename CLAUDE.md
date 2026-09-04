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

### A defect seen in a frame is a hypothesis

**Check `docs/decisions.md` for the screen before filing one, and read the
builder's comment if it is listed. If the comment answers it, the finding is
retracted, not argued.**

Seven findings were filed and withdrawn in a single review pass, and every one
of them was already answered in a comment a few lines above the code that
drew it: the KEYS tab strip, the dice fills, the RECEIVE caption picker, the
storage chooser's pairing, the attention dots' routing, the band's language and
theme controls, and the stop tab. The screens were right and the reasoning was
invisible, so every reviewer re-derived the same wrong conclusions and every
reply was spent refuting them.

The comment beside the code stays the source of truth -- a second hand written
copy goes stale the first time one changes. `docs/decisions.md` is GENERATED
from `// DECIDED:` markers by `tools/gen_decisions.py`, with `file:line` links,
and a drift gate in `desktop-tests.yml` keeps it honest. Mark **reversals
only**: X was tried, it was wrong, Y is why. A marker on every interesting
comment produces a document nobody reads, which is the same as not having one.

The rule points at the generated page and not at the comment on purpose. A
reviewer holding a frame has no idea which file drew it, which is exactly how
all seven happened.

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

### BITCOIN simple, not simple simple

**The plain English paraphrase of a Bitcoin word is not the simple version of
it. It is a house term with extra steps, and it is worse than the word.** The
owner has said this more times than either of us has counted, shouting the last
one, and it keeps happening because the Copy rules below read like a licence for
it — "no four syllable word", "said the way somebody would say it out loud".
They are not. They are about SENTENCES. A NAME is settled by the Vocabulary rule
above, and the answer is whatever Sparrow, Nunchuk and mempool.space already
call it.

The sign screen headed its output column **"WHERE IT GOES"**. Three plain words,
no jargon, and every one of them wrong: the column beside it says **INPUTS**,
the glossary one tap away teaches **OUTPUTS**, `GLOSS_ICONS[1]` is the OUTPUTS
mark, and the DETAILS deck's own tab is called OUTPUTS. So the screen taught a
paraphrase in the one place its own pair was already on the glass, and the
reader had to learn the real word somewhere else anyway. It says OUTPUTS, from
`gloss_term(1)` — the glossary's own line, already translated 21 times, no key.

**"DESTINATIONS" was the next thing reached for and is the same mistake.** It is
not what a coordinator calls them either, and it is not more honest for the fee
row or the change row than the word that actually covers all three.

The test, before inventing anything: **what does the coordinator on the owner's
laptop call this?** If it has a name there, that is the name. Reach for plain
words for the SENTENCE around it, never for the name itself — the Copy rule
already says so in its own words, *"the name in full, every time; when the line
will not fit, cut a different word, never the name"*, and that rule is not only
about seed words.

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

**All of it in one command: `bash tools/preflight.sh`.** The block below is
the list; that script is the list plus the four things only CI ever ran, and
it prints a table rather than stopping at the first failure. It exists because
`sim/check_sd_psbts.sh` broke on an include, was invisible to everyone working
by hand, and sat red across THIRTEEN consecutive pushes.

```bash
bash tools/preflight.sh                                    # all of the below

bash sim/build_test.sh && /tmp/kisstest                    # unit tests
bash sim/build_fitcheck.sh && FITCHECK_SELFTEST=1 SIM_LANG=en /tmp/kissfit  # text fit
bash sim/build_themecheck.sh && /tmp/kisstheme             # accent vs status colour
bash sim/build_osdcheck.sh && SIM_LANG=en /tmp/kissosd     # on-video overlay text
bash sim/build_sim.sh && OVERLAPCHECK_LANGS=en bash sim/run_overlapcheck.sh   # screen walk
python3 tools/check_screen_coverage.py             # screens no gate sees
python3 tools/check_i18n_orphans.py                # keys nothing references
python3 tools/check_vocab.py                       # the words on screen and in the docs
python3 tools/check_stop_reasons.py                # a refusal with no words in 21 locales
GLYPHCHECK_SELFTEST=1 python3 tools/check_glyphs.py  # an icon with no glyph in the fonts
python3 tools/check_mono_glyphs.py                 # the same, for the mono faces
python3 tools/check_text_glyphs.py                 # a STRING with no glyph, in any locale
GATECHECK_SELFTEST=1 python3 tools/check_gates.py  # a checker nothing runs
python3 tools/check_layout_reads.py                # a measurement taken before a layout
python3 tools/check_lv_conf.py                     # the sim's LVGL config vs the device's
python3 tools/check_preflight.py                   # a CI step preflight.sh does not run
python3 tools/check_sim_fresh.py                   # the published wasm vs the tree
python3 tools/check_docs_fresh.py                  # how far the pictures trail the screens

docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /project/build-docker build     # the device compiler
```

**That command is also the lint lane.** `main/CMakeLists.txt` passes fifteen
more warnings to this component and IDF already passes `-Werror`, so a hit is
a build failure rather than a line to read. The comment beside the block says
what each one found and, longer, what was rejected and why -- read that before
adding one. There is no lint lane on the desktop side and cannot be a useful
one: clang answers `-Wformat-truncation` and `-Wstringop-truncation` with
"unknown warning option" and compiles on.

What the desktop lane does have is `KISS_WERROR=1`, which turns every
`sim/build_*.sh` warning into an error. `tools/preflight.sh` and the CI job
set it; the commands above do not, so a build you run by hand still finishes.

`clang --analyze` is worth an occasional pass and is not a gate -- it ships
with the host clang, needs nothing installed, and takes about twenty seconds
over the crypto sources. HOUSE-RULES.md has the invocation.

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

**`build-release/` is not that directory and `idf.py` must never be pointed at
it.** It belongs to `tools/build_release.sh`, which builds with a DIFFERENT
config -- `-DSDKCONFIG=/project/sdkconfig.release -DKISS_RELEASE=1`, WARN level
logs and signed-app verification ON -- and then signs the app on the host. A
plain `idf.py -B build-release build` re-configures it to the default sdkconfig
and relinks over objects compiled under the release one.

The result boots into `abort()` at 2031 ms, every cycle, before `app_main` is
reached. There is nothing in the build output to see: it finished, exit 0, no
warnings, and the version string it reports is correct. The clean container
build of the same commit ran for twelve seconds with zero resets, so the
defect was never in the tree.

**And it was flashed with every part hash verified.** esptool wrote four parts
at the right offsets, read each one back and printed "Hash of data verified",
and the board still could not boot. **A verified flash proves the bytes on the
chip match the file. It proves nothing about whether the file was linked from
a coherent object tree**, which is the failure that actually happens -- so
"esptool verified it" is not evidence a build is sound, and the only thing
that is, is the boot log.

When a release build looks wrong, `rm -rf build-release` and go through
`tools/build_release.sh`. Never incrementally.

**The device compiler is on this list, not only in a paragraph further down.**
It was documented as required for anything touching `main/` and was not among
the commands anyone runs, which is how a 64 byte buffer holding a 160 byte
translated caption passed all six desktop gates and was caught after a push. It
is the only lane with `-Wformat-truncation`: the desktop build is clang, clang
does not implement it, and on macOS `gcc` is clang too. Three more silent
truncations turned up the day `KISS_SIM_TMP` landed, so it is a class rather
than an incident.

**And the two glyph checks are on it for the same reason, one step worse.**
`check_glyphs.py` and `check_mono_glyphs.py` were written, they self test, they
pass -- and until now NOTHING invoked them: not a workflow, not a build script,
not this list. A gate nothing runs is a gate that does not exist, and this one
covers what `kiss_theme.h` names twice in its own words, *a wrong pick survives
every gate and is caught on glass*: a codepoint missing from the generated fonts
draws a blank box about half a line wide, and draws it IDENTICALLY in the
simulator, so no frame, no walk and no overlap check has ever had an opinion
about it. Four other checkers are absent from this list and that is fine --
`check_cur_link.py` runs inside `sim/build_test.sh`, `check_flash_budget.py` and
`check_fw_version.py` inside the release scripts, `check_sim_taps.py` in CI. The
test is not "is it listed", it is "does anything run it" -- and
`check_gates.py` is that question, asked mechanically, so this cannot be found
by hand a third time. It counts CLAUDE.md as a runner on purpose: a command a
person is told to run IS run, and that is the whole lane for the gates the
owner drives by hand between commits.

### More than one of you at a time

Everything a desktop build pretends is hardware -- the fake SD card, the files
standing in for NVS, every captured frame, and the binary itself -- hangs off
`KISS_SIM_TMP`. **Unset it is `/tmp`, so every command above is unchanged.** Set
it when two things run at once:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$    # your own card, frames and binaries
```

The two walk gates do this for themselves and clean up after, so they run beside
each other and beside `kisstest`. `HOUSE-RULES.md` has the account of what
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

**`OVERLAPCHECK_SIZES=1` prints every rendered label with its font**, which is
the only way to ask "show me everything small" — each check is defined by what
it EXCUSES, so a clean sweep says nothing about what the exemptions cover. The
sweep that found the four remaining font14 sentences was
`OVERLAPCHECK_SIZES=1 SIM_LANG=en /tmp/kissoverlap | grep '^\[size\]'`, sorted
by font. 745 labels came back at font14 and 143 were distinct; almost all were
marks, and the four that were not had each been let through by a different
exemption.

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

- **SLACK** — the English string leaves no room for its own translation. A
  Latin translation runs about a third longer; that is the most reliable
  number in this repository, and it is why a sweep whose English was clean
  came back with 143 findings at font14. So SLACK measures the English at the
  FLOOR rung, adds the third every locale gains, and asks whether the lane
  still holds it. If not, that string is font14 in fifteen locales before
  anybody translates it.

  **This is the only check here that can be answered before a translation
  exists**, which is why it is worth more than the twenty locale sweep the
  English-only rule already forbids. It runs `SIM_LANG=en` for the same reason
  READ does: measuring a translation's own slack is meaningless, and the only
  fix on offer — cut the SOURCE copy — is an English edit either way.

  What it found on the day it landed was not a copy problem, and that is the
  point: forty six of the sixty six sat on ONE lane, the 352px value half of a
  `wt_facts` row. That lane learned to wrap in the same commit and all forty
  six left without losing a word. **A one line pinned lane sized to fit
  English exactly is a lane that fails in fifteen languages**, and the fix is
  the lane, not the sentence.

  The twenty one left are mostly the glossary's own anchors — `SEED WORDS`,
  `PASSPHRASE`, `DESCRIPTOR`, `FINGERPRINT` — which the anchor rule already
  covers: where the name IS the string, the lane is what is wrong.

Each has a shrink-only backlog (`OC_BARE_BACKLOG`, `OC_WALL_BACKLOG`,
`OC_FIT_BACKLOG`, `OC_SLACK_BACKLOG` in `sim/overlapcheck.c`) and the run
prints how many are left. The first three are empty: FIT's two launch entries
were cut rather than excused. SLACK's holds the twenty one that outlived the
wrap, as a ratchet — it cannot grow.

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

**And when you do not know WHICH screen you changed, ask.** The walk writes
about 500 frames and nothing compared them across runs -- `check_sim_taps`
compares neighbours inside ONE run, which answers a different question. So an
edit to shared kit moved screens nobody opened.

```bash
python3 tools/contact_sheet.py --update    # this run is the baseline
# ...edit, rebuild, walk again...
python3 tools/contact_sheet.py             # only the frames that moved, as a page
```

One string changed reports one frame out of 513. It is not a gate: it never
fails, excuses nothing and has no backlog, which is why it is not named
`check_*` and why `check_gates.py` does not count it. It exists because every
gate here is defined by what it EXCUSES, and four defects in one session --
a body at font14, an output column half stood down, an accent that survived a
theme change, two flags on one bit -- were invisible to all of them and three
were caught by a person looking at a frame.

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

### Cutting a locale's copy to its lanes

`tools/lane_budget.py <findings-file> <locale>` turns a saved
`run_overlapcheck.sh` into the only thing the work actually needs: one row per
KEY, with the number of characters the copy has to fit in.

    python3 tools/lane_budget.py /tmp/fr.txt fr
    D_WORDS   row label  20ch -> keep 12ch (cut 8, lane 210px)

The gate names a pixel lane and quotes the string as RENDERED -- truncated,
joined across hand-set line breaks, sometimes already wearing an ellipsis. Three
locales were cut by hand before this existed, and each one started with the same
half hour: work out which key each finding is, divide width by length to get
this script's pixels per character, turn the lane into a budget. That is
arithmetic and it is the same arithmetic every time.

The budget is derived per finding from that finding's own width, so it carries
the locale's real glyph width rather than an assumption about Latin or Cyrillic.
Rows it cannot name print `?`: the string is built at runtime, and those are the
only ones worth thinking about.

Two things it does not know, and both have bitten:

- **A shorter string can collide.** `gen_i18n.py` refuses duplicates, so the
  check is free -- but four Russian cuts landed on a string another key already
  had, one of them the tab directly above the row. Read its complaint.
- **An anchor is not cuttable.** `кодовая фраза`, `phrase secrète` and
  `Seed-Wörter` overflow lanes sized for English and are the glossary's own
  terms. The rule is to cut a different word and never the name; where the name
  IS the string, the lane is what is wrong. Leave it and say so.

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

A gained CJK glyph means a font rebuild across three scripts. **Reword
instead.** The check is CJK only, and that is the whole of it: `ja`, `ko`, `zh`.

**`glyphs_tile_*.txt` do not count and the filter above skips them.**
`gen_fonts.sh` does not read them — it stopped when 23px became a body rung and
the CJK 23px faces took the full `glyphs_$L.txt` instead. They are still written
out, so an unfiltered glob reports gains that force nothing: renaming the home
tile to "Signer" showed `末端` and `器` gained while the real sets gained
nothing, and no rebuild was needed.

**There is no `glyphs_lat.txt` any more, and it was the more expensive of the
two.** The tile sets at least announce themselves as a special case; the lat set
sat in the list of REAL ones, so a gain there read as authoritative. It had no
reader anywhere in the repo: `gen_fonts.sh` builds the Latin faces from the
hardcoded `LAT` range list passed as `-r`, and only `cat`s `glyphs_$L.txt`
inside its `for L in ja ko zh` loops. A French string adding `œ` therefore
gained a character in a file nothing consumes, and it was reported as a defect
— "the device draws a blank box" — on top of a `grep` for the literal `0x153`
in the generated fonts, which found nothing and proved nothing, because a dense
`FORMAT0_TINY` range needs no `unicode_list` entry. `œ` is `U+0153`, inside
`0x100-0x17F`, and had been in every Latin face all along.

Two lessons, and only the second is about fonts:

- **A generated file with no consumer is worse than no file.** It cannot fail,
  so it is never wrong, so it is believed. `gen_i18n.py` no longer writes it.
- **Latin coverage is already a hard error, not a diff to read.**
  `lat_covered()` in `tools/gen_i18n.py` checks every string in every Latin
  locale against those same ranges and appends to `errors`, because a label
  whose glyphs are all missing hard-hangs LVGL 9.5. That is the mechanism. The
  glyph-gain check above never was one — it is a prompt to go and look, and it
  only has anything to say about CJK.

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
