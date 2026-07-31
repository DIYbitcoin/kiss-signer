# The wallet's picture

The master fingerprint, drawn as a 16x22 pattern beside its eight hex
characters. Implemented; this describes what shipped and why the rest did not.

Encoder: [BitSquiggles](https://github.com/maggo83/BitSquiggles), vendored at
`components/bitsquiggle32/` and pinned by commit and sha256 (see that
component's README).

## The one rule

**Beside the hex, never instead of it.** Upstream says it plainly: 32 bits, a
targeted collision is feasible, not a hash and not protection. It catches the
accident — a mistyped passphrase opening a wallet the owner did not mean to
open — and nothing else. A screen that cannot show both gets the hex and no
picture.

`wt_squiggle()` also refuses an all-zero fingerprint, the same rule
`wallet_setup.c` already applies to the hex: a zeroed buffer is a failed
derivation, and a picture is the worse thing to get wrong, because a picture is
trusted at a glance rather than read.

## Why it is worth drawing at all

The passphrase story rests on one sentence: a typo opens a different wallet, no
error, no recovery. That is a **recognition** task, and hex is bad at
recognition. Nobody remembers `EC5A4595`; everybody remembers a shape.

`sim/squigglecheck.c` says the shapes really are distinct: across 19,900 pairs of
random fingerprints, zero identical and zero within 8 of the 352 cells.

## Where it appears

**The fingerprint reveal screen** (`main/wallet_ui.c`, `show_fingerprint`), at
3x — 48x66 px, left of the num48 code inside the existing 420x118 card. This is
where the owner meets the code, so it is where they meet the picture.

The size is measured, not chosen. The caption runs to 363px of the 420 card in
pt-BR, so there is no column a long translation would not spill out of, which is
why the picture sits *under* the caption rather than beside it — and under the
caption there are 78 vertical pixels, which is 3x and not the 4x this was first
sketched at. The card's size, position and pop-in are untouched.

**The fingerprint explainer card** (`main/wallet_info.c`, `fp_model`), at 6x —
96x132 px, under the `words + passphrase -> fingerprint` equation, so the arrow
points at the thing instead of at a label. This is the size worth learning.

It appears there **only when the card's title carries the code**, which is the
home-chip path (`DIAG_FP_PIC`). Opened from the WALLET screen's "?" the title is
the bare word, so that path stays `DIAG_FP` and draws nothing — the one thing
this must never be is a picture with no number beside it.

## Where it deliberately does not appear

- **The write-it-down screen** (`wallet_setup.c`). The only screen that tells the
  owner to copy the fingerprint onto paper, and a picture cannot be copied onto
  paper. It would dilute the one instruction that screen exists to give.
- **The spare-wallet explainer** (`wallet_duress_ui.c`, ST_FUND). Two pictures
  side by side would be the best argument in the product that two passphrases are
  two wallets — but at that stage the spare's passphrase does not exist yet, so
  there is no second fingerprint to draw. Invented ones would be worse than
  prose. Revisit when a real one exists to show.
- **The sign path** (`wallet_sign.c`, SIGNING AS). Hex leads there.
- **The chips** on home, WALLET and settings. Phase 2, below.

## Colour

`wt_accent()`, always. The library derives an OKLCH hue from the value, and that
hue is thrown away: it would fight a palette where accent means identity, amber
means testnet and red means destructive, and it dies on MONO and on colourblind
eyes. `sim/test_squiggle.c` asserts the raster is identical whatever style is
asked for, which is what makes discarding the colours safe.

## Rendering

`wt_squiggle()` in `main/wallet_theme.c` expands the pattern into an `lv_canvas`
itself, one grid cell to one `scale` x `scale` square, in `LV_COLOR_FORMAT_A8`
tinted by `image_recolor`. Integer scales only, and deliberately no
`lv_image_set_scale()`: LVGL 9.5 interpolates, and a smoothed squiggle is a
blurred one, which is the point thrown away.

The buffer is owned here and freed on `LV_EVENT_DELETE`. That is safe only while
the image cache is off (`LV_CACHE_DEF_SIZE` is 0 in both `sim/lv_conf.h` and
`sdkconfig`); the note on `squiggle_free_cb` says what would have to change if it
is ever switched on.

## The encoding is frozen

`sim/test_squiggle.c` asserts the full 16x22 raster for four fingerprints,
written out as ASCII pictures so a reviewer can see what is being promised.

This is not a nit. The pattern is something owners learn by sight; if a firmware
update changed it, every owner who had learned their picture would open their own
wallet and be shown the wrong one, with no error and no way to take it back. A
failure there is a breaking change to every wallet already in the world, not a
vector in need of re-blessing.

## Phase 2, not done

The 2x mark in the fingerprint chip on home, WALLET, settings and SIGNING AS.
Home would need its chip frame to go from `(566,40) 195x47` to `(526,40) 235x47`,
mark at inset 12, hex keeping montserrat_28.

The open question is entirely physical: at 2x the finest feature on the glass is
two pixels. `sim/squigglecheck.c` prints that number and refuses to draw a
conclusion from it. Answer it on a device before writing any of this.

## Verification

```bash
bash sim/build_test.sh && /tmp/kisstest
bash sim/build_squigglecheck.sh && /tmp/kisssquiggle 200
bash sim/build_fitcheck.sh && /tmp/kissfit
bash sim/build_overlapcheck.sh && /tmp/kissoverlap
bash sim/build_themecheck.sh && /tmp/kisstheme
```

None of them can answer whether a 3x pattern reads on the panel at arm's length.
That needs hardware.
