# design/

Drawings that specify a screen precisely enough to build it.

These are **results**, not briefs. The design review bundle that produced them
is unzipped into `design_handoff_kiss_signer/` and is gitignored on purpose:
the repo carries what was decided, not the material it was decided from.

## The rules a drawing in this folder follows

A drawing here is only useful if the device can actually draw it, so each one
is bound by the same three constraints as the firmware:

1. **Four type rungs, nothing between them.** `wt_font14`, `wt_font23`,
   `wt_font28`, `wt_font34`, plus whatever new faces the drawing is proposing.
2. **One weight.** `tools/fonts/gen_fonts.sh` builds every face from Montserrat
   Medium, so there is no bold. Hierarchy comes from size, colour and letter
   spacing.
3. **Fixed action geometry.** `WT_ACTION_Y 404`, `WT_ACTION_H 52`, and nothing
   above the row may cross `WT_CONTENT_BOTTOM`. The back pill's x is *not*
   fixed, and this rule used to say it was. It ends flush with the content lane
   of the screen it sits on, because the app has no single lane: measured off
   the frames, Settings' right column ends at 770, Receive's at about 751, and
   Sign's panels at 776. `WT_BACK_X 610` ends at 750, which is the 48px page
   margin `main/wallet_theme.c:373` declares and what most screens are drawn
   to, so it stays the default. Sign is the exception and carries its own
   `SG_BACK_X 636`.

Colours are the `main/wallet_theme.h` macros. Never a new one.

Every file is self contained: fonts are subset from the repo's own originals
and inlined, so a drawing renders identically offline, with no network, no CDN
and no runtime. Open one by double clicking it.

Each drawing carries a self check that runs on load and walks every element in
every device frame, failing on any size, family or weight that the firmware
cannot produce. That check exists because the review these drawings correct
shipped with nine invented type sizes and a weight that does not exist, and a
human reading a mockup cannot see either.

## Contents

| File | Screens | Source |
|---|---|---|
| `sign-screens-buildable.html` | Sign normal, Sign cautions, and the zero cost fallback | `main/wallet_sign.c` |

## Fonts these drawings embed

| Family | Subset from | Licence |
|---|---|---|
| Montserrat | `managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf` | SIL OFL 1.1 |
| KissIcons | `FontAwesome5-Solid+Brands+Regular.woff`, same folder, two glyphs | CC BY 4.0 |
| KissMono | Ioskeley Mono, proposed, not yet in the firmware | SIL OFL 1.1 |

The first two are the exact files `gen_fonts.sh` feeds to `lv_font_conv`, so a
drawing and the firmware rasterise the same outlines.
