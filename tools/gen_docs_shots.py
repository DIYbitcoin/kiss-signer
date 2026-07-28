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
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = "/tmp"
OUT = os.path.join(ROOT, "docs", "shots")
MD = os.path.join(ROOT, "docs", "walkthrough.md")
SIM = os.path.join(ROOT, "sim", "sim_main.c")

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
          "Draw K, I, S, S anywhere on the menu. On a device with no duress "
          "strokes set, that opens the passphrase login."),
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
         ("02c-duress-pick", "sim_duress_pick_real",
          "One extra stroke after the word decides which wallet opens. You "
          "choose which stroke is yours, so reading this firmware does not "
          "tell anyone what to draw."),
         ("02d-duress-draw", "sim_duress_draw_real",
          "Draw it over the printed word, twice, before anything is saved. "
          "Plain KISS keeps working forever and always opens the spare, so "
          "forgetting your stroke can never lock you out of the device."),
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

    # A picture nobody links to is a picture nobody notices going wrong.
    linked = ""
    for doc in ("README.md", "docs/guide.html", "docs/walkthrough.md"):
        p = os.path.join(ROOT, doc)
        if os.path.exists(p):
            with open(p) as fh:
                linked += fh.read()
    orphans = [rel for rel, _ in LEGACY
               if os.path.basename(rel) not in linked]
    if orphans:
        print("note: generated but not referenced by any doc: %s"
              % ", ".join(orphans))
    return 0


def main():
    if "--check" in sys.argv:
        return check()

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

    lines = write_md()
    print("%d screenshots from %d frames, %.0f KB"
          % (len(list(targets())), len(cache), total / 1024.0))
    print("%d lines -> %s" % (lines, os.path.relpath(MD, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
