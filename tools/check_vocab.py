#!/usr/bin/env python3
"""The words on screen, against i18n/GLOSSARY.md.

Why this is a script and not a paragraph
----------------------------------------
GLOSSARY.md already said it, in these words: bare "words", used as if it
named the thing, "reads as a house term and has to be unlearned the first
time an owner opens anything else". CLAUDE.md has a whole Vocabulary section
above it. Both were in force when the LOCKED BACKUP explainer was written
saying "A locked copy of your words", and then rewritten saying "Keep your
paper words too" -- two invented terms in a row, on the one screen deciding
whether a second copy of an owner's seed words gets made, and it took two
rounds of the owner shouting to get "seed words" onto it.

So the rule stops being something to remember. Every rule here is one that
was already written down and broken anyway.

Shape, copied from sim/overlapcheck.c because it is the shape that works
here: a rule fires, a per-rule ALLOW carries the uses that are correct with
the reason each is correct, and a per-rule BACKLOG carries the strings the
glossary already admits are unconverted. New findings fail. A backlog entry
that stops firing is reported so the list can only shrink. VOCAB_SELFTEST=1
proves every rule still fires, because a sweep that reports nothing means
nothing unless a dead rule would have been caught.
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EN = ROOT / "i18n" / "en.json"

# Product and coordinator names are other people's spelling and are stripped
# before any rule runs. "Sparrow Wallet" is what Sparrow calls itself.
PROPER = [
    "BlueWallet", "Sparrow Wallet", "Airgapped Hardware Wallet",
    "New Wallet", "SeedQR", "SeedSigner",
]


class Rule:
    def __init__(self, name, pattern, instead, why, allow=(), backlog=(),
                 fires_on="", clean=""):
        self.name = name
        self.re = re.compile(pattern, re.I)
        self.instead = instead
        self.why = why
        self.allow = dict(allow)
        self.backlog = set(backlog)
        self.fires_on = fires_on      # selftest: must fire
        self.clean = clean            # selftest: must NOT fire


RULES = [
    Rule(
        "BARE-WORDS",
        r"\b(?:your|the|these|those|my)\s+words\b",
        'say "seed words" (what they are) or "recovery words" (the backup)',
        'GLOSSARY.md: bare "words" reads as a house term and has to be '
        "unlearned the first time an owner opens anything else",
        backlog=[
            # GLOSSARY.md's own "not yet converted" list. Each is pinned to a
            # pixel box in sim/fitcheck.c, so the anchor is wider than the word
            # it replaces and every slot has to be re-measured, not assumed.
            "GD_WORD_C_W2_B", "L_PPINTRO_B", "T_PATH_PLAIN", "W_QRBAD_B",
            "W_TYPE_MY_WORDS", "W_VINTRO_W1_B",
        ],
        fires_on="A locked copy of your words.",
        clean="A locked copy of your seed words.",
    ),
    Rule(
        "COINED-WORDS",
        r"\b(?:paper|backup|secret|magic|wallet|signer|key)\s+words\b",
        'say "seed words"; if the sentence is about paper, say where they go '
        '("seed words still go on paper")',
        "a modifier in front of `words` invents a second name for a thing "
        "that already has one, and the reader has to work out whether it is "
        "the same thing",
        fires_on="Optional. Keep your paper words too.",
        clean="Optional. Seed words still go on paper.",
    ),
    Rule(
        "BARE-SEED",
        r"\bseed\b(?!\s+words)(?!\s*QR)",
        'say "seed words"',
        "GLOSSARY.md: never the bare word `seed` in a user-facing string. It "
        "is fine in code, comments and filenames",
        backlog=["GD_INTRO_S"],
        fires_on="a passphrase makes a second signer from one seed",
        clean="a passphrase opens different seed words",
    ),
    Rule(
        "WALLET",
        r"\bwallet\b",
        "say signing device, signer, or keys",
        "CLAUDE.md: `wallet` names ONLY the coordinator's object -- a key set "
        "and the coins it watches. Never this box, never a screen, never "
        "anything stored on it",
        allow={
            "I_NOTE_BW": "BlueWallet's own menu path, quoted",
            "I_NOTE_SPARROW": "Sparrow's own menu path, quoted",
            "R_SP_WHY_B": "the SENDER's wallet: someone else's software",
            "S_SPARROW_SAVE": "Sparrow's own name for itself",
            "W_WHATSEED_B": "every BIP39 wallet that exists, not this one",
        },
        fires_on="erases the wallet history stored now",
        clean="erases what this signer has seen",
    ),
]


def scan(strings):
    """[(rule, key, text)] for every fresh finding, plus stale backlog keys."""
    found, stale = [], []
    for rule in RULES:
        hit = set()
        for key, text in strings.items():
            if not isinstance(text, str):
                continue
            probe = text
            for name in PROPER:
                probe = probe.replace(name, " ")
            if not rule.re.search(probe):
                continue
            hit.add(key)
            if key in rule.allow or key in rule.backlog:
                continue
            found.append((rule, key, text))
        stale += [(rule, k) for k in sorted(rule.backlog - hit)]
    return found, stale


def selftest():
    """Every rule fires on the string it was written for, and stays quiet on
    the string that fixed it. A rule that fires on everything is as dead as
    one that fires on nothing, so both halves are asserted."""
    bad = 0
    for rule in RULES:
        if not rule.re.search(rule.fires_on):
            print(f"SELFTEST: {rule.name} no longer fires on "
                  f"{rule.fires_on!r}", file=sys.stderr)
            bad += 1
        if rule.clean and rule.re.search(rule.clean):
            print(f"SELFTEST: {rule.name} fires on the FIXED string "
                  f"{rule.clean!r}", file=sys.stderr)
            bad += 1
    print(f"vocabulary selftest: {len(RULES)} rules, {bad} broken")
    return 1 if bad else 0


def main():
    if os.environ.get("VOCAB_SELFTEST"):
        return selftest()
    if selftest():
        print("vocabulary gate: refusing to report, a rule is dead",
              file=sys.stderr)
        return 1

    strings = json.loads(EN.read_text(encoding="utf-8"))
    found, stale = scan(strings)

    backlogged = sum(len(r.backlog) for r in RULES)
    for rule, key, text in found:
        print(f"ERROR: {rule.name} {key}: {text[:72]!r}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key in stale:
        print(f"note: {rule.name} backlog entry {key} no longer fires -- "
              f"drop it from tools/check_vocab.py")

    print(f"vocabulary: {len(strings)} strings, {len(found)} new, "
          f"{backlogged} backlogged, {len(stale)} backlog entries to retire")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
