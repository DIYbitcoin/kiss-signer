#!/usr/bin/env python3
"""A translation that says more than the English it translates.

    python3 tools/check_i18n_bloat.py
    I18NBLOAT_SELFTEST=1 python3 tools/check_i18n_bloat.py

WHY THIS EXISTS. L_RECOVER_B is the screen that stops an owner throwing away
the only copy of their seed words in existence. English is 145 characters.
German had grown to 257, and every other locale to somewhere near 200,
because each translation had gained a clause English never had -- "before
you do anything else", "on a signer you trust". Nothing reported it. The
screen gate photographs a rendered frame, so it only speaks when the text
overflows something; a body that merely says twice as much as it should,
inside a box big enough to hold it, is invisible to every check here.

It was found by reading all 856 keys by hand during the first translation
sweep, and reading them by hand is not a mechanism.

WHAT IT MEASURES. The ratio of a translation's length to its English. A
Latin language runs about a third longer -- that is the most reliable number
in this repository and the SLACK check in overlapcheck.c is built on it -- so
a third is normal and half again is a language being itself. Past 1.6 is
something else: a clause that was added, an explanation the translator felt
was owed, a name written out that English left short.

WHAT IT DOES NOT MEASURE, and cannot: whether the sentence is correct. Three
real grammar bugs turned up in the same pass -- "de el coordinador", "la
coordinador", "da coordenador" -- and no check here would see any of them.
What this does is point at the strings nobody has re-read, which is where
they were.

ja, ko and zh-CN are excluded. They run SHORTER than English, roughly half,
so a ratio against English says nothing about them in either direction.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
I18N = ROOT / "i18n"

# A third longer is normal, half again is a long language. This is the line
# past which a translation is saying something the English does not.
LIMIT = 1.6

# Below this, the ratio is noise: "OK" against a four letter word is 2.0 and
# means nothing. Short strings are the anchors and the button labels, and the
# screen gate already measures those against their real lanes.
MIN_EN = 20

# Denser scripts. A ja string at half the English length is correct, and a
# ratio against English cannot say anything useful about it.
SKIP_LOCALES = {"en", "ja", "ko", "zh-CN"}

# Shrink only. Every entry is a string that was over the line when this gate
# was written and has not been cut yet; the count may fall and never rise.
# "locale KEY" per line.
BACKLOG = set("""
cs-CZ S_STOP_NET_B
da-DK N_NOTHING_SIGNED
da-DK S_STOP_NET_B
de G_STORAGE_CONFIRM_AMNESIC_B
de G_WIPE_CANT
de N_NOTHING_SIGNED
de R_NOT_FOUND_B
de S_STOP_NET_B
de W_KEF_SD_S
es-ES N_NOTHING_SIGNED
es-ES P_OUT_GT_IN
es-ES S_STOP_NET_B
es-MX P_OUT_GT_IN
es-MX S_STOP_NET_B
fr L_KEF_TYPE_PROMPT
fr N_NOTHING_SIGNED
fr R_CHAIN_CLEAN
fr S_STOP_NET_B
hr-HR N_NOTHING_SIGNED
hr-HR S_STOP_NET_B
it G_STORAGE_FAIL_GENERIC_B
it N_NOTHING_SIGNED
it S_STOP_NET_B
it W_SETUP_S
nb-NO N_NOTHING_SIGNED
nb-NO S_STOP_NET_B
nl G_WIPE_CANT
nl N_NOTHING_SIGNED
nl S_STOP_NET_B
pl N_NOTHING_SIGNED
pt-BR G_STORAGE_CONFIRM_AMNESIC_B
pt-BR N_NOTHING_SIGNED
pt-BR S_STOP_NET_B
pt-PT G_STORAGE_CONFIRM_AMNESIC_B
pt-PT L_FAIL_OPEN_T
pt-PT N_NOTHING_SIGNED
pt-PT S_STOP_NET_B
ru N_NOTHING_SIGNED
sv-SE N_NOTHING_SIGNED
sv-SE S_STOP_NET_B
sv-SE W_VERIFY_S
tr G_STORAGE_CONFIRM_AMNESIC_B
tr H_HINT_ROW
tr S_STOP_NET_B
vi G_STORAGE_CONFIRM_AMNESIC_B
vi N_NOTHING_SIGNED
""".split("\n")) - {""}


def load() -> tuple[dict, dict]:
    en = json.loads((I18N / "en.json").read_text(encoding="utf-8"))
    locs = {}
    for p in sorted(I18N.glob("*.json")):
        loc = p.stem
        if loc in SKIP_LOCALES:
            continue
        locs[loc] = json.loads(p.read_text(encoding="utf-8"))
    return en, locs


def findings(en: dict, locs: dict) -> list[tuple[float, str, str, int, int]]:
    out = []
    for loc, d in locs.items():
        for key, e in en.items():
            v = d.get(key, "")
            if not v or len(e) < MIN_EN:
                continue
            ratio = len(v) / len(e)
            if ratio > LIMIT:
                out.append((ratio, loc, key, len(e), len(v)))
    out.sort(reverse=True)
    return out


def selftest() -> int:
    """The rule fires on the string it was written for and is quiet on the
    one that fixed it. Both are real: L_RECOVER_B's German before and after.
    """
    was = ("Ihre Seed-Wörter sind noch im Speicher und sonst nirgends. "
           "schreiben Sie sie auf Papier, bevor Sie irgendetwas anderes tun. "
           "dann erneut versuchen. schlägt das Speichern weiter fehl, "
           "stellen Sie von diesem Papier auf einem Signer wieder her, dem "
           "Sie trauen.")
    now = ("Ihre Seed-Wörter sind noch im Speicher und sonst nirgends. jetzt "
           "auf Papier schreiben. dann erneut versuchen. schlägt es weiter "
           "fehl, vom Papier wiederherstellen.")
    en_len = 145
    cases = [
        ("the bloated German fires", len(was) / en_len > LIMIT, True),
        ("the cut German is quiet", len(now) / en_len > LIMIT, False),
        ("a third longer is quiet", 1.33 > LIMIT, False),
        ("half again is quiet", 1.5 > LIMIT, False),
        ("a short string is skipped", MIN_EN > 4, True),
    ]
    bad = 0
    for name, got, want in cases:
        ok = got is want
        bad += 0 if ok else 1
        print("  %-32s %s" % (name, "ok" if ok else "BROKEN"))
    print("bloat selftest: %d cases, %d broken" % (len(cases), bad))
    return bad


def main() -> int:
    bad = selftest() if os.environ.get("I18NBLOAT_SELFTEST") else 0
    if bad:
        print("the check no longer fires on the string it was written for")
        return 1

    en, locs = load()
    found = findings(en, locs)
    live = [f for f in found if f"{f[1]} {f[2]}" not in BACKLOG]
    seen = {f"{f[1]} {f[2]}" for f in found}
    stale = sorted(BACKLOG - seen)

    for ratio, loc, key, le, lv in live:
        print("  %.2fx  %-6s %-24s en %3d -> %3d" % (ratio, loc, key, le, lv))

    print("i18n bloat: %d strings past %.1fx English, %d backlogged"
          % (len(live), LIMIT, len(BACKLOG)))
    for s in stale:
        print("  backlog entry to retire: %s is under the line now" % s)
    if live:
        print("\nA translation this much longer than its English has gained a")
        print("clause the English never had. Cut it back to what English says.")
    return 1 if (live or stale) else 0


if __name__ == "__main__":
    sys.exit(main())
