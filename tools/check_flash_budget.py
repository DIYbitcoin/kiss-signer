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
import os
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


# The two things this gate can get wrong are both about WHICH partition it
# measures against, not about arithmetic. It must take the SMALLEST app slot,
# because with two OTA slots an image that fits ota_0 and not ota_1 installs
# once and can then never be replaced -- and it must read the sizes in every
# notation partitions.csv is allowed to use, since a hex cap misread as decimal
# is a gate that passes everything.
def selftest():
    import tempfile
    bad = 0

    def cap_of(csv_text):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write(csv_text)
            path = fh.name
        try:
            return app_cap(path)
        finally:
            os.unlink(path)

    two_slots = (
        "# name, type, subtype, offset, size\n"
        "nvs,      data, nvs,     0x9000,  0x6000\n"
        "ota_0,    app,  ota_0,   0x20000, 0x7F0000\n"
        "ota_1,    app,  ota_1,   ,        0x7E0000\n")
    cap, name = cap_of(two_slots)
    ok = (cap, name) == (0x7E0000, "ota_1")
    print("  %-52s %s (%s)" % ("the SMALLER of two OTA slots wins",
                               "ok" if ok else "FAILED", name))
    bad += not ok

    cap, _ = cap_of("factory, app, factory, 0x10000, 4M\n")
    ok = cap == 4 * 1024 * 1024
    print("  %-52s %s (%d)" % ("an M suffix is megabytes",
                               "ok" if ok else "FAILED", cap))
    bad += not ok

    cap, _ = cap_of("factory, app, factory, 0x10000, 3072K\n")
    ok = cap == 3072 * 1024
    print("  %-52s %s (%d)" % ("a K suffix is kilobytes",
                               "ok" if ok else "FAILED", cap))
    bad += not ok

    # A data partition is not somewhere an app can live, and counting one would
    # measure the image against a slot it is never written to.
    cap, name = cap_of("storage, data, fat, 0x10000, 0x100000\n"
                       "factory, app,  factory, , 0x200000\n")
    ok = (cap, name) == (0x200000, "factory")
    print("  %-52s %s (%s)" % ("a data partition is not an app slot",
                               "ok" if ok else "FAILED", name))
    bad += not ok

    print("flash budget selftest: 4 cases, %d broken" % bad)
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        sys.exit(selftest())
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
