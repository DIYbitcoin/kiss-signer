# Handoff 02, the sign bundle graph

Target: `main/kiss_sign.c`, `main/kiss_theme.c`, `i18n/*.json`.
Source of truth for the drawing: `Sign IO Visualization.dc.html`, frames `2c`, `3a`, `3b`, `4a`, `5a`, `5b`.
Everything below is 800x480, the four type rungs (14 / 23 / 34 / 48), and the existing
theme macros. No new colours, no new fonts.

---

## 1. What is new, in one line each

| # | Change | Where |
|---|--------|-------|
| 1 | Verify screen leads with a converging bundle graph, inputs left, junction, outputs right | `kiss_sign.c` verify page |
| 2 | Strand thickness is proportional to value | new widget in `kiss_theme.c` |
| 3 | Long input lists elide the middle, stating hidden count and hidden total | same widget |
| 4 | Signing lights each strand as its input is signed | `kiss_psbt_sign` callback |
| 5 | Fee states its share of the send | existing `S_FEERATE_PCT_FMT` |
| 6 | Details page: one `?` per term, not one for the row | `kiss_sign.c` details page |
| 7 | Glossary becomes eight term rows | `glossary_cb` |
| 8 | `wt_pill` border is the accent, not `WT_MUT` | `kiss_theme.c` |

---

## 2. The bundle widget

New builder, beside the other `wt_*` builders:

```c
typedef struct {
    uint64_t sats;
    bool     signed_ok;     // drawn in wt_accent() once its signature lands
    bool     is_group;      // the elided middle: dashed, holds N coins
    uint16_t group_n;
} wt_strand_t;

lv_obj_t *wt_bundle(lv_obj_t *scr, int x, int y, int w, int h,
                    const wt_strand_t *in,  size_t n_in,
                    const wt_strand_t *out, size_t n_out);
```

Geometry as drawn (frame `2c`), content box `x=24 y=172 w=752 h=118`:

- input labels right-aligned in a 104px lane at `x=24`, mono14
- strands start at `x=144`, junction dot at `x=354, r=5`, `WT_INK`
- output strands end at `x=454`, output labels start `x=464`, mono23
- vertical pitch 47px, three rows fit the 118px box; five rows use pitch 24 (frame `3a`)

**Stroke width from value.** Linear on the largest strand, clamped:

```c
int wt_strand_px(uint64_t sats, uint64_t max_sats)
{
    if (!max_sats) return 2;
    int px = (int)((sats * 11) / max_sats);      // 11px is the widest strand
    return px < 2 ? 2 : px;                       // 2px floor, or dust vanishes
}
```

The floor matters: an 800 sat fee against a 4.2M send is 0px unclamped. Two pixels
is the smallest stroke that still reads as a line on this panel.

**Elision.** Above five inputs, draw first two, one group strand, last two:

```c
// group strand is dashed (LV_STYLE_LINE_DASH_WIDTH 3, GAP 5) so many coins never
// read as one coin, and carries both numbers: "16 more coins" + their total.
```

Label for the group row uses a new string, see section 5. `S_D_MANYIN_FMT`
already covers the details page's version of this and is not changed.

---

## 3. Signing, coin by coin

`kiss_psbt_sign` already walks inputs in order. Add a progress callback so the
graph is a real event log rather than decoration:

```c
typedef void (*wpsbt_sign_cb_t)(size_t idx, size_t total, void *ud);
int kiss_psbt_sign_progress(wpsbt_sign_cb_t cb, void *ud);
```

On each callback: set that strand `signed_ok`, repaint it and its label in
`wt_accent()`, update the header to `S_SIGNING_COIN_FMT`, advance the hold pill's
sweep to `idx/total`. The output side does not move while this runs; it takes the
`LOCKED` eyebrow at `x=660, y=150`, font14, `wt_accent()`.

Timing in frame `4a` is a 6s loop for review only. On device the driver is the
callback, not a timer.

---

## 4. Two changes outside the graph

**`wt_pill` border.** `kiss_theme.c` currently sets `WT_MUT`:

```c
lv_obj_set_style_border_color(p, wt_accent(), 0);   // was WT_MUT
lv_obj_add_flag(p, WT_FLAG_ACCENT);                  // so wt_accent_restyle repaints it
```

The `WT_FLAG_ACCENT` flag is the part not to forget, or the rims go stale when the
accent changes on the settings screen without a rebuild of the page.

Inert pills keep `#2A3346`, which is how "disabled" reads while signing.

**Details page, one `?` per term.** Today the fee row carries a chip whose card
covers fee rate, version, locktime and sighash together. Give each of the four
rows its own chip at the row's right edge, each opening only its own term. Chips
stay circles: `kiss_theme.c` already states the rule, a rectangle does something,
a circle is a thing.

Icons on those rows and in the glossary are already assigned in `GLOSS_ICONS` and
already drawn in `wt_accent()` at `kiss_sign.c:1664`, so nothing is needed for
them but the layout. They render today; the HTML drawing had to redraw them as
vectors only because the review page ships a two-glyph icon subset.

---

## 5. New strings

Six keys, and one edit. All 21 locales.

| Key | English |
|-----|---------|
| `S_BUNDLE_IN_FMT` | `SPENDING %u OF YOUR COINS` |
| `S_BUNDLE_OUT` | `WHERE IT GOES` |
| `S_BUNDLE_MORE_FMT` | `%u more coins` |
| `S_BUNDLE_NOCHANGE_FMT` | `no change, this empties all %u` |
| `S_SIGNING_COIN_FMT` | `SIGNING COIN %u OF %u` |
| `S_ALL_SIGNED_FMT` | `ALL %u COINS SIGNED` |

Edit: `S_TOTAL_LEAVING`, `TOTAL LEAVING WALLET` becomes `TOTAL LEAVING YOUR WALLET`.
The coordinator is watch-only and nothing of value leaves the signer, so the
owner of those coins is the person holding the device.

The glossary needs no new strings. `S_GLOSSARY_B` is already written one
`TERM: definition` per line in every locale, which `wt_split_colon` reads
directly, so the eight-row layout is a layout change only.

---

## 6. Screens that must still work

- **3 inputs, 1 recipient, change** — frame `2c`, the common case
- **20 inputs, 1 recipient, no change** — frame `3a`. Always carries the
  coins-linked bar, because `WPSBT_C_MERGE_INS` fires at 5 inputs, and the hold
  pill is inert until the bar is acknowledged. Third strand states the no-change
  case rather than going missing.
- **A strand tapped** — frame `3b`. Card at `y=300`, `h=80`, holds the coin's
  value, derivation path, previous txid and vout, and its proven mark. The
  address and the whole output side stay on screen.
- **Signing** — frame `4a`.

---

## 7. Accent wiring

Every object in this list takes `WT_FLAG_ACCENT` so `wt_accent_restyle` repaints it
when the accent changes on the settings screen. Miss the flag and the object keeps
the old accent until the page is rebuilt, which is the bug this list exists to stop.

| Object | Property | Source of the colour |
|--------|----------|----------------------|
| page title | text | `wt_accent()`, already done at `kiss_theme.c:509` |
| `wt_pill` rim, enabled | border | `wt_accent()`, this handoff, section 4 |
| `wt_pill` fill, pressed | bg | `wt_accent_pressed()`, already done |
| hold pill rim | border | `wt_accent()` |
| hold pill fill | bg | `wt_accent_bg()` |
| hold pill arc | arc indicator | `wt_accent()`, already done at `kiss_sign.c:1529` |
| signer badge rim | border | `wt_accent()` |
| change strand + its label | line, text | `wt_accent()` |
| a signed strand + its label | line, text | `wt_accent()` |
| `LOCKED` eyebrow | text | `wt_accent()` |
| input proven ticks | text | `wt_accent()` |
| derivation path lines | text | `wt_accent()` |
| glossary + detail row icons | text | `wt_accent()`, already done at `kiss_sign.c:1664` |

Not accent, and must not become accent: `WT_WARN` amber on caution rows and the
caution badge, `WT_STOP` red on the block state and the hold pill's sweep, `WT_MUT`
on captions, `#2A3346` on inert pills. Status colours never move; the accent is
the one that has to.

Inputs on the graph are drawn `WT_MUT` until signed, so "arriving" and "leaving"
stay two families under all four accents, including MONO where the accent is
`WT_INK`.

---

## 8. Coordinate appendix

Every direct child of each frame, as drawn. Taken from the drawing itself, not
retyped. `x`/`y` are the frame's own origin, so they map onto
`lv_obj_set_pos(o, x, y)` with the page's 800x480 as the parent. `rung` is the type
size; the four legal values are 14, 23, 34, 48, plus 11 which appears only on the
review page's own labels and never on device.

### 2c Bundle verify

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIGN |
| 132px | 28px | auto | auto | 14 | `#7A869C` | payout-04.psbt |
| 540px | 14px | 236px | 36px | 14 | `#0A0E15` | SIGNING AS EC5A4595 |
| 0 | 64px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 78px | auto | auto | 14 | `#7A869C` | TOTAL LEAVING YOUR WALLET |
| 24px | 92px | auto | auto | 48 | `` | 61 000 sats 0.00061000 BTC = amount + fee |
| 24px | 150px | auto | auto | 14 | `#7A869C` | SPENDING 3 OF YOUR COINS |
| 464px | 150px | auto | auto | 14 | `#7A869C` | WHERE IT GOES |
| 24px | 172px | auto | auto |  | `` | SVG: `M120,12 C210,12 250,59 330,59` w9<br>`M120,59 C210,59 250,59 330,59` w6<br>`M120,106 C210,106 250,59 330,59` w4<br>`M330,59 C410,59 350,12 430,12` w11<br>`M330,59 C380,59 380,59 430,59` w3<br>`M330,59 C410,59 350,106 430,106` w2<br>junction `cx330 cy59 r5` |
| 24px | 174px | 104px | auto | 14 | `` | 32 000 |
| 24px | 221px | 104px | auto | 14 | `` | 18 400 |
| 24px | 268px | 104px | auto | 14 | `` | 10 600 |
| 464px | 170px | auto | auto | 23 | `` | 60 000 recipient |
| 464px | 220px | auto | auto | 23 | `` | 800 network fee 1.3% |
| 464px | 267px | auto | auto | 23 | `` | 200 change back to you |
| 24px | 300px | auto | auto | 14 | `#7A869C` | recipient address |
| 24px | 318px | auto | auto | 23 | `#4C5666` | bc1qzyg3h8ffkz7mvd3sjn54khce6mua7lqpz9x8gf |
| 0 | 356px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 366px | auto | auto | 14 | `#7A869C` | MAINNET real bitcoin · REPLACEABLE (RBF) you can raise the fee |
| 0 | 390px | 800px | 90px |  | `#0B0E14` | fill |
| 48px | 404px | 310px | 52px | 23 | `var(--accbg,#102417)` | HOLD TO SIGN |
| 56px | 410px | 40px | 40px |  | `5px solid #10141D` | rule |
| 366px | 404px | 150px | 52px | 23 | `#10141D` | DETAILS |
| 672px | 404px | 104px | 52px | 23 | `#10141D` | BACK |

### 2c Bundle during signing

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIGN |
| 132px | 28px | auto | auto | 14 | `#7A869C` | payout-04.psbt |
| 540px | 14px | 236px | 36px | 14 | `#0A0E15` | SIGNING AS EC5A4595 |
| 0 | 64px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 78px | auto | auto | 14 | `#7A869C` | TOTAL LEAVING YOUR WALLET |
| 24px | 92px | auto | auto | 48 | `` | 61 000 sats 0.00061000 BTC = amount + fee |
| 24px | 150px | auto | auto | 14 | `#7A869C` | SIGNING COIN 2 OF 3 |
| 464px | 150px | auto | auto | 14 | `#7A869C` | WHERE IT GOES |
| 660px | 150px | 116px | auto | 14 | `var(--acc,#35D07F)` | LOCKED |
| 24px | 172px | auto | auto |  | `` | SVG: `M120,12 C210,12 250,59 330,59` w9<br>`M120,59 C210,59 250,59 330,59` w6<br>`M120,106 C210,106 250,59 330,59` w4<br>`M330,59 C410,59 350,12 430,12` w11<br>`M330,59 C380,59 380,59 430,59` w3<br>`M330,59 C410,59 350,106 430,106` w2<br>junction `cx330 cy59 r5` |
| 24px | 174px | 104px | auto | 14 | `var(--acc,#35D07F)` | 32 000 |
| 24px | 221px | 104px | auto | 14 | `` | 18 400 |
| 24px | 268px | 104px | auto | 14 | `#4C5666` | 10 600 |
| 464px | 170px | auto | auto | 23 | `` | 60 000 recipient |
| 464px | 220px | auto | auto | 23 | `` | 800 network fee 1.3% |
| 464px | 267px | auto | auto | 23 | `` | 200 change back to you |
| 24px | 300px | auto | auto | 14 | `#7A869C` | recipient address |
| 24px | 318px | auto | auto | 23 | `#4C5666` | bc1qzyg3h8ffkz7mvd3sjn54khce6mua7lqpz9x8gf |
| 0 | 356px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 366px | auto | auto | 14 | `#7A869C` | MAINNET real bitcoin · REPLACEABLE (RBF) you can raise the fee |
| 0 | 390px | 800px | 90px |  | `#0B0E14` | fill |
| 48px | 404px | 310px | 52px | 23 | `var(--accbg,#102417)` | SIGNING 2 OF 3 |
| 56px | 410px | 40px | 40px |  | `5px solid #10141D` | rule |
| 366px | 404px | 150px | 52px | 23 | `#4C5666` | DETAILS |
| 672px | 404px | 104px | 52px | 23 | `#4C5666` | BACK |

### 3a Twenty coins cautioned

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIGN |
| 132px | 28px | auto | auto | 14 | `#7A869C` | consolidate.psbt |
| 540px | 14px | 236px | 36px | 14 | `#0A0E15` |  1 CAUTION |
| 0 | 64px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 78px | auto | auto | 14 | `#7A869C` | TOTAL LEAVING YOUR WALLET |
| 24px | 92px | auto | auto | 48 | `` | 4 210 000 sats 0.04210000 BTC = amount + fee |
| 24px | 150px | auto | auto | 14 | `#7A869C` | SPENDING 20 OF YOUR COINS |
| 464px | 150px | auto | auto | 14 | `#7A869C` | WHERE IT GOES |
| 24px | 172px | auto | auto |  | `` | SVG: `M236,10 C280,10 290,55 330,55` w10<br>`M236,34 C280,34 290,55 330,55` w7<br>`M236,55 C280,55 290,55 330,55` w11 dash 3 5<br>`M236,76 C280,76 290,55 330,55` w3<br>`M236,100 C280,100 290,55 330,55` w2<br>`M330,55 C410,55 350,10 430,10` w12<br>`M330,55 C380,55 380,55 430,55` w3<br>junction `cx330 cy55 r5` |
| 24px | 174px | 206px | auto | 14 | `` | 1 400 000 |
| 24px | 198px | 206px | auto | 14 | `` | 820 000 |
| 24px | 219px | 206px | auto | 14 | `` | 16 more coins1 700 000 |
| 24px | 240px | 206px | auto | 14 | `` | 190 000 |
| 24px | 264px | 206px | auto | 14 | `` | 100 000 |
| 464px | 168px | auto | auto | 23 | `` | 4 200 000 recipient |
| 464px | 216px | auto | auto | 23 | `` | 10 000 network fee 0.2% |
| 464px | 270px | auto | auto | 14 | `` | no change, this empties all 20 |
| 24px | 292px | auto | auto | 14 | `#7A869C` | recipient address |
| 464px | 292px | 312px | auto | 14 | `#7A869C` | MAINNET · REPLACEABLE (RBF) |
| 24px | 310px | auto | auto | 23 | `#4C5666` | bc1qm52k4nv8ffkz7mvd3sjn54khce6mua7l7p9c |
| 24px | 344px | 752px | 44px | 14 | `#0A0E15` |  COINS LINKED: spending 20 at once shows they all belong to you. I UNDERSTAND |
| 0 | 390px | 800px | 90px |  | `#0B0E14` | fill |
| 48px | 404px | 310px | 52px | 23 | `#10141D` | HOLD TO SIGN |
| 56px | 410px | 40px | 40px |  | `5px solid #10141D` | rule |
| 366px | 404px | 150px | 52px | 23 | `#10141D` | DETAILS |
| 672px | 404px | 104px | 52px | 23 | `#10141D` | BACK |

### 3b Strand tapped

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIGN |
| 132px | 28px | auto | auto | 14 | `#7A869C` | payout-04.psbt |
| 540px | 14px | 236px | 36px | 14 | `#0A0E15` | SIGNING AS EC5A4595 |
| 0 | 64px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 78px | auto | auto | 14 | `#7A869C` | TOTAL LEAVING YOUR WALLET |
| 24px | 92px | auto | auto | 48 | `` | 61 000 sats 0.00061000 BTC = amount + fee |
| 24px | 150px | auto | auto | 14 | `#7A869C` | SPENDING 3 OF YOUR COINS |
| 464px | 150px | auto | auto | 14 | `#7A869C` | WHERE IT GOES |
| 24px | 172px | auto | auto |  | `` | SVG: `M126,10 C210,10 250,45 330,45` w9<br>`M126,45 C210,45 250,45 330,45` w6<br>`M126,80 C210,80 250,45 330,45` w4<br>`M330,45 C410,45 350,10 430,10` w11<br>`M330,45 C380,45 380,45 430,45` w3<br>`M330,45 C410,45 350,80 430,80` w2<br>junction `cx330 cy45 r5` |
| 24px | 174px | 106px | auto | 14 | `#7A869C` | 32 000 |
| 24px | 206px | auto | 26px | 14 | `#232E42` | 18 400 |
| 24px | 244px | 106px | auto | 14 | `#7A869C` | 10 600 |
| 464px | 168px | auto | auto | 23 | `` | 60 000 recipient |
| 464px | 206px | auto | auto | 23 | `` | 800 network fee 1.3% |
| 464px | 242px | auto | auto | 23 | `` | 200 change back to you |
| 24px | 272px | auto | auto | 14 | `#7A869C` | recipient address |
| 150px | 272px | auto | auto | 14 | `#4C5666` | bc1qzyg3h8ffkz7mvd3sjn54khce6mua7lqpz9x8gf |
| 24px | 300px | 752px | 80px | 14 | `#0A0E15` | COIN 2 OF 3 18 400 sats m/84'/0'/0'/1/03 amount proven by its previous transaction from 4f2a9c…d81e vout 1 CLOSE |
| 0 | 390px | 800px | 90px |  | `#0B0E14` | fill |
| 48px | 404px | 310px | 52px | 23 | `var(--accbg,#102417)` | HOLD TO SIGN |
| 56px | 410px | 40px | 40px |  | `5px solid #10141D` | rule |
| 366px | 404px | 150px | 52px | 23 | `#10141D` | DETAILS |
| 672px | 404px | 104px | 52px | 23 | `#10141D` | BACK |

### 4a Signing animation

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIGN |
| 132px | 28px | auto | auto | 14 | `#7A869C` | payout-04.psbt |
| 540px | 14px | 236px | 36px | 14 | `#0A0E15` | SIGNING AS EC5A4595 |
| 0 | 64px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 78px | auto | auto | 14 | `#7A869C` | TOTAL LEAVING YOUR WALLET |
| 24px | 92px | auto | auto | 48 | `` | 61 000 sats 0.00061000 BTC = amount + fee |
| 24px | 150px | 400px | 17px | 14 | `` | SIGNING COIN 1 OF 3 SIGNING COIN 2 OF 3 SIGNING COIN 3 OF 3 ALL 3 COINS SIGNED |
| 464px | 150px | auto | auto | 14 | `#7A869C` | WHERE IT GOES |
| 660px | 150px | 116px | auto | 14 | `var(--acc,#35D07F)` | LOCKED |
| 24px | 172px | auto | auto |  | `` | SVG: `M120,12 C210,12 250,59 330,59` w9<br>`M120,59 C210,59 250,59 330,59` w6<br>`M120,106 C210,106 250,59 330,59` w4<br>`M120,12 C210,12 250,59 330,59` w9 dash 420<br>`M120,59 C210,59 250,59 330,59` w6 dash 420<br>`M120,106 C210,106 250,59 330,59` w4 dash 420<br>`M330,59 C410,59 350,12 430,12` w11<br>`M330,59 C380,59 380,59 430,59` w3<br>`M330,59 C410,59 350,106 430,106` w2<br>junction `cx330 cy59 r5` |
| 24px | 174px | 104px | auto | 14 | `` | 32 000 |
| 24px | 221px | 104px | auto | 14 | `` | 18 400 |
| 24px | 268px | 104px | auto | 14 | `` | 10 600 |
| 464px | 170px | auto | auto | 23 | `` | 60 000 recipient |
| 464px | 220px | auto | auto | 23 | `` | 800 network fee 1.3% |
| 464px | 267px | auto | auto | 23 | `` | 200 change back to you |
| 24px | 300px | auto | auto | 14 | `#7A869C` | recipient address |
| 24px | 318px | auto | auto | 23 | `#4C5666` | bc1qzyg3h8ffkz7mvd3sjn54khce6mua7lqpz9x8gf |
| 0 | 356px | 800px | 1px |  | `#1E2531` | fill |
| 24px | 366px | auto | auto | 14 | `#7A869C` | MAINNET real bitcoin · REPLACEABLE (RBF) you can raise the fee |
| 0 | 390px | 800px | 90px |  | `#0B0E14` | fill |
| 48px | 404px | 310px | 52px | 23 | `var(--accbg,#102417)` | SIGNING SIGNED |
| 56px | 410px | 40px | 40px |  | `5px solid #10141D` | rule |
| 366px | 404px | 150px | 52px | 23 | `#4C5666` | DETAILS |
| 672px | 404px | 104px | 52px | 23 | `#4C5666` | BACK |

### 5a Details

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | DETAILS |
| 196px | 28px | auto | auto | 14 | `#7A869C` | payout-04.psbt |
| 560px | 28px | 192px | 44px | 14 | `#10141D` | SIMPLE EXPLAINERS |
| 28px | 100px | 288px | 296px |  | `#0A0E15` | fill |
| 328px | 100px | 424px | 296px |  | `#0A0E15` | fill |
| 40px | 108px | 264px | auto | 14 | `` | INPUTS (3) - ALL VERIFIED YOURS |
| 40px | 150px | 264px | auto | 23 | `` | 32 000 sats 4f2a9c1b...8e2d1a04 : 1 m/84'/0'/0'/0/12 |
| 40px | 224px | 264px | auto | 23 | `` | 18 400 sats 9b31d0ae...4c77f210 : 0 m/84'/0'/0'/1/03 |
| 40px | 298px | 264px | auto | 23 | `` | 10 600 sats c05e7748...1af96b3d : 2 m/84'/0'/0'/0/07 |
| 340px | 108px | auto | auto | 14 | `` | TRANSACTION ID ? |
| 340px | 130px | 400px | auto | 14 | `` | 8f2a 41c7 9b0d e5a3 7c11 6d84 02fb 39aec4d7 158b 6a20 ef93 3d5c 7e01 b862 40af |
| 340px | 174px | auto | auto | 14 | `#7A869C` | your coordinator shows this same id |
| 340px | 196px | auto | auto | 23 | `` | = 0.00061000 BTC |
| 340px | 230px | auto | auto | 14 | `` | 2.4 sat/vB, 1.3% of what you send ? |
| 340px | 256px | auto | auto | 14 | `` | version 2, locktime 0 ? |
| 340px | 276px | 400px | auto | 14 | `#7A869C` | can confirm any time (normal) |
| 340px | 302px | auto | auto | 14 | `` | sighash ALL ? |
| 340px | 322px | 400px | auto | 14 | `#7A869C` | signatures cover every destination and its amount |
| 340px | 348px | auto | auto | 14 | `` | replaceable (RBF) ? |
| 340px | 368px | 400px | auto | 14 | `#7A869C` | the fee can be bumped after broadcast |
| 610px | 404px | 140px | 52px | 23 | `#10141D` | BACK |

### 5b Explainers

| x | y | w | h | rung | colour | content |
|---|---|---|---|---|---|---|
| 24px | 12px | auto | auto | 34 | `var(--acc,#35D07F)` | SIMPLE EXPLAINERS |
| 24px | 88px | 752px | 290px |  | `#0A0E15` | fill |
| 400px | 104px | 1px | 258px |  | `#1A2130` | fill |
| 40px | 104px | 344px | auto | 14 | `` | INPUTS coins this transaction spends. |
| 40px | 172px | 344px | auto | 14 | `` | OUTPUTS every place it sends bitcoin. |
| 40px | 240px | 344px | auto | 14 | `` | CHANGE the part returned to your own verified address. |
| 40px | 308px | 344px | auto | 14 | `` | TXID the tracking ID used after broadcast. |
| 424px | 104px | 328px | auto | 14 | `` | FEE RATE fee paid per unit of transaction size. |
| 424px | 172px | 328px | auto | 14 | `` | LOCKTIME the earliest block it may confirm in. |
| 424px | 240px | 328px | auto | 14 | `` | DERIVATION PATH the route from seed words to addresses. |
| 424px | 308px | 328px | auto | 14 | `` | DESCRIPTOR lets a coordinator watch, never spend. |
| 610px | 404px | 140px | 52px | 23 | `#10141D` | BACK |

---

## 9. Not in this handoff

The caution-rows page (`WHY FLAGGED`) and the signed / QR-out screen are still on
the old chrome. They should follow, so the flow reads as one piece end to end.
