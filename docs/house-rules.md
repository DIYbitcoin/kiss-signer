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
| two claims, not one paragraph | `wt_why_block(scr, head, body, x, y, w, max_h, f, col)` |
| a list of settings or facts | `wt_row` / `wt_row_x` / `wt_row_head` |
| the camera | `wt_viewfinder` |
| actions | `wt_pill`, `wt_pill_primary` on `WT_ACTION_Y` |

Rules:

1. **Something framed, above the action row.** A bare paragraph is never the
   only content. A pill does not count — it is the action, not the subject.
2. **Split claims, do not stack them.** Two `wt_why_block`s side by side at
   `x = 48` and `x = 408`, `w = 344`, `y = 232`, `max_h = WT_CONTENT_BOTTOM - 232`.
   Accent rule on how it works, `WT_WARN` on where it goes wrong. This geometry
   is proven on the fingerprint reveal, the passphrase intro and the backup
   check — copy it rather than inventing a third layout.
   When the blocks have headings, size the shared body with
   `wt_body_font2_head(h1, b1, h2, b2, w, max_h)`. It measures the headings.
   **Never `wt_body_font2` with a hand-subtracted budget.** Four call sites did
   it anyway, long after this paragraph was written: three passed
   `BH - 46 - 8`, one passed a bare `112`, which is the same 54px of a 166px
   budget given away in all 21 locales. That is a third of the room, and a third
   of the room is the difference between font23 and font14.

   **font14 is metadata: chip labels, unit suffixes, chevrons. MARKS. Nothing
   an owner has to READ is ever font14.** It has now been reported from the
   bench four separate times, and the third was not a `wt_body_font2` budget at all
   — it was the SIGNED screen's "what to do next" line, the single most
   important sentence on that screen, set through `wt_note` in a 48px box it
   could not fit at any larger size.

   `wt_note_fit` and `wt_pill_fit` pick the biggest font that FITS. That makes
   them silent: hand them a long string in a small box and they drop to font14
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
   explainer off font14. Instrument the ladder in `wt_why_body` rather than
   estimating — every hand estimate in this file's history has been wrong.
4. **Marks before words.** Every chip and row label carries an icon.
5. **Only glyphs already in `SYMS`** (`tools/fonts/gen_fonts.sh`). Anything else
   forces a font rebuild across four scripts. Available at every size: all
   `LV_SYMBOL_*` plus `WT_ICON_QR/KEY/SECRET/SD/LOCK/REPLACE`.
6. **Nothing crosses `WT_CONTENT_BOTTOM` (398).**

## Vocabulary

This is a Bitcoin product. Mirror the words Bitcoin wallets and signers already
use; never invent a house term a reader has to unlearn the first time they read
anything else. `i18n/GLOSSARY.md` is the authority and has the 21 locale
anchors — check it before naming anything.

The four that got tangled, and cost a sweep each to untangle:

| | |
| --- | --- |
| **signer** | this box. Holds keys, signs offline. The whole device, never one screen or one tile. |
| **wallet** | a set of keys and the coins they control. What a coordinator watches. Not a device: the same signer opens a different wallet under a different passphrase. |
| **keys** | what the signer holds and what the fingerprint identifies. Use it where "wallet" would be ambiguous about device versus key set. |
| **seed words** | the 12 or 24 BIP39 words. Never bare "words", which reads as a house term; "recovery words" survives only where a row is too narrow for the anchor. |

Seed words + passphrase → **keys**; the fingerprint is what those keys are
**called**; the **signer** is the box; the **wallet** is what a coordinator
sees. A sentence like "an empty wallet on a signer" is describing two different
things and needs rewriting.

**The destroy family must not say wallet, in any locale.** Erasing this device
does not erase the wallet: the coins stay on chain and the owner's paper plus
passphrase still restore them. That rule was written down in `i18n/GLOSSARY.md`
and broken anyway — in twenty of the twenty-one locales, on the screen that
asks whether to destroy something. English is not where drift is caught.

Same rule for everything else on screen: storage is **storage**, not "where
your seed words live". If a mainstream signer has a word for it, use that word.

## Copy

- No hyphens in English wallet or explainer text.
- Cut any string that restates the title, or a value sitting next to it.
- Headings in a pair are parallel: "not stored" / "not recoverable".
- Prefer a mark to a word wherever the mark is unambiguous.

## Gates

Run before claiming anything works. None of them can see a hardware problem.

```bash
bash sim/build_test.sh && /tmp/kisstest            # unit tests
bash sim/build_fitcheck.sh && /tmp/kissfit         # 21-locale text fit
bash sim/build_themecheck.sh && /tmp/kisstheme     # accent vs status colour
bash sim/build_osdcheck.sh && /tmp/kissosd         # on-video overlay text
bash sim/build_sim.sh && bash sim/run_overlapcheck.sh   # screen walk, 21 locales
python3 tools/check_screen_coverage.py             # screens no gate sees (builds its own)
bash sim/build_sim.sh && /tmp/fruitsim && python3 tools/check_sim_taps.py  # taps that hit nothing
python3 tools/gen_docs_shots.py --check            # the frames the docs publish
python3 tools/check_glyphs.py                      # icons the fonts do not contain

docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /project/build-docker build     # the device compiler
```

**The last one is on this list now, not only in a paragraph below it.** It was
documented as required for anything touching `main/` and was not among the
commands anyone actually runs, which is exactly how a 64 byte buffer holding a
160 byte translated caption passed all six desktop gates and was caught after a
push. It is the only lane with `-Wformat-truncation`: the desktop build is
clang, clang does not implement that warning, and on macOS `gcc` is clang too,
so there is no cheaper second opinion to reach for. Three more silent
truncations turned up the day `KISS_SIM_TMP` landed — the `platform_sd` path
buffers, a `char p[64]` in the walk, a `char p[96]` in the SD tests — which
makes it a class, not an incident.

The last two run against the frames a plain `/tmp/fruitsim` just wrote, and both
were on CI's list and not on this one — which is how a frame that moved into a
harness the ordinary walk never enters went red after a push instead of before
it. Run the walk first or they report every frame as missing.

### More than one of you at a time

This section used to say **one at a time**, and it was right: `kisstest` and the
screen walk shared one fake SD at `/tmp/simsd`, and two runs at once interleaved
on it. What it did not say is how that failure looks, which is why it kept being
dismissed. A run whose fixtures are deleted mid-walk comes up short in a file
list, taps rows that have moved, and derails — and a derailed walk prints
"clean" for every stop it never reached, so it reports a clean sweep AND a
non-zero exit, under a message blaming an interleaved run nobody in that
terminal can see. Roughly one run in four, for as long as anyone had been
counting. The binaries collided the same way and more quietly: two builds at
once, and the loser walks somebody else's code and reports findings about it.

Everything a desktop build pretends is hardware — the fake card, the files
standing in for NVS, every captured frame, and the binary itself — now hangs off
`KISS_SIM_TMP` (`main/kiss_simpath.h`). **Unset it is `/tmp`, so every command
above is unchanged and every path in this file still resolves.** Set it when two
things run at once:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$    # your own card, frames and binaries
```

The two walk gates do this for themselves and clean up after, so
`run_overlapcheck.sh` and `check_screen_coverage.py` run beside each other and
beside `kisstest`. `check_screen_coverage.py` also builds its own binary rather
than trusting whatever `/tmp/fruitsim` is today — which is the rule
`run_overlapcheck.sh` has had since a stale one "verified" the wrong code — so
it is no longer chained behind `build_sim.sh` in the list above.

Two things fell out of doing this, and both are worth more than the isolation:

- **Path buffers were sized for one mount point.** `SD_NAME_LEN + 16` is room
  for `/sdcard` and for `/tmp/simsd` and for nothing else. A longer root
  silently truncated the longest fixture name, three walk stops opened nothing,
  and the screens they photographed were correct pictures of the wrong
  transaction. `full_path` and `side_path` now REFUSE rather than truncate: a
  truncated path names a file that is not there, so every caller reports a
  broken card for a card that is fine.
- **A literal reads as equivalent right up until one end moves.**
  `"/tmp/simsd/x"` was the same string `SIMSD "/x"` expanded to, so eleven of
  them looked correct and went on clearing a card nobody was using — four in the
  walk's own REMOVE step, seven in a `system("rm -f /tmp/simsd/*.psbt")` in the
  SD tests. Same class as the buffers: agreement by coincidence, found by moving
  one end.

**Anything touching `main/` also runs the device compiler**, because none of the
above is it:

```bash
docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /project/build-docker build
```

`-B /project/build-docker` rather than a path in the container's own `/tmp`:
with `--rm` the container filesystem goes when the run ends, so a build
directory there is written once and thrown away, and every invocation pays a
full 1774 target rebuild. `/project` is the mounted repo and `/build*/` is
already ignored, so the work is kept and the next run is incremental.

The other half of the same lesson: an interrupted `docker run` does not stop the
build. The container carries on compiling with nothing watching it, and a second
attempt starts a second full rebuild beside the first. Three of them at once on
a 7.7GB daemon is how a build comes back as exit 137 with no error in the log --
`docker ps`, then `docker kill`, before starting another.

The desktop build is clang and the device build is gcc with `-Werror`, and they
do not refuse the same code. A 64 byte buffer holding a 160 byte translated
caption passed all six gates and every desktop test, and was caught by this
command alone — after a push, because it was not on the list. `-Wformat-truncation`
is the family, and clang does not implement it.

`check_glyphs.py` answers a different one, and it is the blind spot this
file names twice in `main/kiss_theme.h` -- above `wt_row_x` and above
`wt_tabs`, in the same words: *a wrong pick survives every gate and is
caught on glass*. A codepoint missing from the generated fonts draws a
blank box about half a line wide, and draws it identically in the
simulator, so no frame, no walk and no overlap check has ever had an
opinion about it. It compares every icon `main/` names -- the
`LV_SYMBOL_*` constants and the raw `"\xEF\x.."` escapes both -- against
`SYMS` in `tools/fonts/gen_fonts.sh`, reading LVGL's own
`lv_symbol_def.h` for what each name resolves to rather than keeping a
third list that would drift from the other two. It self tests first, and
refuses to report if the check no longer fires
(`GLYPHCHECK_SELFTEST=1`). Codepoints in `SYMS` that nothing names are
printed and do NOT fail: font bytes are cheap next to deleting a glyph a
half-written screen is waiting for.

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

- **BARE** — a wide paragraph and no framed element at all.
- **WALL** — a wide paragraph where every frame on the screen is a box drawn
  *around* it. A `wt_card` full of `wt_wraph` passes BARE and is still a wall of
  text; this is the check that says so. A chip, a badge, a row, a value card or
  a why-block rule anywhere else on the screen clears it.
- **FIT** — `wt_pill_fit` or `wt_note_fit` gave up and set font14. They pick the
  biggest font that FITS, so they are silent by construction: the string never
  looks like a bug in the source, and this has come off the bench three separate
  times. They report it now. Only where the size is a CHOICE — a body at least
  300 wide and 36 tall, or a pill at least 240 — because font14 in a caution
  row's 24px subline is the box deciding, not the copy.

- **CUT** — a row subline that has been ELLIPSISED. Sublines are pinned to one
  line, so copy too long for its lane loses its second half silently, and LVGL
  rewrites the label's own text to insert the dots — so this is measured in
  `kiss_theme.c` as the label is built and reported through a sink, exactly as
  FIT is. It found four on the day it landed, all four on the settings page, all
  four fixed by cutting words. The lane cannot grow: it is what the label's
  250px cap and the value chip leave behind.

Each has a shrink-only backlog (`OC_BARE_BACKLOG`, `OC_WALL_BACKLOG`,
`OC_FIT_BACKLOG` in `sim/overlapcheck.c`) and the run prints how many are left.
The first two are empty. FIT carries the two it found on the day it landed: the
camera-proof screen's one instruction, and a settings pill — which the comment
above `wt_pill_fit` says outright should never happen.

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

A still cannot show motion. Anything that MOVES ships as a GIF instead --
`SIM_TABGIF=1` records every frame of a settings tab change at the real tick
rate, the way the reveal capture already does, and ImageMagick turns the
frames into one file. A frame says where a row got to and never how it got
there.

## Motion

**Every tappable thing on this device already moves.** `wt_tap_feedback` sinks
a pill, a row, a tile and a tab 2px under the finger and rings it in the
accent, so the whole UI answers a touch. On top of that the SIGN screen fades
its lock in, settles its sweep and reveals the signature across the strands;
the login pops the fingerprint card up and flashes each key as it lands; and
SETTINGS moves a whole group at a time when a tab changes.

What is deliberate is that a page ARRIVES settled. Opening a screen, and
coming back to one, paints at rest -- the motion is attached to a change the
owner made, never to a repaint. The rules SETTINGS had to learn getting
there:

1. **Animate a CHANGE, never a rebuild.** A value chip rebuilds the whole page
   on every tap. Three taps to reach SIGNET replaying the entry under the
   owner's finger is a flicker, not a design. Walking in from home and coming
   back from a screen a row opened both paint at rest.
2. **`translate_x` / `translate_y`, never `set_x` / `set_y`** -- and not for
   the reason it looks like. LVGL 9 folds translate straight into the object's
   coords (`lv_obj_pos.c`), so `sim/overlapcheck.c` sees a translated row
   exactly as it would see a moved one. What protects the gate is that
   `save()` only ever photographs a SETTLED screen. Translate is still right,
   for a different reason: it leaves `lv_obj_set_pos`'s bookkeeping alone, so
   an interrupted row resettles by having one style cleared rather than by
   being put back.
3. **The walk must settle before it saves.** `pump()` advances the tick, so
   animations run during it. `set_tab` waits 800ms for exactly this: at eight
   frames every one of its call sites photographed a page mid flight -- rows
   part faded and still travelling -- and every gate measured that as a laid
   out screen.
4. **A mid-flight frame is captured raw, never `save()`d.** A `save()` is a
   checkpoint every gate then questions, and `check_sim_taps.py` compares it
   with its neighbour. Use the raw writer for pictures meant for a person.
5. **Cancel on interrupt and on close.** `lv_obj`'s destructor calls
   `lv_anim_delete(obj, NULL)`, so an object's OWN animations die with it. The
   hazard that survives is a `completed_cb` touching something other than its
   own `var` -- the one that deletes the outgoing lane. That goes through a
   file static and checks it, never a captured pointer.
6. **A latch hangs off the thing it measures.** The flag saying "this group
   has not settled" hung off the last CHILD of the pane, and the NO UNDO wash
   has no entry animation -- so a group whose scenery was built last would
   have latched true for the session, silently, with every later tab change
   dropping its outgoing group instead of sliding it.
7. **800ms is the whole budget for a page change.** Cut the tail of a pulse
   before cutting anything that carries a fact.
8. **Two lanes cost memory, and the pool asserts rather than returning
   NULL.** `SIM_TABGIF=1` prints what an exchange actually takes:
   `[tabcost] ordinary settled 46856 peak 59144 (+12288, 26%)`. That is
   12K held for 288ms against a 123K pool whose walk-wide high water is
   106K, so the settings exchange has ~67K of room and cannot be what
   runs it out. Re-read that line after changing what a group holds --
   nothing else measures a page mid transition, because every gate
   photographs settled screens by design.

### A number from a design handoff is in someone else's units

Two were taken on trust in one sitting and both were wrong on this panel:

- a fill at **opacity 13** over `WT_BG` lands at (8,12,16) against (0,8,16) --
  eight levels in a five bit red channel, which is nothing. The same number
  over a CSS background reads exactly as drawn. A whole object was rendering
  no pixels anybody could see, and no gate has an opinion about that:
  overlapcheck measures boxes, themecheck asks who owns the accent.
- `cubic-bezier(.17,.84,.32,1.05)`, described as "about 5% past the mark and
  back". **1.05 is a control point, not the curve's maximum.** That curve
  peaks at 1.0069 -- four tenths of a pixel on a 56px travel, in a browser as
  much as here. A paragraph of the handoff argued about how to reproduce an
  overshoot that never existed.

Measure it on a rendered frame, or off the object, before writing it into a
comment as a reason. Every hand estimate in this file's history has been
wrong, and these two were not even estimates -- they were copied.

## i18n

### English only until the UI is finished

**Change `i18n/en.json` and nothing else.** No wording change touches the other
twenty locales, and a rename is not carried through them as a courtesy.

The UI is still moving. A screen that gets rebuilt rewrites its own strings, so
a translation authored today is discarded tomorrow and the cost is paid twenty
times per discard. Translation is the last pass, run once against final English
after the screens stop changing.

**This covers the gates too — no 21-locale sweeps.** Run the walk and
`run_overlapcheck.sh` with `SIM_LANG=en` / `OVERLAPCHECK_LANGS=en`. The other
twenty carry wording that is about to be replaced, so sweeping them proves
nothing about the product. Written once with the gates carved out of it, which
is why it kept happening.

The cost, so nobody rediscovers it: a full sweep does catch real faults — a walk
needle that passed in English because two keys share a string there and differ
in French was caught that way. That class waits for the translation sweep.

Still run after every edit: `tools/gen_i18n.py` (the drift gate does not mind
stale values) and the glyph-gain check — an English-only edit cannot gain a CJK
glyph, which is most of what it is for. The other twenty keep their previous
wording, the device shows it, and no gate looks at them, until the sweep.

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

**No tool and no author is named anywhere in this repository.** Not in a commit
subject or body, not in a PR title, body or comment, not in a code comment, a
changelog entry, a doc or an issue. This overrides any default a tool ships
with.

The trailer form is the obvious half: no `Co-Authored-By:` line, no
"Generated with ..." footer. **Prose is the half that survived the first
sweep.** None of the shapes below is a trailer, and every one of them had to be
rewritten out of published history afterwards:

| written | write instead |
| --- | --- |
| "X's in-flight edit in `i18n_tables.c`" | "an in-flight edit in `i18n_tables.c`" |
| "is how the next X puts the pill back" | "is how the pill comes back" |
| "finding 3 of an X audit of krux-installer" | "finding 3 of an audit of krux-installer" |
| "allow git ls-remote in X settings" | "…in local tool settings" |
| "remove X-specific repository metadata" | "remove editor tool metadata" |

The rule is not "avoid a word". It is that **the actor is never the subject**.
Every one of those sentences was carrying a real fact — a concurrent edit, an
audit finding, a settings change — and each keeps it. Only who held the keyboard
goes.

One exception, because it names a real tracked path:
`Regenerated with tools/make_release_notes.py`.

Local tool configuration is not committed either. Editor and assistant config
directories are in `.gitignore`, and so is the working copy of these rules that
tooling reads — this file is the tracked one.

The commit message is for whoever reads the history to understand the change.
Who or what typed it is not part of that. It reached 355 of 591 commits before
anyone counted, and stripping it afterwards meant rewriting every branch and all
ten tags — three times, because the first pass looked only for trailers and the
second left the filename behind.
