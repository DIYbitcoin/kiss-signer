# Handoff 02, the sign bundle graph

Target: `main/kiss_sign.c`, `main/kiss_theme.c`, `i18n/*.json`.
Drawing this specifies: `design/sign-bundle.dc.html`, frames `2c`, `3a`, `3b`, `4a`, `5a`, `5b`.
800x480, the four type rungs (14 / 23 / 34 / 48), existing theme macros. No new colours,
no new fonts, no new sizes.

Revision 2 corrects three claims in revision 1 that did not match the tree, and answers
the question revision 1 never asked (more than five outputs). Each is marked **[R2]**.

---

## 1. What is new, in one line each

| # | Change | Where |
|---|--------|-------|
| 1 | Verify screen leads with a converging bundle graph, inputs left, junction, outputs right | `kiss_sign.c` verify page |
| 2 | Strand thickness is proportional to value | new widget in `kiss_theme.c` |
| 3 | Long *input* lists elide the middle, stating hidden count and hidden total | same widget |
| 4 | Outputs are never elided; above the fold they keep today's scroll gate | section 6 **[R2]** |
| 5 | Signing reveals all strands on success, not one at a time | section 3 **[R2]** |
| 6 | Accent rim on the suggested action only, 1 call site | section 4 **[R2]** |
| 7 | Fee states its share of the send | existing `S_FEERATE_PCT_FMT` |
| 8 | Details page: one `?` per term, not one for the row | section 5 |
| 9 | Details page gains an outputs list | section 6 **[R2]** |
| 10 | Glossary becomes eight term rows | `glossary_cb`, layout only |

---

## 2. The bundle widget

New builder, beside the other `wt_*` builders:

```c
typedef struct {
    uint64_t sats;
    bool     signed_ok;     // drawn in wt_accent() once signatures land
    bool     is_group;      // the elided middle: dashed, holds N coins
    uint16_t group_n;
} wt_strand_t;

lv_obj_t *wt_bundle(lv_obj_t *scr, int x, int y, int w, int h,
                    const wt_strand_t *in,  size_t n_in,
                    const wt_strand_t *out, size_t n_out);
```

**Stroke width from value.** Linear on the largest strand, with a floor:

```c
int wt_strand_px(uint64_t sats, uint64_t max_sats)
{
    if (!max_sats) return 2;
    int px = (int)((sats * 11) / max_sats);      // 11px is the widest strand
    return px < 2 ? 2 : px;                       // 2px floor, or dust vanishes
}
```

The floor is not cosmetic: an 800 sat fee against a 4.2M send is 0px unclamped, and
2px is the thinnest stroke that still reads as a line on this panel.

**Input elision.** Above five inputs: first two, one group strand, last two. Five rows
at any count. The group strand is dashed (`LV_STYLE_LINE_DASH_WIDTH 3`, `GAP 5`) so a
bundle never reads as one coin, and its label carries both numbers — `S_BUNDLE_MORE_FMT`
beside the summed value in mono14.

Inputs may be bundled because they are all yours and their total is the fact that
matters. Outputs may not, for the reason in section 6.

Exact geometry for both cases is in section 9.

---

## 3. Signing **[R2 — revision 1 was wrong]**

Revision 1 claimed `kiss_psbt_sign` walks inputs in order. It does not.
`kiss_psbt.c:1135` is one `wally_psbt_sign_bip32(s_psbt, master, EC_FLAG_GRIND_R)` that
signs every input inside a single libwally call, and `sign_sp_spends` follows it. There
is no per-input hook and no callback.

**Do not add a timer that walks the strands while that call runs.** A progress display
whose steps are invented is worse than none here: the whole claim of this screen is that
a strand turning green means a signature exists.

Do this instead:

1. On hold complete, the graph enters a signing state: all input strands to `WT_INK` at
   full opacity, output strands to `0x2A3346`, output side takes the `LOCKED` eyebrow,
   `DETAILS` and `BACK` inert. Header is `S_SIGNING` (no count).
2. The existing hold pill keeps its arc. The arc means work in progress, which is true.
3. On return 0, every input strand and its label repaint to `wt_accent()` together, and
   the header becomes `S_ALL_SIGNED_FMT`. On non-zero, no strand changes colour and the
   existing failure path runs unchanged.

So `S_SIGNING_COIN_FMT` from revision 1 is dropped, and `S_SIGNING` is used instead.

**If per-input progress is wanted later**, it needs a real loop, not a fake one: check
whether the pinned libwally exposes per-input signing (`wally_psbt_sign_input_bip32` or
equivalent) in this version. If it does, iterate inputs and fire the callback per index,
and `S_SIGNING_COIN_FMT` comes back. If it does not, the honest screen is the one above.
Decide that from the pinned header, not from this document.

Frame `4a` in the drawing animates a per-coin sequence. That frame is a **review
artifact for the look of the accent reveal**, not a spec for the timing. Build step 3.

---

## 4. The accent rim **[R2 — revision 1 was over-scoped]**

Revision 1 said to move `wt_pill`'s border from `WT_MUT` to the accent. That is 95 call
sites and it breaks two documented rules:

- `kiss_theme.c:1431` — GREEN is byte-identical to `WT_OK` and ORANGE is a near match for
  `WT_WARN`, so anywhere the two can be confused the accent stands aside. A green-bordered
  `ERASE` reads as safe.
- ADDENDUM-02 rule 3 — the accent marks the *suggested* action. If every pill wears it,
  it marks nothing, and `docs/device-ux-test.md` task 6 tests exactly that.

**Scope it to one call site:** the suggested action on the sign screen, `HOLD TO SIGN`.
It takes the accent rim and the `ACC_BG_HEX` fill. `DETAILS`, `BACK`, and every other
pill in the app keep `WT_MUT`. Inert pills keep `0x2A3346`.

That is also what rule 3 already asks for, so this is applying the rule, not amending it.

**Wiring note, easy to half-do.** `accent_walk` (`kiss_theme.c:1447`) sets
`lv_obj_set_style_text_color` only, so `WT_FLAG_ACCENT` will not repaint a border. For
the rim to survive a theme change while the screen is up, either extend `accent_walk` to
set border colour on a second flag (`WT_FLAG_ACCENT_BORDER`), or repaint that one pill
explicitly in the sign screen's restyle path. Do one of the two; a flagged object with an
unhandled property is how the rim goes stale.

---

## 5. Details page, one `?` per term

Today the fee row carries a chip whose card covers fee rate, version, locktime and
sighash together. Give each of the four rows its own chip at the row's right edge, each
opening only its own term, so a reader taps the word they do not know.

Chips stay circles — `kiss_theme.c` already states the rule: a rectangle does something,
a circle is a thing.

`GLOSS_ICONS` and its `wt_accent()` draw at `kiss_sign.c:1664` already exist and already
render. Nothing is needed for the icons but the row layout. The HTML drawing redraws them
as vectors only because the review page ships a two-glyph icon subset; do not port those
vectors.

---

## 6. More than five outputs **[R2 — new, revision 1 did not cover this]**

`WPSBT_MAX_OUTS` is 16, and `kiss_sign.c:1466-1482` holds `HOLD TO SIGN` inert until the
recipient list has been scrolled to its end — measured by `lv_obj_get_scroll_bottom`, not
counted, because one wrapping bech32m address in `de` is a scroll and three short ones may
not be. That gate exists to stop a second destination hiding below the fold.

**Outputs are therefore never elided, at any count.** An elided output would be a hidden
recipient that is visible nowhere — `DETAILS` lists inputs only today — which is the exact
failure the gate was built to prevent. Bundling inputs is safe; bundling outputs is a
security regression.

Above the fold, the output side of the graph becomes the scrolling list it already is:

- The junction and the input side stay fixed. Only the output column scrolls.
- Keep the existing `LV_SCROLLBAR_MODE_ON` / `OFF` behaviour verbatim, including the
  reason it is `ON` and not `AUTO`: a list with more below the fold must not look
  identical to one that ends there.
- Keep `s_recip_seen`, `recip_scroll_cb`, and the inert `HOLD TO SIGN` until the end of
  the list is reached, unchanged.
- Strands are drawn to the visible rows. A strand whose row has scrolled out is clipped at
  the column's edge, not redirected — a strand must never appear to end somewhere its row
  is not.
- `wt_strand_px` still uses the transaction's true `max_sats`, so thickness stays
  comparable across a scroll rather than renormalising per screenful.

**Second change, same reason:** the details page gains an outputs list beside its inputs
list, so every destination is readable somewhere that does not scroll under a gate.
`S_D_INPUTS_FMT` has an obvious sibling; add `S_D_OUTPUTS_FMT`
(`OUTPUTS (%u) - %u TO YOU`) and give the right card the same treatment the left one has.

---

## 7. New strings

Five keys and one edit, all 21 locales. (`S_SIGNING_COIN_FMT` from revision 1 is dropped
per section 3; `S_D_OUTPUTS_FMT` is added per section 6.)

| Key | English |
|-----|---------|
| `S_BUNDLE_IN_FMT` | `SPENDING %u OF YOUR COINS` |
| `S_BUNDLE_OUT` | `WHERE IT GOES` |
| `S_BUNDLE_MORE_FMT` | `%u more coins` |
| `S_BUNDLE_NOCHANGE_FMT` | `no change, this empties all %u` |
| `S_ALL_SIGNED_FMT` | `ALL %u COINS SIGNED` |
| `S_D_OUTPUTS_FMT` | `OUTPUTS (%u) - %u TO YOU` |

Edit: `S_TOTAL_LEAVING`, `TOTAL LEAVING WALLET` becomes `TOTAL LEAVING YOUR WALLET`. The
coordinator is watch-only and nothing of value leaves the signer, so the owner of those
coins is the person holding the device.

`S_SIGNING` already exists. The glossary needs no new strings: `S_GLOSSARY_B` is already
written one `TERM: definition` per line in every locale, which `wt_split_colon` reads
directly, so the eight-row layout is a layout change only.

---

## 8. Screens that must still work

- **3 in, 1 recipient, change** — frame `2c`, the common case.
- **20 in, 1 recipient, no change** — frame `3a`. Always carries the coins-linked caution,
  because `WPSBT_C_MERGE_INS` fires at 5 inputs, and `HOLD TO SIGN` is inert until it is
  acknowledged. Third output row states the no-change case rather than going missing.
- **6+ outputs** — section 6. Gate intact, nothing elided.
- **A strand tapped** — frame `3b`.
- **Signing** — section 3.

---

## 9. Coordinate appendix

Page origin is the screen's top left. Every value is px. Rungs: `f14`, `f23`, `f34`,
`f48`. Colours: `INK` `#E8EEF7`, `MUT` `#7A869C`, `DIM` `#4C5666`, `RIM` `#1E2531`,
`CARD` `#0A0E15`, `BAR` `#0B0E14`, `INERT` `#2A3346`, `ACC` `wt_accent()`.

### 9.1 Chrome, identical on every frame

| Element | x | y | w | h | Type / colour |
|---|---|---|---|---|---|
| Page title | 24 | 12 | — | — | f34, ACC |
| Filename | 132 | 28 | — | — | f14, MUT |
| Signer badge | 540 | 14 | 236 | 36 | r12, CARD, rim ACC |
| Header rule | 0 | 64 | 800 | 1 | RIM |
| Hero caption | 24 | 78 | — | — | f14, MUT, tracking .14em |
| Hero amount | 24 | 92 | — | — | f48 mono INK, then f23 `sats`, then f14 mono MUT |
| Content rule | 0 | 356 | 800 | 1 | RIM |
| Meta row | 24 | 366 | — | — | f14, MUT with INK terms |
| Action bar | 0 | 390 | 800 | 90 | BAR, 1px RIM top border |
| HOLD TO SIGN | 48 | 404 | 310 | 52 | r10, `ACC_BG`, 2px ACC rim, f23 |
| Arc | 56 | 410 | 40 | 40 | r20, 5px ring |
| DETAILS | 366 | 404 | 150 | 52 | r10, `#10141D`, 1px MUT rim, f23 |
| BACK | 672 | 404 | 104 | 52 | r10, `#10141D`, 1px MUT rim, f23 |

Inert pills: rim `INERT`, label `DIM`. Sweep fill on the hold pill is
`rgba(255,77,94,.35)`, left-anchored, width = progress.

### 9.2 Frame 2c — 3 inputs

Graph box `x=24 y=172 w=752 h=118`. Path data below is in box coordinates; page
coordinate = box origin + path coordinate (the box is 1:1).

| Element | x | y | w | Type |
|---|---|---|---|---|
| Left caption `S_BUNDLE_IN_FMT` | 24 | 150 | — | f14 MUT .14em |
| Right caption `S_BUNDLE_OUT` | 464 | 150 | — | f14 MUT .14em |
| Input labels (right-aligned) | 24 | 174 / 221 / 268 | 104 | f14 mono |
| Output rows | 464 | 170 / 220 / 267 | — | f23 mono + f14 label |
| Address caption | 24 | 300 | — | f14 MUT |
| Address | 24 | 318 | — | f23 mono, DIM body, INK first/last 4 with 2px underline |

Strands, `stroke-linecap: round`, junction dot `cx=330 cy=59 r=5` INK:

| Strand | Path | px | Colour |
|---|---|---|---|
| in 1 | `M120,12 C210,12 250,59 330,59` | 9 | MUT |
| in 2 | `M120,59 C210,59 250,59 330,59` | 6 | MUT |
| in 3 | `M120,106 C210,106 250,59 330,59` | 4 | MUT |
| send | `M330,59 C410,59 350,12 430,12` | 11 | INK |
| fee | `M330,59 C380,59 380,59 430,59` | 3 | DIM |
| change | `M330,59 C410,59 350,106 430,106` | 2 | ACC |

Change row's label and amount are the only ACC text in the graph. Fee row appends its
share in f14 mono MUT (`S_FEERATE_PCT_FMT`).

### 9.3 Frame 3a — 20 inputs, no change

Graph box `x=24 y=172 w=752 h=110`. Row pitch 24 on the left, junction `cx=330 cy=55`.

| Element | x | y | w | Type |
|---|---|---|---|---|
| Input labels (right-aligned) | 24 | 174 / 198 / 219 / 240 / 264 | 206 | f14 mono |
| Group row | — | 219 | 206 | f14 MUT `S_BUNDLE_MORE_FMT` + f14 mono MUT total, `nowrap` |
| Output rows | 464 | 168 / 216 | — | f23 mono + f14 label |
| No-change note | 464 | 270 | — | f14, MUT + INK |
| Address caption / address | 24 | 292 / 310 | — | as 9.2 |
| Caution bar | 24 | 344 | 752 | h44, r12, CARD, 1px `WT_WARN` rim |
| ↳ icon | 15 | 12 | — | f14 `WT_WARN` (bar-relative) |
| ↳ text | 52 | 13 | **475** | f14 INK, 1.3 line height (bar-relative) |
| ↳ pill | 543 | 4 | 170 | h36, r10, f14 (bar-relative) |

The 475px text box is the fix for the string running under the pill.

Strands: in `M236,{10,34,55,76,100} C280,… 290,55 330,55` at px 10 / 7 / **11 dashed** /
3 / 2; out `M330,55 C410,55 350,10 430,10` at 12 (send) and `M330,55 C380,55 380,55
430,55` at 3 (fee).

### 9.4 Frame 3b — a strand tapped

Graph box `x=24 y=172 w=752 h=90`, junction `cx=330 cy=45`.

| Element | x | y | w | h | Type |
|---|---|---|---|---|---|
| Input labels | 24 | 174 / 244 | 106 | — | f14 mono MUT |
| Selected chip | 24 | 206 | — | 26 | r10, `#232E42`, 1px INK rim, f14 mono |
| Output rows | 464 | 168 / 206 / 242 | — | — | as 9.2 |
| Address caption | 24 | 272 | — | — | f14 MUT |
| Address (inline) | 150 | 272 | — | — | f14 mono |
| Coin card | 24 | 300 | 752 | 80 | r12, CARD, 1px INK rim |

Card contents, card-relative: `COIN %u OF %u` at 15/11 f14 MUT; amount right-aligned at
15/7 f23 mono; path at 15/35 f14 mono MUT; proven tick + text at 200/35; `from <txid
short> vout %u` at 15/57; `CLOSE` pill right-aligned at 15/53, w104 h24 r10.

Unselected input strands drop to DIM; the selected one is INK. Output strands unchanged.

### 9.5 Frame 5a — details

| Element | x | y | w | h |
|---|---|---|---|---|
| Filename | 196 | 28 | — | — |
| `SIMPLE EXPLAINERS` pill | 560 | 28 | 192 | 44 |
| Left card | 28 | 100 | 288 | 296 |
| Right card | 328 | 100 | 424 | 296 |
| Inputs header (icon + f14 .14em) | 40 | 108 | 264 | — |
| Input blocks | 40 | 150 / 224 / 298 | 264 | — |
| `TRANSACTION ID` (icon + f14) | 340 | 108 | — | — |
| txid, two lines of 8 groups | 340 | 130 | 400 | — |
| coordinator note | 340 | 174 | — | — |
| BTC total | 340 | 196 | — | — |
| fee row (icon + f14 mono + `?`) | 340 | 230 | — | — |
| version / locktime row | 340 | 256 | — | — |
| ↳ note | 340 | 276 | 400 | — |
| sighash row | 340 | 302 | — | — |
| ↳ note | 340 | 322 | 400 | — |
| RBF row | 340 | 348 | — | — |
| ↳ note | 340 | 368 | 400 | — |
| BACK | 610 | 404 | 140 | 52 |

Input block, block-relative: tick + f23 mono amount + f14 `sats` on line 1; txid `: vout`
in f14 mono MUT at +2; tick + path in f14 mono ACC at +2. Paths print apostrophe-hardened
(`m/84'/0'/0'/0/12`). Header uses the plain hyphen from `S_D_INPUTS_FMT`.

`?` chips: 22x22, r11, `#10141D`, 1px MUT rim, f14 MUT, at the row's right edge.

Right column ends at y=388, inside the card's floor. When the outputs list of section 6 is
added, the right card's rows re-flow; keep the 388 floor.

### 9.6 Frame 5b — explainers

| Element | x | y | w | h |
|---|---|---|---|---|
| Card | 24 | 88 | 752 | 290 |
| Column divider | 400 | 104 | 1 | 258 |
| Left column rows | 40 | 104 / 172 / 240 / 308 | 344 | — |
| Right column rows | 424 | 104 / 172 / 240 / 308 | 328 | — |
| BACK | 610 | 404 | 140 | 52 |

Each row: icon (16x16, ACC) + term in f14 .14em INK, definition in f14 MUT at +4, line
height 1.4. Terms in `GLOSS_ICONS` order — INPUTS, OUTPUTS, CHANGE, TXID down the left;
FEE RATE, LOCKTIME, DERIVATION PATH, DESCRIPTOR down the right.

---

## 10. Where these two files live **[R2]**

`.gitignore:24-27` ignores `design_handoff_kiss_signer/` on purpose — "the repo carries
the results rather than the brief" — so revision 1's instruction to commit there was
wrong.

`design/README.md` describes exactly what these two files are: drawings that specify a
screen precisely enough to build it, kept as results. Put both there:

```
design/sign-bundle.dc.html          the drawing, frames 2c 3a 3b 4a 5a 5b
design/HANDOFF-02-sign-bundle.md    this file
```

The HTML needs `support.js` and `kiss-fonts.css` beside it to open in a browser. If
`design/` should stay dependency-free, commit a self-contained single-file build instead
and note it in `design/README.md`.

---

## 11. Not in this handoff

The caution-rows page (`WHY FLAGGED`) and the signed / QR-out screen are still on the old
chrome. They should follow, so the flow reads as one piece end to end.
