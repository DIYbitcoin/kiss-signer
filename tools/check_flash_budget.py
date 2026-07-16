#!/usr/bin/env python3
"""Flash-budget gate: fail when the app binary nears its factory partition.

    check_flash_budget.py <app.bin> <partitions.csv> [max_pct]

The factory partition (6MB) is the project's real wall: baked full-screen art
is ~75% of the binary and one more RGB565 screen is ~768KB, so the build that
no longer fits arrives suddenly. Gate at max_pct (default 95) so a release
fails while there is still headroom to react (compress baked art into PSRAM-
decoded assets) instead of on the flash step of a funded board.
"""
import sys


def psize(tok):
    tok = tok.strip()
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    if tok[-1] in "KkMm":
        return int(tok[:-1]) * (1024 if tok[-1] in "Kk" else 1024 * 1024)
    return int(tok)


def factory_cap(csv_path):
    for line in open(csv_path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [t.strip() for t in line.split(",")]
        if len(f) >= 5 and f[0] == "factory" and f[1] == "app":
            return psize(f[4])
    sys.exit(f"FAIL: no factory app partition in {csv_path}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    app_bin, csv_path = sys.argv[1], sys.argv[2]
    max_pct = int(sys.argv[3]) if len(sys.argv) > 3 else 95
    size = len(open(app_bin, "rb").read())
    cap = factory_cap(csv_path)
    pct = size * 100.0 / cap
    line = (f"app {size} B = {pct:.1f}% of the {cap} B factory partition "
            f"(gate {max_pct}%, {cap * max_pct // 100 - size:+} B to the gate)")
    if size > cap * max_pct // 100:
        print("FAIL: flash budget: " + line)
        sys.exit(1)
    print("PASS: flash budget: " + line)


if __name__ == "__main__":
    main()
