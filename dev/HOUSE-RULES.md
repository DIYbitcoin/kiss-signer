# KISS Signer — the long form of two things the working rules cite

*For people working ON this signer. `docs/` is the owner's site; this is not
owner material and used to sit there anyway.*

**The working rules are not tracked here.** Replies, screen chrome,
vocabulary, copy, the gate list, showing the screen, i18n, the device test
verdict and attribution live with whoever is at the keyboard, and this file
never keeps a second copy of any of them — the copy went stale and sent people
to `wt_why_block`, which was deleted.

What is left here is the long form of two things those rules cite: what
running two desktop builds at once used to cost, and how motion works.
`tools/preflight.sh` is the runnable half of the gate list, and it is the one
to read first.

## More than one of you at a time

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
in the gate list is unchanged and every path in this file still resolves.** Set
it when two things run at once:

```bash
export KISS_SIM_TMP=/tmp/kiss-$$    # your own card, frames and binaries
```

The two walk gates do this for themselves and clean up after, so
`run_overlapcheck.sh` and `check_screen_coverage.py` run beside each other and
beside `kisstest`. `check_screen_coverage.py` also builds its own binary rather
than trusting whatever `/tmp/fruitsim` is today — which is the rule
`run_overlapcheck.sh` has had since a stale one "verified" the wrong code — so
it is no longer chained behind `build_sim.sh` in the gate list.

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

**Anything touching `main/` also runs the device compiler**, because nothing
here is it:

```bash
docker run --rm -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory \
  -e GIT_CONFIG_VALUE_0=/project -v "$PWD":/project -w /project \
  espressif/idf:v6.1 idf.py -B /project/build-docker build
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

So that command is where the lint lives, and there is no second one:
`main/CMakeLists.txt` hands this component fifteen more warnings -- `-Wshadow`,
`-Wnull-dereference`, `-Wmissing-prototypes` and the cheap ones around them --
PRIVATE, so the vendored components never see them. The rejected list in that
comment is the longer half and the useful one: `-Wundef` is 1022 hits inside
IDF headers, `-Wformat-truncation=2` is 28 deliberate snprintf folds, and
level 1 is already on and is the one that catches the accident above.

The first thing `-Wshadow` found had shipped: `game_tick` declared its own
`s_sd_badge_live` over the file scope flag, so `sd_badge_sync()` set one and
the breathe read the other. The SD badge had never breathed, and it renders
identically in the simulator.

Two things that are NOT that command:

```bash
KISS_WERROR=1 bash sim/build_sim.sh    # warnings are errors on the desktop too
clang --analyze -Imain -Isim -Icomponents/cUR/src main/kiss_psbt.c
```

The first is what `tools/preflight.sh` and the CI job set for every build they
run; a build you start by hand still only warns, because the same invocation
compiles 463 vendored LVGL sources and an upstream clang update must not be
able to stop your work. The second is the host analyzer, which is not a gate
and is worth twenty seconds now and then -- it reads paths rather than lines,
and the last pass over the crypto sources returned exactly one thing.

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

### The other half of that, which nobody had: the SOURCE

`KISS_SIM_TMP` stops two runs sharing a fake card. Nothing stops two people
sharing the checkout, and that is worse, because the failure is silent about
where it came from.

Measured, and misdiagnosed on the spot. Two runs of
`tools/check_screen_coverage.py` 29 seconds apart: **exit 0 with no failures,
then exit 1 with twelve** — `tap "DEVICE"`, then `Legacy`, `Nested SegWit`,
`Address types`, `network never reached 0`. Same commit either side. It was
reported to the owner as a flaky gate that would redden a green branch at
random.

It was nothing of the kind. Somebody else was editing the same tree: SETTINGS >
BACKUP had gained a third row, and the walk's `def_go(n, i)` call sites had not
been bumped yet. Both facts landed together in one commit six minutes later, so
the broken state existed **only in the working tree** and no commit ever held
it.

Two things make this class expensive, and both are worth knowing on their own:

- **`def_go(n, i)` takes the ROW COUNT**, because `wt_def_h_closed` is `284/n`.
  A tab going from two rows to three moves every `def_go(2, 1)` onto the new
  third row. Seven call sites needed the bump.
- **The tap does not miss.** `def_go(2, 1)` on a three row page computes a `y`
  inside the third row, so it hits a real control and opens a real page that
  renders and photographs perfectly. Every `save()` after it captures the wrong
  screen, and the walk fails at whatever needle it reaches — which is why the
  failures MOVE between runs and why not one of them named the page that was
  actually missed. Four rows still fit (`WT_WIDE_Y` bottoms at 384 against
  `WT_CONTENT_BOTTOM` 398), so the next row added gets the same trap with no
  layout complaint to warn anybody.

So `sim/sim_tmp.sh` stamps every sim build with the commit it came from and how
many files under `main/`, `sim/` and `i18n/` were uncommitted at the time. It
cannot prevent any of the above. What it does is stop the failure being
attributed to the reader's own diff, which is where the entire cost was — the
diagnosis, not the bug. `docs/` is excluded deliberately: it is regenerated
constantly and compiled into nothing, so counting it would print "dirty" on
every run and mean nothing by the second day.

**Before blaming a walk failure on your diff, read that line.** If it says
uncommitted files, ask who is in the tree before reading anything else. The
house rule about running the same binary three times catches shared SCRATCH
state, where the failures move; this one moves too, and running it three times
tells you nothing except that somebody is still typing.

## Motion

**Every tappable thing on this device already moves.** `wt_tap_feedback` sinks
a control, a row, a tile and a tab 2px under the finger and rings it in the
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

