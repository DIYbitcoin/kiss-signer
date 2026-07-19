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
# the LV_SYMBOL_* codepoints, verbatim from LVGL's built_in_font_gen.py
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

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

# 23px: wallet-home tile titles only (matches the baked 23px typography).
# CJK subsets carry just the tile-title glyphs (glyphs_tile_*.txt); no
# FontAwesome plane (titles never use symbols).
echo "== font_kiss_lat23"
conv --size 23 \
  --font "$LVF/Montserrat-Medium.ttf" -r "$LAT" \
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
    --font "$FONT" --symbols "$(cat glyphs_tile_$L.txt)" \
    $FB -o "$OUT/font_kiss_${L}23.c"
done

# lv_font_conv emits an extra blank line; normalize generated sources so
# regeneration stays clean under git diff --check.
perl -0pi -e 's/\n+\z/\n/' "$OUT"/font_kiss_*.c

ls -la "$OUT"/font_kiss_*.c
echo "fonts generated"
