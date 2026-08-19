#!/usr/bin/env python3
"""Turn simulator frames into the documentation screenshots.

Every picture in docs/shots/ is a real frame the simulator rendered, not a
photo of a screen and not something exported by hand. That matters twice over:

  * a hand-captured screenshot goes stale the moment a layout moves, and
    nobody notices until someone follows a guide that no longer matches the
    firmware;
  * the walkthrough doubles as a review surface. If a frame in here looks
    wrong, the screen IS wrong.

SECTIONS below is the single source for both the images and the prose, so
docs/walkthrough.md cannot drift from what the pictures actually show.

Usage:
    bash tools/gen_docs_shots.sh          # build sim, run it, write everything
    python3 tools/gen_docs_shots.py       # frames already in /tmp
    python3 tools/gen_docs_shots.py --check   # manifest vs sim_main.c, no output

--check is what CI runs. It does not compare pixels: two zlib versions can
encode the same image to different bytes, and a flaky docs job teaches people
to ignore the docs job. It checks the thing that actually rots, which is a
save() being renamed or deleted out from under the manifest.

PNG is written here rather than with Pillow on purpose: the frames are plain
8-bit RGB, the encoder that needs is thirty lines, and CI plus a fresh clone
then need no image library at all.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "/tmp"
OUT = os.path.join(ROOT, "docs", "shots")
MD = os.path.join(ROOT, "docs", "walkthrough.md")
SIM = os.path.join(ROOT, "sim", "sim_main.c")
REVIEW = os.path.join(ROOT, "docs", "review")

# (heading, intro, [(output name, sim frame, caption)])
SECTIONS = [
    ("Returning from Fruit Island",
     "The wallet has no icon and no launcher. You get back to it by drawing "
     "the letters K, I, S, S on the game menu with a fingertip. Nothing on "
     "the screen invites you to, and a wrong gesture does nothing at all.",
     [
         ("01-island-menu", "sim_menu_back",
          "This is everything an onlooker sees: a fruit game. No wallet "
          "button, no lock icon, no hint that anything else is installed."),
         ("02-kiss-login", "sim_login",
          "Draw K, I, S, S anywhere on the menu. On a signer with no spare "
          "set up, that opens the passphrase login. Once a spare exists, the "
          "same four letters open that instead, and your own swipe after them "
          "is what asks for the passphrase."),
         ("03-wallet-home", "sim_wallet",
          "Your recovery words and your exact passphrase together make this "
          "wallet. A different passphrase silently opens a different wallet, "
          "so check the fingerprint is the one you expect.\n\n"
          "This only works if you wrote the fingerprint down. Do it once, on "
          "the same piece of paper as your recovery words: the eight character "
          "code on this screen. Every passphrase is valid, so a typo never "
          "shows an error, it just opens a different and empty wallet. If the "
          "code here ever differs from your paper, you typed the passphrase "
          "wrong. Lock and try again."),
     ]),

    ("Choosing where the recovery words live",
     "Storage is chosen while a wallet is created or restored, and it can be "
     "changed later from an unlocked wallet. All three modes are offered on "
     "every build; the passphrase, never stored here, is what guards the real "
     "wallet whichever mode holds the words.",
     [
         ("03a-setup-storage", "sim_setup_storage",
          "Setup asks what remains after you power off, before the recovery "
          "words are committed. FLASH keeps the words on this device (they open "
          "the decoy if it is taken); AMNESIC keeps nothing; SD CARD seals the "
          "words to a card only this signer can open."),
         ("03b-settings-storage", "sim_storage_choose",
          "SETTINGS → STORAGE shows the current mode and the same three "
          "choices. Moving between modes requires a deliberate hold "
          "and verifies the destination before removing the source."),
         # SETTINGS itself, which the walkthrough sent people to twice without
         # ever showing. The simulator has captured this frame all along.
         ("03c-settings", "sim_settings",
          "SETTINGS itself, the page both of those are reached from. Storage "
          "and address type read their current value on the row that opens "
          "them, so nothing here has to be opened to be checked."),
     ]),

    # Kept before pairing/receiving/signing: those are done from whichever
    # signer the stroke opened.
    ("Two ways in: the spare wallet",
     "A passphrase field on screen is itself a tell. It proves there is "
     "something to leave out of it, and you can never show that the "
     "passphrase you gave was the last one. So the obvious gesture opens a "
     "real, working wallet that asks for nothing, and the passphrase lives "
     "behind one extra stroke.",
     [
         ("02a-duress-intro", "sim_duress_intro",
          "Drawing KISS on its own opens a spare wallet: the same recovery "
          "words with no passphrase. It has its own fingerprint, pairs with a "
          "coordinator and signs, because a wallet that cannot do those "
          "things is not a story anyone would believe."),
         ("02b-duress-fund", "sim_duress_fund",
          "Put a small amount in it. An empty wallet on a signer looks "
          "exactly like a wallet with something hidden behind it."),
         ("02c-duress-pick", "sim_duress_pick",
          "One extra swipe after your drawing asks for your passphrase, and "
          "the passphrase opens your real signer. You choose which swipe is "
          "yours, so reading this firmware tells nobody what to draw."),
         ("02d-duress-draw", "sim_duress_draw_again",
          "Draw it over the printed word, twice, before anything is saved. "
          "Any other swipe opens the spare, exactly as no swipe does, so a "
          "wrong guess looks like a device with nothing behind it."),
     ]),

    ("Pairing with Sparrow",
     "Pairing hands Sparrow a watch-only map of the wallet so it can find "
     "your addresses and build transactions. It never hands over anything "
     "that can spend.",
     [
         ("04-wallet-facts", "sim_winfo",
          "WALLET carries the facts you check against the coordinator: "
          "fingerprint, network, address type and the first address."),
         ("05-pair-sparrow", "sim_pair",
          "PAIR COORDINATOR with DESKTOP selected shows the descriptor "
          "Sparrow reads. Scan it with Sparrow's webcam, or export to SD. "
          "Tap the + beside any QR to enlarge it."),
         ("06-pair-explained", "sim_pair_help",
          "The \"?\" spells out what the coordinator can and cannot do with "
          "what you just handed it."),
     ]),

    ("Verifying the first receive address",
     "Do this once, before any money moves. It is the step that catches a "
     "computer showing you an address that is not yours.",
     [
         ("07-receive", "sim_recv",
          "RECEIVE lists addresses KISS derived on the device. Tap any of "
          "them for its QR. Read them here, on the device, never off the "
          "computer."),
         ("08-verify-match", "sim_vfy_yes",
          "VERIFY re-derives whatever address you type in. Green means KISS "
          "found it in this wallet, independently of whatever displayed it."),
         ("09-verify-wrong-net", "sim_vfy_wrong_net",
          "A well-formed address from the wrong network fails red. So does "
          "an address that is simply not one of yours."),
     ]),

    ("Receiving a tiny test payment",
     "Send yourself an amount you would not mind losing, and confirm it "
     "arrives, before the wallet holds anything real.",
     [
         ("10-receive-first", "sim_recv1",
          "Page to the address you want paying. The counter says which "
          "address you are looking at. Tap its QR for a full-screen scan view."),
         ("11-receive-reuse", "sim_recv_reminder",
          "Every address keeps the standing privacy reminder in view: use a "
          "new one for each payment, because reuse links payments in public."),
     ]),

    # The order of these five is the safety argument: see what you are
    # agreeing to, then hold, then carry the signature back yourself.
    ("Signing and broadcasting a tiny transaction",
     "Sparrow builds the transaction and Sparrow sends it. KISS only signs, "
     "and it is never online to broadcast anything itself.",
     [
         ("12-sign-choose", "sim_sign_files",
          "Signing always starts from an unsigned transaction Sparrow made, "
          "over SD card or QR. KISS never builds one itself."),
         ("13-sign-verify", "sim_sign_verify",
          "What you are actually agreeing to: who gets paid, the network fee, "
          "and the total leaving the wallet. Money coming back to you is "
          "labelled change only when KISS re-derived that address itself."),
         ("14-sign-hold", "sim_sign_hold",
          "Signing takes a deliberate hold, not a tap, so no single stray "
          "touch can ever sign anything."),
         ("15-sign-done", "sim_sign_done",
          "Signed. The signed file or QR has to go back to Sparrow, and "
          "Sparrow broadcasts it. Nothing has been sent yet at this point."),
         ("16-sign-qr-out", "sim_qr_out1",
          "Handing the signature back by animated QR when there is no SD "
          "card. Point Sparrow's webcam at it and let it run; tap the QR if "
          "the camera needs larger modules."),
     ]),
]


# The pictures README.md and docs/guide.html already used. They were captured
# by hand, which is why several of them drifted a release behind the firmware.
# Mapping them here puts them on the same footing as the walkthrough: one
# command regenerates every screenshot in the repo. The frame chosen for each
# is the one matching the alt text already written at the call site.
# The three unreviewed axes from design_handoff_kiss_signer/ADDENDUM-03. After
# this table exists, `bash tools/gen_docs_shots.sh` writes each set under
# docs/review/<axis>/ so theme, network and CJK regressions can be looked at by
# a person or diffed by CI instead of argued about in prose.
#
# env    what sim run this axis needs, empty when the canonical run captured it
# pfx    the frame prefix save() adds, from SIM_LANG in sim_main.c
# extra  (output name, frame) pairs no canonical target already covers
AXES = {
    "green": dict(
        env={"SIM_ACCENT": "GREEN"}, pfx="", extra=[],
        note="GREEN accent, English, mainnet. ADDENDUM-02's axis, the one "
             "MONO hides because MONO's accent is ink."),
    "testnet": dict(
        env={}, pfx="",
        extra=[("t0-network", "sim_net_choose"),
               ("t0a-signet-settings", "sim_settings_signet"),
               ("t0b-signet-home", "sim_wallet_signet"),
               ("t1-home", "sim_wallet_testnet"),
               ("t2-settings", "sim_settings_tn"),
               ("t3-receive", "sim_recv_tn"),
               ("t4-receive-detail", "sim_recv_detail_tn"),
               ("t5-receive-sp", "sim_recv_sp_tn"),
               ("t6-verify", "sim_verify_tn")],
        note="The test networks, MONO, English. The walk already captures "
             "these near the end and they were published nowhere. SIGNET is a "
             "label: the same keys, the same tb1 addresses, a different chain."),
    "ja": dict(
        env={"SIM_LANG": "ja"}, pfx="ja", extra=[],
        note="Japanese. Every title and primary button drops from wt_font34 "
             "to wt_font28, since the 34 rung has no CJK face by design."),
}


LEGACY = [
    ("docs/readme/menu.png",              "sim_menu"),
    ("docs/readme/wallet.png",            "sim_wallet"),
    ("docs/readme/setup-1-choose.png",    "sim_setup_choose"),
    ("docs/readme/setup-2-words.png",     "sim_setup_words"),
    ("docs/readme/setup-3-quiz.png",      "sim_setup_quiz"),
    ("docs/readme/setup-4-passphrase.png", "sim_setup_pass"),
    ("docs/readme/verify-backup.png",     "sim_verify_ok"),
    ("docs/readme/use-1-receive.png",     "sim_recv"),
    ("docs/readme/use-2-sign.png",        "sim_sign_verify"),
    ("docs/readme/use-3-qr.png",          "sim_qr_out1"),
    ("docs/readme/use-4-sd.png",          "sim_sign_done"),
    ("docs/readme/warn-caution.png",      "sim_sign_combo"),
    # sim_home_fp, not sim_winfo_help: the WALLET screen's "?" chips moved down
    # when that column was re-laid out, so the scripted tap behind
    # sim_winfo_help now misses the chip and saves the unchanged screen. This
    # is the same explainer, reached from the home fingerprint chip instead.
    ("docs/readme/learn-card.png",        "sim_home_fp"),
    ("docs/media/game-menu.png",          "sim_menu"),
    ("docs/media/wallet-home.png",        "sim_wallet"),
    ("docs/media/passphrase-warning.png", "sim_setup_warn"),
    ("docs/media/export-descriptor.png",  "sim_pair"),
    ("docs/media/sign-verify.png",        "sim_sign_verify"),
    ("docs/media/qr-scan.png",            "sim_qr_scan"),
]


def shots():
    for _, _, items in SECTIONS:
        for it in items:
            yield it


def targets():
    """(absolute output path, frame) for everything this script writes."""
    for name, frame, _ in shots():
        yield os.path.join(OUT, name + ".png"), frame
    for rel, frame in LEGACY:
        yield os.path.join(ROOT, rel), frame


def review_targets(axis):
    """(output path, frame) for one axis: the walkthrough set plus its extras.

    Frame names get the SIM_LANG prefix save() adds, so this is what actually
    landed in /tmp after the axis's sim run. `extras` are frames no canonical
    target covers, so testnet does not have to invent duplicate save() calls.
    """
    a = AXES[axis]
    pre = ("sim_%s_" % a["pfx"]) if a["pfx"] else "sim_"

    def px(frame):
        return pre + frame[4:] if frame.startswith("sim_") else frame

    for name, frame, _ in shots():
        yield os.path.join(REVIEW, axis, name + ".png"), px(frame)
    for name, frame in a["extra"]:
        yield os.path.join(REVIEW, axis, name + ".png"), px(frame)


def write_review(axis):
    """One axis set. No GIF, no captions: both belong to the canonical set."""
    want = list(review_targets(axis))
    missing = [f for _, f in want
               if not os.path.exists(os.path.join(SRC, f + ".ppm"))]
    if missing:
        sys.stderr.write("axis %s has no frame for: %s\n"
                         % (axis, ", ".join(sorted(set(missing)))))
        return 1
    total = 0
    for path, frame in want:
        w, h, px = read_ppm(os.path.join(SRC, frame + ".ppm"))
        if (w, h) != (800, 480):
            sys.stderr.write("%s: want 800x480, got %dx%d\n" % (frame, w, h))
            return 1
        os.makedirs(os.path.dirname(path), exist_ok=True)
        total += write_png(path, w, h, px)
    print("docs/review/%-10s <- %2d frames, %5.0f KB"
          % (axis, len(want), total / 1024.0))
    return 0


def write_review_index():
    """One page per axis, pictures only. Captions stay in walkthrough.md so
    a reviewer reads one description of each screen, not four that can
    disagree."""
    out = ["<!-- Generated by tools/gen_docs_shots.py. Do not edit by hand. -->",
           "", "# Review sets", "",
           "The walkthrough renders in one state: MONO, English, mainnet, "
           "funded. These are the same screens under the conditions nothing "
           "has been reviewed against yet. What each screen is for is written "
           "once, in [the walkthrough](walkthrough.md).", ""]
    for axis in AXES:
        out += ["## " + axis, "", AXES[axis]["note"], ""]
        for path, frame in review_targets(axis):
            name = os.path.basename(path)[:-4]
            out += ["![%s, %s](%s/%s.png)" % (axis, name.replace("-", " "),
                                              axis, name), "",
                    "`%s`" % frame, ""]
    out += ["---", "", "Regenerate with `bash tools/gen_docs_shots.sh`.", ""]
    p = os.path.join(REVIEW, "index.md")
    os.makedirs(REVIEW, exist_ok=True)
    with open(p, "w") as fh:
        fh.write("\n".join(out))
    print("%d lines -> %s" % (len(out), os.path.relpath(p, ROOT)))
    return 0


def read_ppm(path):
    """Parse a binary P6 PPM. Returns (w, h, rgb_bytes)."""
    with open(path, "rb") as fh:
        data = fh.read()

    fields, i = [], 0
    while len(fields) < 4:                       # P6, width, height, maxval
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                # comments run to end of line
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1                                       # exactly one space after maxval

    if fields[0] != b"P6":
        raise ValueError("%s: not a binary PPM (got %r)" % (path, fields[0]))
    w, h, maxval = int(fields[1]), int(fields[2]), int(fields[3])
    if maxval != 255:
        raise ValueError("%s: want 8-bit samples, got maxval %d" % (path, maxval))

    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError("%s: truncated, want %d pixel bytes got %d"
                         % (path, w * h * 3, len(px)))
    return w, h, px


def write_png(path, w, h, rgb):
    """Minimal 8-bit truecolour PNG. No palette, no interlace, filter 0."""
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                            # filter type 0 per scanline
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)
    return len(png)


# ---------------------------------------------------------------- the reveal

# The one thing in this repo a screenshot cannot show: the game becoming a
# signer. README.md described it in prose between a picture of the start and a
# picture of the end, with the whole pitch missing from the middle.
#
# Recording it turned up why nobody had done it. The device draws NOTHING while
# the gesture is made: the menu idles, its stars twinkle, and 56 frames later
# the signer is simply there. That is the security property doing its job, and
# it is also a jump cut that teaches nobody where to draw.
#
# So the frames are real and the finger is drawn on: sim/sim_main.c writes the
# touch coordinate beside every frame it captures, and the stroke is traced from
# that file. Tracing from the data rather than from a copy of the stroke table
# means the annotation cannot drift the first time a coordinate moves. The
# README caption says the trace is added, because a reader who expected the
# device to draw it would be looking for a trail that is not there.
REVEAL_FRAMES = "/tmp/sim_reveal_%03d.ppm"
REVEAL_PATH = "/tmp/sim_reveal_path.txt"
REVEAL_GIF = os.path.join(ROOT, "docs", "media", "kiss-reveal.gif")

# Two frames per GIF frame: the walk runs at LVGL's 16ms tick, so 32ms is real
# time, and 6 centiseconds is the closest a GIF can express it. Under 20ms some
# browsers silently clamp to 100, which would play the gesture five times slower
# than a hand makes it.
REVEAL_STEP = 2
REVEAL_DELAY = 6
REVEAL_HOLD = 200                                # ~2s parked on the signer


def theme_colour(name):
    """A hex colour from main/kiss_theme.h. Never invent one, never copy one."""
    with open(os.path.join(ROOT, "main", "kiss_theme.h")) as fh:
        m = re.search(r"#define\s+%s\s+lv_color_hex\(0x([0-9A-Fa-f]{6})\)" % name,
                      fh.read())
    if not m:
        raise ValueError("no %s in main/kiss_theme.h" % name)
    v = int(m.group(1), 16)
    return (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF


def disc(px, w, h, cx, cy, r, rgb):
    """Filled circle, clipped to the frame. The pen this traces strokes with."""
    for y in range(max(0, cy - r), min(h, cy + r + 1)):
        dy = y - cy
        for x in range(max(0, cx - r), min(w, cx + r + 1)):
            dx = x - cx
            if dx * dx + dy * dy <= r * r:
                i = (y * w + x) * 3
                px[i], px[i + 1], px[i + 2] = rgb


def segment(px, w, h, a, b, r, rgb):
    """A round-capped line, drawn as discs along it. Short strokes, so the
    cost of not being clever is a few thousand pixels."""
    (x0, y0), (x1, y1) = a, b
    steps = max(abs(x1 - x0), abs(y1 - y0), 1)
    for s in range(steps + 1):
        disc(px, w, h, x0 + (x1 - x0) * s // steps,
             y0 + (y1 - y0) * s // steps, r, rgb)


def halve(w, h, px):
    """2x2 box average. Drawing at full size and shrinking afterwards is what
    gives the trace smooth edges without any antialiasing code."""
    ow, oh = w // 2, h // 2
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        r0, r1 = (2 * y) * w * 3, (2 * y + 1) * w * 3
        for x in range(ow):
            a, b = r0 + 6 * x, r1 + 6 * x
            o = (y * ow + x) * 3
            for c in range(3):
                out[o + c] = (px[a + c] + px[a + 3 + c] +
                              px[b + c] + px[b + 3 + c]) // 4
    return ow, oh, out


def build_reveal_gif():
    if not os.path.exists(REVEAL_PATH):
        print("note: no %s, skipping the reveal GIF (rerun /tmp/fruitsim)"
              % REVEAL_PATH)
        return 0
    if not shutil.which("magick") and not shutil.which("convert"):
        print("note: ImageMagick not found, keeping the reveal GIF as committed."
              "\n      Install it (brew install imagemagick) to regenerate.")
        return 0

    pts = []
    with open(REVEAL_PATH) as fh:
        for line in fh:
            n, x, y, down = (int(v) for v in line.split())
            pts.append((n, x, y, bool(down)))

    # Where the signer appears, found by looking rather than by counting
    # strokes. detect_cover_word in main/main.c wants four pen lifts and a wide
    # enough shape, and it is deliberately lenient because it opens the decoy
    # rather than the real wallet, so it fires partway through the last letter.
    # The walk keeps drawing after that, onto a passphrase keyboard, and a trace
    # over those taps would show a gesture being made at a screen that is no
    # longer listening for one. So the GIF ends where the screen changes.
    first = read_ppm(REVEAL_FRAMES % pts[0][0])[2]
    reveal = None
    for n, _, _, _ in pts:
        src = REVEAL_FRAMES % n
        if not os.path.exists(src):
            continue
        cur = read_ppm(src)[2]
        moved = sum(1 for a, b in zip(cur[::997], first[::997]) if a != b)
        if moved > len(cur[::997]) // 5:
            reveal = n
            break
    if reveal is None:
        print("note: the reveal never happened in the captured frames, "
              "skipping the GIF")
        return 0
    pts = [p for p in pts if p[0] <= reveal]

    live = theme_colour("WT_INK")                # the stroke being drawn
    done = theme_colour("WT_MUT")                # strokes already finished
    halo = theme_colour("WT_BG")                 # see below
    tmp = []
    strokes, cur = [], []

    for n, x, y, down in pts:
        if down:
            cur.append((x, y))
        elif cur:
            strokes.append(cur)
            cur = []
        if n == reveal:                          # the signer, drawn on by nothing
            strokes, cur = [], []

        if n % REVEAL_STEP and n != pts[-1][0]:
            continue

        src = REVEAL_FRAMES % n
        if not os.path.exists(src):
            continue
        w, h, frame = read_ppm(src)
        px = bytearray(frame)

        # Halos first, all of them, then the strokes. The menu art is a sunset
        # over a neon grid, so a bare line disappears into whichever band it
        # crosses. Laying every halo down before any stroke stops a later
        # stroke's halo from biting a chunk out of an earlier one where the
        # letters cross.
        for st in strokes + ([cur] if cur else []):
            for i in range(1, len(st)):
                segment(px, w, h, st[i - 1], st[i], 6, halo)
        if cur:
            disc(px, w, h, cur[-1][0], cur[-1][1], 10, halo)

        for st in strokes:
            for i in range(1, len(st)):
                segment(px, w, h, st[i - 1], st[i], 4, done)
        for i in range(1, len(cur)):
            segment(px, w, h, cur[i - 1], cur[i], 4, live)
        if cur:
            disc(px, w, h, cur[-1][0], cur[-1][1], 8, live)

        ow, oh, small = halve(w, h, px)
        out = os.path.join("/tmp", "sim_gif_%03d.ppm" % n)
        with open(out, "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (ow, oh))
            fh.write(bytes(small))
        tmp.append(out)

    if len(tmp) < 2:
        print("note: only %d reveal frames, skipping the GIF" % len(tmp))
        return 0

    im = shutil.which("magick") or shutil.which("convert")
    cmd = ([im, "-loop", "0", "-delay", str(REVEAL_DELAY)] + tmp[:-1] +
           ["-delay", str(REVEAL_HOLD), tmp[-1],
            "-layers", "OptimizePlus", REVEAL_GIF])
    os.makedirs(os.path.dirname(REVEAL_GIF), exist_ok=True)
    if subprocess.call(cmd) != 0:
        sys.stderr.write("ImageMagick failed building %s\n" % REVEAL_GIF)
        return 1
    for f in tmp:
        os.unlink(f)

    print("%-34s <- %d frames, %.1f KB"
          % (os.path.relpath(REVEAL_GIF, ROOT), len(tmp),
             os.path.getsize(REVEAL_GIF) / 1024.0))
    return 0


def write_md():
    out = ["<!-- Generated by tools/gen_docs_shots.py. Do not edit by hand:",
           "     the captions live in that file so they cannot drift from the",
           "     screenshots, and every screenshot is a real simulator frame. -->",
           "",
           "# Walkthrough",
           "",
           "The checks to make before this wallet holds anything you care "
           "about. Every screenshot here is a frame the simulator rendered "
           "from the current firmware, so what you see is what the device "
           "draws.",
           ""]
    for title, intro, items in SECTIONS:
        out += ["## " + title, "", intro, ""]
        for name, _, caption in items:
            # alt describes the picture, the caption below says why it
            # matters. Repeating the caption as alt makes a screen reader
            # read every sentence on this page twice.
            alt = name.split("-", 1)[1].replace("-", " ")
            out += ["![KISS screen: %s](shots/%s.png)" % (alt, name),
                    "", caption, ""]
    out += ["---", "",
            "Regenerate with `bash tools/gen_docs_shots.sh`.", ""]
    with open(MD, "w") as fh:
        fh.write("\n".join(out))
    return len(out)


def check():
    """Manifest vs sim_main.c: catch a save() renamed out from under us."""
    with open(SIM) as fh:
        src = fh.read()
    saved = set(re.findall(r'save\("/tmp/(sim_[a-z0-9_]+)\.ppm"\)', src))
    want = [f for _, f in targets()]
    gone = [f for f in want if f not in saved]
    if gone:
        sys.stderr.write(
            "docs screenshots reference frames sim_main.c no longer saves:\n"
            "  %s\n"
            "either restore the save() or update SECTIONS in %s\n"
            % ("\n  ".join(gone), os.path.relpath(__file__, ROOT)))
        return 1
    dupes = [f for f in set(want) if want.count(f) > 1]
    print("ok: %d screenshots, %d distinct frames, all present in %s%s"
          % (len(want), len(set(want)), os.path.relpath(SIM, ROOT),
             " (reused: %s)" % ", ".join(sorted(dupes)) if dupes else ""))

    # The reveal GIF has no save() to look for, so check its own machinery:
    # the capture switch in the walk, and the file it produced. Losing either
    # would leave the README pointing at a picture nothing regenerates.
    if "g_seq_on = 1" not in src:
        sys.stderr.write(
            "sim_main.c no longer records the reveal (g_seq_on is never set),\n"
            "so docs/media/kiss-reveal.gif cannot be regenerated.\n")
        return 1
    if not os.path.exists(REVEAL_GIF):
        sys.stderr.write("%s is missing, rerun tools/gen_docs_shots.sh\n"
                         % os.path.relpath(REVEAL_GIF, ROOT))
        return 1

    # A picture nobody links to is a picture nobody notices going wrong.
    linked = ""
    for doc in ("README.md", "docs/guide.html", "docs/walkthrough.md"):
        p = os.path.join(ROOT, doc)
        if os.path.exists(p):
            with open(p) as fh:
                linked += fh.read()
    orphans = [rel for rel, _ in LEGACY
               if os.path.basename(rel) not in linked]
    if os.path.basename(REVEAL_GIF) not in linked:
        orphans.append(os.path.relpath(REVEAL_GIF, ROOT))
    if orphans:
        print("note: generated but not referenced by any doc: %s"
              % ", ".join(orphans))

    # The other direction, and it is the one that mattered. The check above
    # asks "is every picture linked", which is a note. Nothing asked "is every
    # linked picture still MADE" -- so when the stroke picker was deleted,
    # 02c-duress-pick.png and 02d-duress-draw.png stopped being generated,
    # stayed on disk from their last run, stayed linked from walkthrough.md,
    # and went on describing a screen the firmware no longer had. Green the
    # whole time. An owner reading the walkthrough was told to choose a stroke
    # the device would not let them choose, which is most of the reason the
    # request kept coming back.
    #
    # A stale file on disk is invisible; a stale file the docs SHOW is a lie.
    # This fails.
    made = {os.path.basename(p) for p, _ in targets()}
    made |= {os.path.basename(rel) for rel, _ in LEGACY}
    made.add(os.path.basename(REVEAL_GIF))
    stale = sorted({m.group(1) for m in re.finditer(
        r'[\w./-]*?([\w-]+\.(?:png|gif))', linked)} - made)
    if stale:
        sys.stderr.write(
            "docs reference pictures nothing generates any more:\n  %s\n"
            "Either add them back to the manifest above, or take them out of\n"
            "the docs -- a picture the docs still show is a screen the reader\n"
            "believes exists.\n" % "\n  ".join(stale))
        return 1
    return 0


def main():
    if "--check" in sys.argv:
        return check()
    if "--axis-list" in sys.argv:
        # For tools/gen_docs_shots.sh. Space separated so the shell can loop
        # it without holding a copy of the axis names itself.
        print(" ".join(AXES))
        return 0
    if "--axis-env" in sys.argv:
        # For tools/gen_docs_shots.sh. Printing KEY=VALUE lines that the shell
        # feeds to `env`, so the environment lives in AXES and nowhere else.
        axis = sys.argv[sys.argv.index("--axis-env") + 1]
        for k, v in AXES[axis]["env"].items():
            print("%s=%s" % (k, v))
        return 0
    if "--review" in sys.argv:
        return write_review(sys.argv[sys.argv.index("--review") + 1])
    if "--review-index" in sys.argv:
        return write_review_index()

    missing = sorted({f for _, f in targets()
                      if not os.path.exists(os.path.join(SRC, f + ".ppm"))})
    if missing:
        sys.stderr.write(
            "no frame for: %s\n"
            "run the simulator first:  bash sim/build_sim.sh && /tmp/fruitsim\n"
            % ", ".join(missing))
        return 1

    total, cache = 0, {}
    for path, frame in targets():
        if frame not in cache:
            w, h, px = read_ppm(os.path.join(SRC, frame + ".ppm"))
            if (w, h) != (800, 480):
                sys.stderr.write("%s: want 800x480, got %dx%d\n" % (frame, w, h))
                return 1
            cache[frame] = (w, h, px)
        w, h, px = cache[frame]
        os.makedirs(os.path.dirname(path), exist_ok=True)
        n = write_png(path, w, h, px)
        total += n
        print("%-34s <- %-20s %6.1f KB"
              % (os.path.relpath(path, ROOT), frame, n / 1024.0))

    if build_reveal_gif():
        return 1

    lines = write_md()
    print("%d screenshots from %d frames, %.0f KB"
          % (len(list(targets())), len(cache), total / 1024.0))
    print("%d lines -> %s" % (lines, os.path.relpath(MD, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
