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
# missing from the font hard-hangs LVGL's renderer (device included).
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
# LVGL has no symbol macro for. They are named in wallet_theme.h as WT_ICON_*
# and must stay in lockstep with it: a codepoint referenced by a label but
# missing from the font does not draw a tofu box, it hard-hangs the renderer.
#   61481 F029 qrcode    61572 F084 key    61979 F21B user-secret
# (63426 F7C2 sd-card was already here as LV_SYMBOL_SD_CARD.)
# Only the Latin faces need these: every locale's font is a Latin base with the
# CJK face as its FALLBACK, so an icon resolves in the base whatever the language.
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61481,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61572,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,61979,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

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
# THE FALLBACK IS LOAD-BEARING. A glyph missing from an LVGL font is not a tofu
# box, it is an infinite loop in the renderer -- on device as well as in the
# sim. There is no font_kiss_ja34 to chain to, so this chains to the 28 CJK
# faces: a stray CJK character in a Latin-locale title renders one size small
# instead of hanging the device. wt_font34() must ALSO refuse to hand this face
# to a CJK locale in the first place; this chain is the second line of defence,
# not the first.
echo "== font_kiss_lat34"
conv --size 34 \
  --font "$LVF/Montserrat-Medium.ttf" -r "$LAT" \
  --font "$LVF/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
  --lv-fallback font_kiss_ja28 \
  -o "$OUT/font_kiss_lat34.c"

# lv_font_conv emits an extra blank line; normalize generated sources so
# regeneration stays clean under git diff --check.
perl -0pi -e 's/\n+\z/\n/' "$OUT"/font_kiss_*.c

ls -la "$OUT"/font_kiss_*.c
echo "fonts generated"
