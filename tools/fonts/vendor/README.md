# Vendored Japanese font

`SourceHanSansJP-Normal.otf` is the Japanese regional subset of Adobe Source
Han Sans. It is used only to generate the Japanese LVGL glyph subsets; Korean
and Simplified Chinese continue to use the Source Han Sans SC font shipped with
LVGL.

- Upstream: <https://github.com/adobe-fonts/source-han-sans>
- Commit: `a4f7cf94edfb9d7ffbdfc4841de276358bd7e0f2`
- Source: `SubsetOTF/JP/SourceHanSansJP-Normal.otf`
- SHA-256: `5120e2d88d761c405c706e9b3dcbcb89fb888e41adfa5a1d170c146401f8a795`
- License: SIL Open Font License 1.1, in `LICENSE-SourceHanSans.txt`

Pinning the file and checksum keeps font generation reproducible and prevents
Japanese text from inheriting Simplified Chinese regional glyph forms.
