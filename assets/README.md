# assets/ — source art + generators

The firmware's image sources (`main/sprites.c`, `main/menu_img.c`, `main/gameover_img.c`) are
**generated**, not hand-edited. This directory holds the generators and their source PNGs so the
build is reproducible.

## Regenerate
From the repo root, with the image venv on PATH (numpy + pillow):

```
/tmp/spritevenv/bin/python assets/generators/convert_fruit.py    # -> main/sprites.{c,h}
/tmp/spritevenv/bin/python assets/generators/menu_mock.py        # -> main/menu_img.{c,h}  (+ /tmp/menu_mock.png preview)
/tmp/spritevenv/bin/python assets/generators/gameover_mock.py    # -> main/gameover_img.{c,h} (+ /tmp/gameover_mock.png preview)
```

Each generator resolves paths relative to its own location, so it works from any checkout.
Always eyeball the `/tmp/*_mock.png` previews (and `/tmp/newfruit_sheet.png`) before flashing.

## What's here
- `generators/convert_fruit.py` — fruit/bomb/heart/effect sprites (RGB565A8). Uses `fruit-pack/`
  whole fruit + procedural apple/pineapple cross-sections; `emoji/bomb.png` + `emoji/collision.png`.
- `generators/menu_mock.py` — baked 480x800 menu scene (RGB565). Uses `emoji/` fruit accents.
- `generators/gameover_mock.py` — baked game-over scene + NEW BEST ribbon. Uses `emoji/` accents.
- `fruit-pack/Items/` — the 10 CC0 pack PNGs actually referenced (of 43).
- `emoji/` — the 6 emoji PNGs actually referenced.

## Licensing
> The repo rule is **CC0 / CC-BY / MIT only**. Track every source here.

- **`emoji/`** — the **only fruit art now in use.** Every whole fruit (watermelon, red_apple,
  tangerine, pineapple, strawberry, cherries, grapes) plus bomb/collision comes from this one
  256x256 3D emoji set; the cut faces are procedural cross-sections in `convert_fruit.py`.
  ✅ **PROVENANCE VERIFIED: Microsoft Fluent Emoji (3D), MIT.** All 9 PNGs are byte-for-byte
  identical (SHA-256) to the upstream files at github.com/microsoft/fluentui-emoji
  (`assets/<Name>/3D/<name>_3d.png`). License text vendored at `emoji/LICENSE-FluentEmoji.txt`.

- **`fruit-pack/`** — the OpenGameArt CC0 "Sprites Fruits" pack. **No longer referenced** by any
  generator (fruit moved to the emoji set for a consistent look). Kept for reference / as a
  CC0 fallback; safe to delete. Source: https://opengameart.org/content/sprites-fruits

## Host font dependency (not vendored)
The menu/game-over generators render text with **Arial Rounded Bold**
(`/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf`), a proprietary macOS system font —
intentionally **not** vendored. On a machine without it, the generators will fail at
`ImageFont.truetype`; substitute a free rounded face (e.g. Baloo 2 / Fredoka / Nunito, all OFL) and
expect slightly different letterforms.
