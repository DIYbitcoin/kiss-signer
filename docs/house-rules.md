# KISS Signer — working rules

## Replies

Short. A few sentences, and the picture when a screen changed. No summaries of
what was just done, no lists of what is left, no restating the commit message
back — it is all in the commit and the code. Detail only when asked for it.

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

   **font14 is metadata. If a body is at font14, that is a BUG, not a
   translation being long.** It was reported from the bench as "WHY IS THE TEXT
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
bash sim/build_sim.sh && python3 tools/check_screen_coverage.py  # screens no gate sees
bash sim/build_sim.sh && /tmp/fruitsim && python3 tools/check_sim_taps.py  # taps that hit nothing
python3 tools/gen_docs_shots.py --check            # the frames the docs publish
```

The last two run against the frames a plain `/tmp/fruitsim` just wrote, and both
were on CI's list and not on this one — which is how a frame that moved into a
harness the ordinary walk never enters went red after a push instead of before
it. Run the walk first or they report every frame as missing.

**One at a time.** `kisstest` and the screen walk share one fake SD at
`/tmp/simsd`, so two of these running at once interleave on it and the walk
comes back with a failure that is not in the code. A backgrounded sweep beside a
foreground run is enough to do it. Re-run alone before believing any of them.

**Anything touching `main/` also runs the device compiler**, because none of the
above is it:

```bash
docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.0.1 idf.py -B /tmp/idfbuild build
```

The desktop build is clang and the device build is gcc with `-Werror`, and they
do not refuse the same code. A 64 byte buffer holding a 160 byte translated
caption passed all six gates and every desktop test, and was caught by this
command alone — after a push, because it was not on the list. `-Wformat-truncation`
is the family, and clang does not implement it.

`check_screen_coverage.py` answers the question the others cannot: **which
screens has nothing ever looked at.** overlapcheck asks seven questions per
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

`overlapcheck` asks seven questions per stop: TEXT, CONTENT, GROWTH, CLIPPED,
ROLE, **BARE** and **WALL**. Both of the last two are rule 1 above, enforced:

- **BARE** — a wide paragraph and no framed element at all.
- **WALL** — a wide paragraph where every frame on the screen is a box drawn
  *around* it. A `wt_card` full of `wt_wraph` passes BARE and is still a wall of
  text; this is the check that says so. A chip, a badge, a row, a value card or
  a why-block rule anywhere else on the screen clears it.

Each has a shrink-only backlog (`OC_BARE_BACKLOG`, `OC_WALL_BACKLOG` in
`sim/overlapcheck.c`) and the run prints how many are left. Both are empty
today. WALL fires on a shape the product no longer contains, so
`OVERLAPCHECK_SELFTEST=1` builds that shape and proves the gate still reports
it — a clean sweep means nothing without that, which is why `run_overlapcheck.sh`
runs the self test first and refuses to continue if it fails.

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

### English only until the UI is finished

**Change `i18n/en.json` and nothing else.** No wording change touches the other
twenty locales, and a rename is not carried through them as a courtesy.

The UI is still moving. A screen that gets rebuilt rewrites its own strings, so
a translation authored today is discarded tomorrow and the cost is paid twenty
times per discard. Translation is the last pass, run once against final English
after the screens stop changing.

Unaffected: `tools/gen_i18n.py` still runs after every edit (the drift gate does
not mind stale values), `kissfit` still measures all 21 against their own
strings, and the glyph-gain check still runs — an English-only edit cannot gain
a CJK glyph, which is most of what it is for. The other twenty keep their
previous wording and the device shows it, which is the accepted state until the
sweep.

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
