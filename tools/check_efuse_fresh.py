#!/usr/bin/env python3
"""Read the board's security eFuses before the step that burns them.

    check_efuse_fresh.py --fresh       -p <port>     # before a release burn
    check_efuse_fresh.py --reflashable -p <port>     # before a rehearsal flash
    check_efuse_fresh.py --fresh --json <summary.json>   # judge a saved summary
    check_efuse_fresh.py --selftest

The release recipe used to say "erase the fresh board (proves it is fresh)".
Erasing flash proves nothing about eFuses: erase-flash succeeds on a board
whose fuses already say DEVELOPMENT-mode encryption, so the rehearsal board
would have passed that step and been burned half locked. The only thing that
knows whether a board is fresh is the eFuse block, and this reads it.

--fresh is the release gate: every security fuse the burn is about to set must
read zero, INCLUDING the key purposes, because a board that already holds an
XTS key has already been through a first boot. --reflashable is the rehearsal
gate: the encryption fuses may be burned (that board has been through it), but
nothing that closes the cable may be. The two lists are the two recipes.

Reads `espefuse summary --format json` through the pinned esptool, or a saved
copy via --json. A field the summary does not carry FAILS: absent must never
look like zero, which is the class of mistake the erase step was. Exit 1 on a
burned fuse, 2 when the board could not be read.

Zero is not the whole question, and reading only the value was the same class
of mistake one level down. A fuse the summary reports as NOT READABLE prints
zero as readily as a fuse that is genuinely clear, and the burn that follows
is permanent, so an unreadable field fails in both modes. A fuse that reads
zero but is already WRITE PROTECTED cannot be set by the provisioning that
comes next, so it fails --fresh: the board would take the recipe and then
refuse the fuse halfway through. A rehearsal board is allowed write-protected
fuses, because it has been through a first boot, so --reflashable asks only
that the ones it names read zero and are readable.

A field carrying neither flag is treated as unreadable rather than as fine.
If espefuse omits them for some fields on some chips, this says which field
and stops, instead of guessing on the one operation with no undo.
"""
import json
import os
import subprocess
import sys

ESPTOOL_PIN = os.environ.get("ESPTOOL_PIN", "esptool==5.3.1")

# Every fuse the release recipe burns or depends on, plus the ones that would
# mean somebody else got there first. Names are espefuse's for the esp32p4.
FRESH = [
    "SPI_BOOT_CRYPT_CNT",            # flash encryption on
    "DIS_DOWNLOAD_MANUAL_ENCRYPT",   # RELEASE mode: no plaintext serial flash
    "SECURE_BOOT_EN",
    "SECURE_BOOT_KEY_REVOKE0", "SECURE_BOOT_KEY_REVOKE1", "SECURE_BOOT_KEY_REVOKE2",
    "SECURE_BOOT_AGGRESSIVE_REVOKE",
    "KEY_PURPOSE_0", "KEY_PURPOSE_1", "KEY_PURPOSE_2",
    "KEY_PURPOSE_3", "KEY_PURPOSE_4", "KEY_PURPOSE_5",
    "DIS_DOWNLOAD_MODE", "ENABLE_SECURITY_DOWNLOAD",
    "SECURE_VERSION",
]

# What a rehearsal board must NOT have: anything that locks the cable or the
# bootloader. Its encryption fuses and the XTS key purposes are allowed, that
# is what a rehearsal board is.
REFLASHABLE = [
    "DIS_DOWNLOAD_MANUAL_ENCRYPT",
    "SECURE_BOOT_EN",
    "SECURE_BOOT_KEY_REVOKE0", "SECURE_BOOT_KEY_REVOKE1", "SECURE_BOOT_KEY_REVOKE2",
    "SECURE_BOOT_AGGRESSIVE_REVOKE",
    "DIS_DOWNLOAD_MODE", "ENABLE_SECURITY_DOWNLOAD",
]

MODES = {"--fresh": FRESH, "--reflashable": REFLASHABLE}


def parse_summary(text):
    """The JSON object out of espefuse's stdout, whatever it printed first."""
    dec = json.JSONDecoder()
    for i, ch in enumerate(text):
        if ch == "{":
            try:
                obj, _ = dec.raw_decode(text[i:])
                return obj
            except ValueError:
                continue
    raise ValueError("no JSON object in the espefuse output")


def read_board(port):
    cmd = ["uvx", "--from", ESPTOOL_PIN, "espefuse", "--chip", "esp32p4",
           "-p", port, "summary", "--format", "json"]
    try:
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        sys.exit("FAIL: could not run espefuse: %s" % e)
    if run.returncode:
        sys.stderr.write(run.stderr)
        sys.exit("FAIL: espefuse exited %d, board not read" % run.returncode)
    return run.stdout


def burned(summary, fields, need_writeable=False):
    """[(name, raw, why)] for every listed fuse that does not pass.

    Three ways to fail, and only the first is about the value. A field
    missing from the summary cannot be proven zero. A field the chip says is
    not readable cannot be proven zero either, whatever it printed. And when
    the caller is about to provision (need_writeable), a fuse that reads zero
    but is already write protected will refuse the burn halfway through.
    """
    out = []
    for name in fields:
        e = summary.get(name)
        if e is None:
            out.append((name, "absent", "not in the summary"))
            continue
        raw = str(e.get("raw_value", ""))
        # `is not True` on purpose: a missing flag is not a passing flag.
        if e.get("readable") is not True:
            out.append((name, raw, "not readable, so zero cannot be proven"))
            continue
        try:
            zero = int(raw, 16) == 0
        except ValueError:
            zero = False
        if not zero:
            out.append((name, raw, str(e.get("value", ""))))
        elif need_writeable and e.get("writeable") is not True:
            out.append((name, raw, "reads zero but is already write protected"))
    return out


def judge(summary, mode):
    fields = MODES[mode]
    fresh = mode == "--fresh"
    hits = burned(summary, fields, need_writeable=fresh)
    what = "fresh" if fresh else "reflashable"
    if hits:
        lines = ["FAIL: board is NOT %s, %d of %d fuses did not pass:"
                 % (what, len(hits), len(fields))]
        lines += ["      %-30s %-8s %s" % h for h in hits]
        return False, "\n".join(lines)
    return True, ("PASS: board is %s, all %d security fuses read zero and "
                  "are readable" % (what, len(fields))
                  + (" and writable" if fresh else ""))


def fixture(**nonzero):
    """A summary in espefuse's JSON shape, zero everywhere except as given."""
    names = sorted(set(FRESH) | set(REFLASHABLE))
    out = {}
    for n in names:
        raw = nonzero.get(n, 0)
        out[n] = {"name": n, "raw_value": "0x%x" % raw, "value": raw,
                  "readable": True, "writeable": True}
    return out


def selftest():
    bad = 0
    ran = 0

    def case(label, want, got):
        nonlocal bad, ran
        ran += 1
        good = got == want
        print("  %-52s %s" % (label, "ok" if good else "FAILED"))
        bad += not good

    fresh = fixture()
    case("a fresh board is fresh", True, judge(fresh, "--fresh")[0])
    case("a fresh board is reflashable", True, judge(fresh, "--reflashable")[0])

    # The rehearsal board: encryption burned in DEVELOPMENT mode, two XTS key
    # blocks in use. This is the board the erase step would have called fresh.
    rehearsal = fixture(SPI_BOOT_CRYPT_CNT=0x7, KEY_PURPOSE_0=2, KEY_PURPOSE_1=3)
    ok, msg = judge(rehearsal, "--fresh")
    case("the rehearsal board is NOT fresh", False, ok)
    case("...and the crypt count is named", True, "SPI_BOOT_CRYPT_CNT" in msg)
    case("the rehearsal board IS reflashable", True,
         judge(rehearsal, "--reflashable")[0])

    locked = fixture(SPI_BOOT_CRYPT_CNT=0x7, DIS_DOWNLOAD_MANUAL_ENCRYPT=1,
                     SECURE_BOOT_EN=1, KEY_PURPOSE_0=2, KEY_PURPOSE_1=3,
                     KEY_PURPOSE_2=9, ENABLE_SECURITY_DOWNLOAD=1)
    case("a burned board is neither", False,
         judge(locked, "--fresh")[0] or judge(locked, "--reflashable")[0])

    # Absent must not pass as zero.
    partial = fixture()
    del partial["SECURE_BOOT_EN"]
    ok, msg = judge(partial, "--fresh")
    case("a summary missing a field fails", False, ok)
    case("...and says which", True, "SECURE_BOOT_EN" in msg and "absent" in msg)

    # Zero from a fuse nobody can read is not a zero. This is the summary the
    # reviewer built: SECURE_BOOT_EN reading 0 with readable false, which the
    # value-only reader passed.
    blind = fixture()
    blind["SECURE_BOOT_EN"]["readable"] = False
    ok, msg = judge(blind, "--fresh")
    case("a fuse reading zero but unreadable fails", False, ok)
    case("...and says it could not be proven", True, "not readable" in msg)
    case("...in the rehearsal mode too", False,
         judge(blind, "--reflashable")[0])

    # Write protected and clear: nothing can burn it, so the provisioning that
    # follows --fresh would stop partway. A rehearsal board is past that.
    shut = fixture()
    shut["SECURE_BOOT_KEY_REVOKE1"]["writeable"] = False
    ok, msg = judge(shut, "--fresh")
    case("a clear fuse that cannot be written fails --fresh", False, ok)
    case("...and says why", True, "write protected" in msg)
    case("...but --reflashable does not care", True,
         judge(shut, "--reflashable")[0])

    # A summary that carries neither flag is not evidence of anything.
    bare = fixture()
    bare["KEY_PURPOSE_0"] = {"name": "KEY_PURPOSE_0", "raw_value": "0x0",
                             "value": 0}
    case("a field with no readable flag fails", False,
         judge(bare, "--fresh")[0])

    # espefuse talks before it dumps; the JSON has to be found, not assumed.
    text = "espefuse v5.3.1\nConnecting....\nDetecting chip type... ESP32-P4\n" \
        + json.dumps(fresh, indent=4) + "\n"
    try:
        case("JSON is found after the connection banner", True,
             parse_summary(text) == fresh)
    except ValueError:
        case("JSON is found after the connection banner", True, False)

    print("efuse fresh selftest: %d cases, %d broken" % (ran, bad))
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    mode = next((m for m in MODES if m in argv), None)
    if not mode:
        sys.exit(__doc__)
    if "--json" in argv:
        text = open(argv[argv.index("--json") + 1]).read()
    elif "-p" in argv:
        text = read_board(argv[argv.index("-p") + 1])
    else:
        sys.exit(__doc__)
    try:
        summary = parse_summary(text)
    except ValueError as e:
        sys.exit("FAIL: %s" % e)
    ok, msg = judge(summary, mode)
    print(msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
