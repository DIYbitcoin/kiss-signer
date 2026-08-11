#!/bin/bash
# Generate the wallet i18n fonts (main/font_kiss_*.c).
#
# Sources (all on disk and OFL-licensed):
#   Montserrat-Medium.ttf                 Latin + Latin-Ext + Vietnamese + Cyrillic
#   FontAwesome5-Solid+Brands+Regular.woff  the LV_SYMBOL_* icons (PUA)
#   vendor/SourceHanSansJP-Normal.otf     Japanese regional glyph forms
#   SourceHanSansSC-Normal.otf            Korean + Simplified Chinese subsets
#
# CJK fonts are subset to EXACTLY the glyphs used by that locale's i18n JSON
# (tools/fonts/glyphs_*.txt, emitted by tools/gen_i18n.py). Run gen_i18n.py
# first, and re-run THIS script whenever a glyphs_*.txt changes: a glyph
# missing from the font draws an empty placeholder box, so a word in a CJK
# locale loses a character silently (device included).
#
# Fallback chain baked into the structs: lat -> ja -> ko -> zh, so every
# wallet label can use font_kiss_lat<N> and any script just resolves.
set -e
cd "$(dirname "$0")"
LVF=../../managed_components/lvgl__lvgl/scripts/built_in_font
JP=vendor/SourceHanSansJP-Normal.otf
OUT=../../main

[ -d "$LVF" ] || { echo "LVGL component not fetched (need $LVF)"; exit 1; }
[ -f "$JP" ] || { echo "Japanese font missing (need tools/fonts/$JP)"; exit 1; }
[ -d node_modules ] || npm install --no-audit --no-fund

# must match LAT_RANGES in tools/gen_i18n.py (the hang-prevention check)
LAT="0x20-0x7E,0xA0-0xFF,0x100-0x17F,0x1A0-0x1B0,0x1EA0-0x1EF9,0x400-0x45F,0x490-0x491,0x2018-0x201D,0x2022,0x2026"
# The LV_SYMBOL_* codepoints from LVGL's built_in_font_gen.py, PLUS three icons
# LVGL has no symbol macro for. They are named in kiss_theme.h as WT_ICON_*
# and must stay in lockstep with it: a codepoint referenced by a label but
# missing from the font draws a blank box the width of half a line, which on
# an icon pill means a button with nothing on it.
#   61481 F029 qrcode    61572 F084 key    61979 F21B user-secret
#   61475 F023 lock  (the RBF explainer's "final" state; its "replaceable"
#                     state uses F021 sync, which LVGL already ships)
# (63426 F7C2 sd-card was already here as LV_SYMBOL_SD_CARD.)
# Only the Latin faces need these: every locale's font is a Latin base with the
# CJK face as its FALLBACK, so an icon resolves in the base whatever the language.
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61475,61478,61479,61480,61481,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61572,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,61979,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

conv() { npx lv_font_conv --no-compress --no-prefilter --bpp 4 --format lvgl \
                          --force-fast-kern-format "$@"; }

for SZ in 14 28; do
  echo "== font_kiss_lat$SZ"
  conv --size $SZ \
    --font "$LVF/Montserrat-Medium.ttf" -r "$LAT" \
    --font "$LVF/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
    --lv-fallback font_kiss_ja$SZ \
    -o "$OUT/font_kiss_lat$SZ.c"

  for L in ja ko zh; do
    case $L in
      ja) FB="--lv-fallback font_kiss_ko$SZ" ;;
      ko) FB="--lv-fallback font_kiss_zh$SZ" ;;
      zh) FB="" ;;
    esac
    echo "== font_kiss_$L$SZ"
    FONT="$LVF/SourceHanSansSC-Normal.otf"
    if [ "$L" = ja ]; then
      FONT="$JP"
    fi
    conv --size $SZ \
      --font "$FONT" --symbols "$(cat glyphs_$L.txt)" \
      $FB -o "$OUT/font_kiss_$L$SZ.c"
  done
done

# 23px: the MIDDLE body rung (wt_body_font) as well as the wallet-home tile
# titles. It used to be tile titles only, so the CJK subsets carried just
# glyphs_tile_*.txt -- about a dozen characters. The moment 23 became a body
# size that rendered every ja/ko/zh screen as tofu boxes, so these now take the
# same full glyph set as 14 and 28.
#
# It also carries the FontAwesome plane. That used to be skipped here with the
# note "23 never shows icons", which stopped being true the moment PILL LABELS
# started auto-fitting to 23: RECEIVE's chevrons went straight to tofu boxes.
# Any size a pill label can take needs the symbols.
echo "== font_kiss_lat23"
conv --size 23 \
  --font "$LVF/Montserrat-Medium.ttf" -r "$LAT" \
  --font "$LVF/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
  --lv-fallback font_kiss_ja23 \
  -o "$OUT/font_kiss_lat23.c"
for L in ja ko zh; do
  case $L in
    ja) FB="--lv-fallback font_kiss_ko23" ;;
    ko) FB="--lv-fallback font_kiss_zh23" ;;
    zh) FB="" ;;
  esac
  echo "== font_kiss_${L}23"
  FONT="$LVF/SourceHanSansSC-Normal.otf"
  if [ "$L" = ja ]; then
    FONT="$JP"
  fi
  conv --size 23 \
    --font "$FONT" --symbols "$(cat glyphs_$L.txt)" \
    $FB -o "$OUT/font_kiss_${L}23.c"
done

# 34px: page titles and primary buttons. LATIN/CYRILLIC ONLY, and the only size
# in this project that does not exist for all four scripts.
#
# Why not all four: the CJK subsets at 34 would add roughly 4.5MB, and the app
# is already 8.9MB of a 12MB partition. Latin alone is ~1.1MB and fits. CJK
# glyphs also read considerably larger than Latin at the same pixel size, so
# leaving ja/ko/zh titles at 28 costs far less legibility than the arithmetic
# suggests.
#
# THE FALLBACK IS LOAD-BEARING, though not for the reason this note used to
# give. A glyph missing from an LVGL font is not an infinite loop: with
# LV_USE_FONT_PLACEHOLDER on, which it is in both builds, the lookup walks the
# fallback chain and then returns a blank box half a line wide. What it costs
# is the character, silently, on a screen the holder cannot file a bug from.
# There is no font_kiss_ja34 to chain to, so this chains to the 28 CJK
# faces: a stray CJK character in a Latin-locale title renders one size small
# instead of vanishing. wt_font34() must ALSO refuse to hand this face
# to a CJK locale in the first place; this chain is the second line of defence,
# not the first.
echo "== font_kiss_lat34"
conv --size 34 \
  --font "$LVF/Montserrat-Medium.ttf" -r "$LAT" \
  --font "$LVF/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
  --lv-fallback font_kiss_ja28 \
  -o "$OUT/font_kiss_lat34.c"

# ---------------------------------------------------------------------------
# The three fixed pitch faces. Latin only, and deliberately so.
#
# Addresses, fingerprints, derivation paths and amounts are never translated,
# so these have no CJK variant and no per locale build. That is the whole
# reason they are affordable: three Latin only faces together are about 17KB,
# against roughly 1.3MB for one more LOCALISED rung once the three CJK subsets
# are built. Measured, not estimated: see design/sign-screens-buildable.html.
#
# They must never be handed a localised string. There is no fallback chain out
# of them on purpose, because a chain would hide the mistake rather than show
# it: a stray CJK character here draws LVGL's placeholder box, which is loud
# and findable, instead of silently resolving one size small.
#
# What they buy, in order:
#   1. A tabular figure. An amount is the same width on every screen it appears
#      on, so digits stop shifting column between Sign, Receive and Verify.
#   2. A body the user can scan one character at a time, which is literally the
#      task "compare these 8" sets on the Sign screen.
#   3. Both compared runs come out the same width, so their underlines match.
#
# 0x20-0x7E is the full printable ASCII set rather than the bech32 charset
# alone. The extra glyphs cost under 2KB across both sizes and buy uppercase
# for the fingerprint (EC5A4595) and the punctuation in a derivation path
# (m/84'/0'/0'), neither of which the bech32 charset contains.
#
# Plus three beyond it: B7 middle dot, 2022 bullet and 2026 ellipsis. The
# ellipsis is load bearing rather than decorative, because wt_addr_short elides
# the middle of an address with it. Without the glyph that elision draws LVGL's
# placeholder box in the middle of the one line on the Sign screen the owner is
# asked to compare against their coordinator.
MONO=vendor/IoskeleyMono-Medium-ascii.ttf
[ -f "$MONO" ] || { echo "Ioskeley Mono missing (need tools/fonts/$MONO)"; exit 1; }

for SZ in 14 23 28; do
  echo "== font_kiss_mono$SZ"
  conv --size $SZ --font "$MONO" -r 0x20-0x7E -r 0xB7 -r 0x2022 -r 0x2026 \
    -o "$OUT/font_kiss_mono$SZ.c"
done

# The two values big enough to be read across a room: the Sign hero amount and
# the wallet fingerprint on the write-it-down screen. Nineteen glyphs, digits
# and A to F and space and full stop, which is exactly a decimal amount or an
# uppercase hex fingerprint and nothing else.
#
# It carries no lowercase and no letter outside A to F, so it still cannot
# render a word in any language. A translated string pointed here would come
# out as a row of placeholder boxes, which is the loud failure these faces are
# built to produce rather than a quiet one.
echo "== font_kiss_num48"
conv --size 48 --font "$MONO" -r 0x20 -r 0x2E -r 0x30-0x39 -r 0x41-0x46 \
  -o "$OUT/font_kiss_num48.c"

# lv_font_conv emits an extra blank line; normalize generated sources so
# regeneration stays clean under git diff --check.
perl -0pi -e 's/\n+\z/\n/' "$OUT"/font_kiss_*.c

ls -la "$OUT"/font_kiss_*.c
echo "fonts generated"
