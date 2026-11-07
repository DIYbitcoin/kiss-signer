#!/usr/bin/env python3
"""Read the export QR codes the simulator drew, with a decoder that is not ours.

Every other QR check in the tree reads codes with k_quirc, the reader that
ships in the signer, or passes the text straight across without drawing it.
A fault shared by our encoder and our reader (a bad mask, a lost quiet zone,
a module rounded the wrong way at one board's size) would pass all of them
and still leave a phone unable to scan the signed transaction.

The walk saves each export QR stop as sim_qr_out*.ppm and writes the text
that QR card was given beside it as sim_qr_out*.qr.txt, one line per visible
card (sim/sim_main.c save, wt_sim_qr_texts in main/kiss_theme.c). This reads
each frame with zxing-cpp and fails unless what it decodes is exactly that
text: a frame that decodes to nothing fails, and so does one that decodes to
anything else.

The stops are taken from the save() literals in the walk's source, so a new
export QR stop is checked without being listed here.

    bash sim/build_sim.sh && /tmp/fruitsim
    python3 tools/check_sim_qr.py --selftest
    python3 tools/check_sim_qr.py
"""

import os
import re
import sys

import zxingcpp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "sim", "sim_main.c")
SRC = os.environ.get("KISS_SIM_TMP") or "/tmp"
STOP = re.compile(r'save\("/tmp/(sim_qr_out[a-z0-9_]*)\.ppm"\)')


def read_ppm(path):
    """The walk's own P6 layout: magic, width height, 255, then RGB."""
    with open(path, "rb") as f:
        data = f.read()
    head = data.split(b"\n", 3)
    if len(head) < 4 or head[0] != b"P6" or head[2] != b"255":
        raise ValueError("not a walk frame")
    w, h = (int(v) for v in head[1].split())
    px = head[3]
    if len(px) != w * h * 3:
        raise ValueError("pixel data is %d bytes, want %d" % (len(px), w * h * 3))
    return w, h, px


def decode(w, h, px):
    img = memoryview(px).cast("B", (h, w, 3))
    hits = zxingcpp.read_barcodes(img, formats=zxingcpp.BarcodeFormat.QRCode)
    return sorted(b.bytes for b in hits if b.valid)


def check(w, h, px, want):
    """None when the frame reads back as exactly `want`, else why not."""
    got = decode(w, h, px)
    if not got:
        return "no QR code decoded"
    if got != sorted(want):
        return "decoded %r, expected %r" % (got, sorted(want))
    return None


def expected(path):
    with open(path, "rb") as f:
        want = [l for l in f.read().split(b"\n") if l]
    for t in want:
        if not t.lower().startswith(b"ur:"):
            raise ValueError("%r is not a UR part" % t[:40])
    return want


def stops():
    with open(SIM) as f:
        names = STOP.findall(f.read())
    if not names:
        sys.exit("FAIL: no sim_qr_out save() found in %s" % SIM)
    return names


def bbox(w, h, px):
    """The decoded code's corners, as an axis-aligned box in pixels."""
    img = memoryview(px).cast("B", (h, w, 3))
    hits = zxingcpp.read_barcodes(img, formats=zxingcpp.BarcodeFormat.QRCode)
    p = hits[0].position
    pts = (p.top_left, p.top_right, p.bottom_left, p.bottom_right)
    return (min(q.x for q in pts), min(q.y for q in pts),
            max(q.x for q in pts), max(q.y for q in pts))


def selftest():
    """The check must be able to say no. Take the first stop's real frame and
    crop it through the middle of the code, and separately paint the quiet
    zone around the code dark; both have to fail. The untouched frame has to
    pass first, or the other two prove nothing."""
    name = stops()[0]
    w, h, px = read_ppm(os.path.join(SRC, name + ".ppm"))
    want = expected(os.path.join(SRC, name + ".qr.txt"))
    if check(w, h, px, want):
        sys.exit("FAIL: selftest frame %s does not pass untouched" % name)
    x0, y0, x1, y1 = bbox(w, h, px)

    # Every row cut at the code's centre line.
    cut = (x0 + x1) // 2
    cropped = b"".join(px[y * w * 3:(y * w + cut) * 3] for y in range(h))
    if check(cut, h, cropped, want) is None:
        sys.exit("FAIL: selftest crop of %s still passed" % name)

    # A dark ring in the white margin, touching the code, the modules left as
    # drawn: the code with no quiet zone. Flush on purpose: zxing still reads
    # a code with one pixel of white left between the ring and the modules.
    ring = max(4, (x1 - x0) // 12)
    dark = bytearray(px)
    for y in range(max(0, y0 - ring), min(h, y1 + ring + 1)):
        for x in range(max(0, x0 - ring), min(w, x1 + ring + 1)):
            if x0 <= x <= x1 and y0 <= y <= y1:
                continue
            dark[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\0\0\0"
    if check(w, h, bytes(dark), want) is None:
        sys.exit("FAIL: selftest quiet zone ring on %s still passed" % name)
    print("selftest ok: crop and quiet zone ring on %s both fail" % name)


def main():
    if "--selftest" in sys.argv[1:]:
        selftest()
        return
    fails = 0
    names = stops()
    for name in names:
        frame = os.path.join(SRC, name + ".ppm")
        side = os.path.join(SRC, name + ".qr.txt")
        try:
            w, h, px = read_ppm(frame)
            want = expected(side)
            why = check(w, h, px, want) if want else "no QR text beside the frame"
        except (OSError, ValueError) as e:
            why = str(e)
        if why:
            print("FAIL: %s: %s" % (name, why))
            fails += 1
        else:
            print("ok: %s reads as %d UR part(s)" % (name, len(want)))
    if fails:
        sys.exit("%d of %d export QR frame(s) failed" % (fails, len(names)))
    print("%d export QR frame(s) read back by zxing-cpp" % len(names))


if __name__ == "__main__":
    main()
