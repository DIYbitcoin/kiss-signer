# Vendored fonts

Pinning each file and its checksum keeps font generation reproducible.

## SourceHanSansJP-Normal.otf

The Japanese regional subset of Adobe Source Han Sans. It is used only to
generate the Japanese LVGL glyph subsets; Korean and Simplified Chinese
continue to use the Source Han Sans SC font shipped with LVGL.

- Upstream: <https://github.com/adobe-fonts/source-han-sans>
- Commit: `a4f7cf94edfb9d7ffbdfc4841de276358bd7e0f2`
- Source: `SubsetOTF/JP/SourceHanSansJP-Normal.otf`
- SHA-256: `5120e2d88d761c405c706e9b3dcbcb89fb888e41adfa5a1d170c146401f8a795`
- License: SIL Open Font License 1.1, in `LICENSE-SourceHanSans.txt`

Pinning prevents Japanese text from inheriting Simplified Chinese regional
glyph forms.

## IoskeleyMono-Medium-ascii.ttf

An Iosevka configuration by ahatem, the same face the website already serves.
It is the source for the four fixed pitch wallet faces: `font_kiss_mono14`,
`font_kiss_mono23`, `font_kiss_mono28` and `font_kiss_num48`.

- Upstream: <https://github.com/ahatem/IoskeleyMono>
- License: SIL Open Font License 1.1, in `LICENSE-IoskeleyMono.txt`
- SHA-256 of this file: `0512878a6c381240d59f4fb29043b0b205e2eccaafd34351c9dc55b50d153b2a`

**This is a subset, not the upstream file.** Upstream ships a Nerd Font build
carrying several thousand icon glyphs nothing here uses, at 4.6 MB. Everything
this repo generates lives in printable ASCII, so the vendored copy is cut to
`U+0020-007E` plus three characters beyond it: 92 KB, about fifty
times smaller.

The three are `U+00B7` middle dot, `U+2022` bullet and `U+2026` ellipsis.
The ellipsis is not decoration: `wt_addr_short` elides the middle of an address
with it, so without the glyph that line draws a placeholder box in the middle of
the address the owner is comparing.

The cut is provably free. All four generated `main/font_kiss_*.c` are byte
identical before and after, apart from the `Opts:` line that records the source
filename. Hinting, kerning and every glyph outline in range are preserved,
which is why `--layout-features='*'` is not optional below: `gen_fonts.sh`
passes `--force-fast-kern-format`, so dropping GPOS would change the output.

### Reproducing it

    pyftsubset IoskeleyMonoNerdFont-Medium.ttf \
      --output-file=IoskeleyMono-Medium-ascii.ttf \
      --unicodes="U+0020-007E,U+00B7,U+2022,U+2026" \
      --layout-features='*' --glyph-names --notdef-outline \
      --name-IDs='*' --recommended-glyphs

Source file for that command was the Nerd Font Medium build,
SHA-256 `7ddc23a37c793078b6f084cef5f9c2d246c9d18e9656c7d3bde74ff21a610026`.
The plain, non Nerd Font build of the same release has identical outlines in
this range and would subset to the same result.

**If you ever need a glyph outside the ranges above, you must re-subset from
upstream.** This file cannot give you one, and the failure is a placeholder box
rather than an error. That has already happened once: the first cut was ASCII
only and the ellipsis in `wt_addr_short` came out as a box.

### Why fixed pitch, and only for these four

Addresses, fingerprints, derivation paths and amounts are never translated, so
these faces are Latin only and have no CJK variant by design. They must never
be handed a localised string. A tabular figure also keeps an amount the same
width on every screen it appears on, which is what stops digits shifting column
between Sign, Receive and Verify.

Note this file is a build time input only. It is not linked, not flashed, and
has no effect on the firmware image or on anything the device does at runtime.
