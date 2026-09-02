#!/usr/bin/env python3
"""Which of the walk's frames changed, as one page you can actually look at.

Why this exists
---------------
Every gate in this repo is defined by what it EXCUSES, so a clean sweep says
nothing about the class nobody thought to check. Four defects in one session
were invisible by construction -- a body set at font14 under 165px of empty
glass, an output column where half the labels stood down for a hold and half
did not, an accent that survived a theme change, two flags sharing a bit --
and three of the four were caught by a person looking at a rendered frame.

The walk writes about 400 frames per run and nothing compares them across
runs. check_sim_taps compares NEIGHBOURS inside one run, which answers a
different question: did this tap do anything. This answers "what did my edit
change", which is the question the house rule about shipping a picture is
really asking, and it narrows 400 frames to the handful worth opening.

It is not a gate. It never fails, it excuses nothing, and it has no backlog.
It renders a page and gets out of the way -- which is why it is not called
check_*.py, and why tools/check_gates.py does not count it.

    bash sim/build_sim.sh && SIM_LANG=en /tmp/fruitsim
    python3 tools/contact_sheet.py              # what changed since the last run
    python3 tools/contact_sheet.py --all        # every frame
    python3 tools/contact_sheet.py --update     # this run becomes the baseline

The baseline is a directory beside the frames, not a file in git: every UI
commit would touch a committed manifest, three sessions would conflict over
it constantly, and the question worth answering is "since I last ran the
walk", not "since the last commit".
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

# Halved, not quartered. The cells are sized by the CSS grid either way, so
# this is sharpness and file size -- and at 200x120 the thing a contact sheet
# exists to catch, type that came out a rung too small, was itself too small
# to see. 400x240 in three columns is legible at a glance on a laptop.
CELL_DIV = 2          # 800x480 -> 400x240
COLS     = 3


def frames_dir(explicit):
    if explicit:
        return Path(explicit)
    return Path(os.environ.get("KISS_SIM_TMP", "/tmp"))


def read_ppm(path):
    """(w, h, bytes) for a binary P6, or None for anything else."""
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P6"):
        return None
    # header: P6 <ws> w <ws> h <ws> maxval <single ws> pixels
    fields, i = [], 2
    while len(fields) < 3 and i < len(data):
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _maxval = fields
    return w, h, data[i:i + w * h * 3]


def shrink(w, h, px, div):
    """Nearest neighbour, because this is a thumbnail to spot things in, not a
    reproduction -- and a box filter would blur away the very thing being
    looked for, which is text that came out too small."""
    ow, oh = w // div, h // div
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        row = (y * div) * w * 3
        o = y * ow * 3
        for x in range(ow):
            s = row + (x * div) * 3
            out[o:o + 3] = px[s:s + 3]
            o += 3
    return ow, oh, bytes(out)


def png(w, h, px):
    """A minimal RGB8 PNG. zlib is stdlib and a filter byte of 0 per row is a
    legal encoding, so this needs nothing installed -- which is the point: a
    tool that has to be pip installed is a tool nobody runs."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += px[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
            chunk(b"IEND", b""))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


HTML_HEAD = """<!doctype html><meta charset=utf-8><title>%s</title>
<style>
 body{background:#0b0f17;color:#c8d2e0;font:14px/1.4 ui-monospace,Menlo,monospace;margin:24px}
 h1{font-size:16px;font-weight:600;letter-spacing:2px;margin:0 0 4px}
 p.sub{color:#7a869c;margin:0 0 20px}
 .grid{display:grid;grid-template-columns:repeat(%d,1fr);gap:18px}
 figure{margin:0}
 img{width:100%%;display:block;border:1px solid #1a2130;image-rendering:pixelated}
 figcaption{color:#9aa7bb;margin-top:6px;word-break:break-all}
 .new{color:#49d27b}
 .gone{color:#e2554f}
</style>
<h1>%s</h1><p class=sub>%s</p><div class=grid>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", help="where the walk wrote its frames")
    ap.add_argument("--baseline", help="the run to compare against")
    ap.add_argument("--all", action="store_true", help="every frame, not the diff")
    ap.add_argument("--out", help="where to write the page")
    ap.add_argument("--update", action="store_true",
                    help="make this run the baseline and write nothing else")
    a = ap.parse_args()

    src = frames_dir(a.dir)
    base = Path(a.baseline) if a.baseline else src / ".frames-prev"
    out = Path(a.out) if a.out else src / "contact.html"

    now = sorted(p for p in src.glob("sim_*.ppm"))
    if not now:
        print("no frames in %s -- run the walk first:\n"
              "    bash sim/build_sim.sh && SIM_LANG=en %s/fruitsim"
              % (src, src), file=sys.stderr)
        return 1

    if a.update:
        if base.exists():
            shutil.rmtree(base)
        base.mkdir(parents=True)
        for p in now:
            shutil.copy2(p, base / p.name)
        print("baseline: %d frames -> %s" % (len(now), base))
        return 0

    have_base = base.is_dir()
    old = {p.name: p for p in base.glob("sim_*.ppm")} if have_base else {}

    changed, added, gone = [], [], []
    for p in now:
        if not have_base:
            continue
        q = old.get(p.name)
        if q is None:
            added.append(p)
        elif digest(p) != digest(q):
            changed.append(p)
    gone = sorted(n for n in old if not (src / n).exists())

    if a.all or not have_base:
        show = [(p, "") for p in now]
        why = ("every frame (no baseline yet -- "
               "run with --update to record one)" if not have_base
               else "every frame")
    else:
        show = [(p, "new") for p in added] + [(p, "") for p in changed]
        why = "%d changed, %d new, %d gone, of %d" % (
            len(changed), len(added), len(gone), len(now))

    if not show:
        print("contact sheet: nothing changed since the baseline (%d frames)"
              % len(now))
        for n in gone:
            print("  gone: %s" % n)
        return 0

    title = "KISS frames"
    body = [HTML_HEAD % (title, COLS, title, why)]
    import base64
    for p, kind in sorted(show, key=lambda t: t[0].name):
        img = read_ppm(p)
        if not img:
            continue
        w, h, px = shrink(*img, CELL_DIV)
        b64 = base64.b64encode(png(w, h, px)).decode()
        cls = " class=new" if kind == "new" else ""
        body.append('<figure><img src="data:image/png;base64,%s">'
                    '<figcaption%s>%s%s</figcaption></figure>\n'
                    % (b64, cls, p.name[4:-4], " (new)" if kind else ""))
    for n in gone:
        body.append('<figure><figcaption class=gone>%s (gone)</figcaption>'
                    '</figure>\n' % n[4:-4])
    body.append("</div>\n")
    out.write_text("".join(body), encoding="utf-8")

    print("contact sheet: %s" % why)
    for p, kind in sorted(show, key=lambda t: t[0].name):
        print("  %s%s" % (p.name[4:-4], " (new)" if kind else ""))
    for n in gone:
        print("  %s (gone)" % n[4:-4])
    print("  -> %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
