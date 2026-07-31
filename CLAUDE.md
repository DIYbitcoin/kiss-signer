# KISS Signer — working rules

## Screen chrome

A content screen is **not** a title, a paragraph and a button. That shape has
shipped more than once and it is the thing being corrected. Every screen the
owner reads is built from the kit in `main/wallet_theme.c`; nothing here needs
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
   When the blocks have headings, measure the shared body font against
   `max_h - 46 - 8`: `wt_why_block` draws the heading above the body at font14,
   and 46 covers a heading that wraps to two lines.
3. **Marks before words.** Every chip and row label carries an icon.
4. **Only glyphs already in `SYMS`** (`tools/fonts/gen_fonts.sh`). Anything else
   forces a font rebuild across four scripts. Available at every size: all
   `LV_SYMBOL_*` plus `WT_ICON_QR/KEY/SECRET/SD/LOCK/REPLACE`.
5. **Nothing crosses `WT_CONTENT_BOTTOM` (398).**

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
```

`overlapcheck` asks six questions per stop: TEXT, CONTENT, GROWTH, CLIPPED,
ROLE and **BARE**. BARE is rule 1 above, enforced: a screen with a wide
paragraph and no framed element fails the build. Screens that are already bare
are listed in `OC_BARE_BACKLOG` in `sim/overlapcheck.c`; that list only shrinks,
and the run prints how many are left.

## i18n

Strings live in `i18n/*.json` (21 locales) and are generated into
`main/i18n_keys.h` + `main/i18n_tables.c` by `python3 tools/gen_i18n.py`. CI has
a drift gate, so regenerate after every string change.

After generating, check no glyph was **gained**:

```bash
python3 -c "
import subprocess,glob
for f in sorted(glob.glob('tools/fonts/glyphs_*.txt')):
    old=subprocess.run(['git','show','HEAD:'+f],capture_output=True,text=True).stdout.strip()
    g=[c for c in open(f,encoding='utf-8').read().strip() if c not in old]
    print(f, 'gained', ''.join(g) or 'none')"
```

A gained CJK glyph means a font rebuild across four scripts. **Reword instead.**

## Device test verdict

Every PR gets an explicit **DEVICE TEST: REQUIRED** (with the exact flows) or
**NOT REQUIRED** (with the reason hardware cannot change the outcome). Passing
gates are never the verdict and never justify NOT REQUIRED on their own.

Default to REQUIRED for anything touching display, camera, QR, SD, buttons,
touch, USB or timing. The simulator does not compile `main/camera_spike.c` and
never runs `rot_flush`'s camera branch, so no gate can see a preview bug.
