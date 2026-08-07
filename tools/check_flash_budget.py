#!/usr/bin/env python3
"""Flash-budget gate: fail when the app binary nears the partition it lives in.

    check_flash_budget.py <app.bin> <partitions.csv> [max_pct]

Baked full-screen art is ~75% of the binary and one more RGB565 screen is
~768KB, so the build that no longer fits arrives suddenly. Gate at max_pct
(default 95) so a release fails while there is still headroom to react
(compress baked art into PSRAM-decoded assets) instead of on the flash step of
a funded board.

The wall moved when SD firmware updates split the single 12MB factory partition
into two 7.94MiB OTA slots. That is the point of the gate, not a problem with
it: the app has to fit the slot it will be WRITTEN to, and a slot is now barely
7% larger than the image. The reserve, when it runs out, is the game art in
menu_img.c/gameover_img.c/sprites.c.

Both layouts are accepted, because the encrypted lane and any older table still
carry `factory`. With OTA slots the SMALLEST one decides: an image that fits
ota_0 and not ota_1 is an image that installs once and then cannot be replaced.
"""
import sys


def psize(tok):
    tok = tok.strip()
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    if tok[-1] in "KkMm":
        return int(tok[:-1]) * (1024 if tok[-1] in "Kk" else 1024 * 1024)
    return int(tok)


def app_cap(csv_path):
    """(bytes, name) of the app partition an image has to fit."""
    slots = []
    for line in open(csv_path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [t.strip() for t in line.split(",")]
        if len(f) >= 5 and f[1] == "app" and f[4]:
            slots.append((psize(f[4]), f[0]))
    if not slots:
        sys.exit(f"FAIL: no app partition in {csv_path}")
    # Smallest wins. With one factory partition this is that partition; with
    # two OTA slots it is the tighter of the pair.
    return min(slots)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    app_bin, csv_path = sys.argv[1], sys.argv[2]
    max_pct = int(sys.argv[3]) if len(sys.argv) > 3 else 95
    size = len(open(app_bin, "rb").read())
    cap, name = app_cap(csv_path)
    pct = size * 100.0 / cap
    line = (f"app {size} B = {pct:.1f}% of the {cap} B {name} partition "
            f"(gate {max_pct}%, {cap * max_pct // 100 - size:+} B to the gate)")
    if size > cap * max_pct // 100:
        print("FAIL: flash budget: " + line)
        sys.exit(1)
    print("PASS: flash budget: " + line)


if __name__ == "__main__":
    main()
