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

Two kinds of rule, because the owner asked for two things. The NAMED rules
are about calling a thing by its name -- seed words, signer, keys. The PLAIN
rules are about the rest of the sentence: ordinary Bitcoin and computer
words, said the way somebody would say them out loud. "Optional. Never
instead of paper." broke none of the named rules and still had to be
explained, which is the failure the plain rules exist for.

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
import subprocess
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

    def search(self, text):
        m = self.re.search(text)
        return m.group(0) if m else None


class Measure:
    """A rule that counts rather than matches. Same ALLOW/BACKLOG contract."""

    def __init__(self, name, fn, instead, why, allow=(), backlog=(),
                 fires_on="", clean=""):
        self.name = name
        self.fn = fn
        self.instead = instead
        self.why = why
        self.allow = dict(allow)
        self.backlog = set(backlog)
        self.fires_on = fires_on
        self.clean = clean

    def search(self, text):
        return self.fn(text)


def _sentences(text):
    return [s for s in re.split(r"[.!?\n]+", text) if s.strip()]


def _words(text):
    return re.findall(r"[A-Za-z][A-Za-z'-]*", text)


def long_sentence(text):
    """A sentence nobody would say in one breath."""
    for s in _sentences(text):
        if len(_words(s)) > 14:
            return s.strip()
    return None


def _syllables(word):
    w = re.sub(r"[^a-z]", "", word.lower())
    if not w:
        return 0
    n = len(re.findall(r"[aeiouy]+", w))
    if w.endswith("e") and n > 1 and not w.endswith(("le", "ee")):
        n -= 1
    return max(1, n)


# Every 4+ syllable word already on screen, each one a term the reader meets
# in Bitcoin or on a computer anyway. A NEW one has to be added here on
# purpose, which is the whole mechanism: a long word is a decision, not a
# reflex.
LONG_WORDS_OK = {
    "coordinator", "coordinator's", "derivation", "compatible", "signatures",
    "replaceable", "unavailable", "unsupported", "unreadable", "unverifiable",
    "unencrypted", "verification", "denomination", "destination", "recovery",
    "information", "deniability", "manufacturer", "security", "internally",
    "everywhere", "everything", "animated",
}


def long_word(text):
    for w in _words(text):
        if _syllables(w) >= 4 and w.lower() not in LONG_WORDS_OK:
            return w
    return None


RULES = [
    Rule(
        "BARE-WORDS",
        r"\b(?:your|the|these|those|my)\s+words\b",
        'say "seed words" (what they are) or "recovery words" (the backup)',
        'GLOSSARY.md: bare "words" reads as a house term and has to be '
        "unlearned the first time an owner opens anything else",
        # GLOSSARY.md's "not yet converted" list was six strings and is
        # empty: they were measured and converted rather than excused.
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
    # ---- PLAIN: the rest of the sentence ---------------------------------
    Rule(
        "APHORISM",
        r"\b(?:never|not)\s+(?:instead|rather)\b",
        "say what to DO: \"seed words still go on paper\"",
        "an instruction phrased as a negation of something else makes the "
        "reader work out what they are being told. The owner read "
        "\"Optional. Never instead of paper.\" and asked what it meant",
        fires_on="Optional. Never instead of paper.",
        clean="Optional. Seed words still go on paper.",
    ),
    Rule(
        "METAPHOR",
        r"\b(?:sits?|lives?|travels?|sleeps?|grows?|wakes?|breathes?)\b"
        r"|\bmakes? the trip\b",
        "say what actually happens: is, stays, never leaves, comes from",
        "seed words do not live anywhere, coins do not sit on the network and "
        "keys do not make trips. A reader deciding something has to translate "
        "the picture back into the fact, and some of them get it wrong",
        fires_on="your seed words now live in flash.",
        clean="your seed words are now in flash.",
    ),
    Measure(
        "LONG-SENTENCE",
        long_sentence,
        "split it, or cut it to the one thing the reader has to do",
        "over fourteen words is longer than anybody says out loud, and every "
        "screen here is read standing up, once, by somebody deciding "
        "something",
        backlog=[
            # Measured, not excused: these predate the rule and each is a
            # teaching paragraph rather than an instruction. They shrink when
            # their screen is next touched.
            "G_FW_BIG_B", "G_FW_OK_B", "G_STORAGE_CLEANUP_B", "N_PSBT_B",
            "S_COINS_HELP_B", "S_D_TXID_CHANGES", "S_D_TXID_SAME",
            "S_STOP_NET_B", "S_STOP_NOFP_B", "W_CARDS_HELP_B",
            "W_SD_CORRUPT_B",
        ],
        fires_on="your coordinator built a bitcoin transaction and this "
                 "signer will now show you every part of it before anything "
                 "is signed",
        clean="your coordinator built this. read it before you sign.",
    ),
    Measure(
        "LONG-WORD",
        long_word,
        "use the shorter word, or add it to LONG_WORDS_OK with the reason it "
        "has to be that word",
        "four syllables is a word somebody has to stop at. The ones already "
        "on screen are terms Bitcoin and computers use anyway; a new one is "
        "a decision",
        fires_on="the authentication requirement is nonnegotiable",
        clean="you must unlock it first",
    ),
]


# Rules that apply to NAMES as well as to sentences. A name is not a string
# an owner reads, so most of the copy rules have nothing to say about one --
# but the vocabulary rules do, because a name is where the next string comes
# from. docs/readme/wallet.png was the picture at the top of README.md for
# months, and the walk called the same frame sim_wallet in six save() calls,
# which is where the filename came from. The sweep that renamed the screen to
# signer-home could not see either: this gate read i18n/en.json and nothing
# else, so a filename was never a string and a save() literal never was.
NAME_RULES = ["WALLET"]

# Names that keep the word on purpose, same contract as a rule's ALLOW.
NAME_ALLOW = {}


def names():
    """(label, name) for every tracked path and every walk frame."""
    out = []
    ls = subprocess.run(["git", "-C", str(ROOT), "ls-files"],
                        capture_output=True, text=True)
    if ls.returncode == 0:
        for p in ls.stdout.split():
            out.append(("path", p))
    sim = ROOT / "sim" / "sim_main.c"
    if sim.exists():
        for frame in re.findall(r'save\("([^"]+)"\)',
                                sim.read_text(encoding="utf-8")):
            out.append(("frame", frame))
    return out


def scan_names():
    """[(rule, label, name)] for every name a vocabulary rule fires on."""
    rules = [r for r in RULES if r.name in NAME_RULES]
    found = []
    for label, name in names():
        # A path is read a segment at a time, so the separators are word
        # breaks: wallet-home.png and docs/wallet/ both have to hit.
        probe = re.sub(r"[/_.-]", " ", name)
        for rule in rules:
            if rule.search(probe) and name not in NAME_ALLOW:
                found.append((rule, label, name))
    return found


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
            if not rule.search(probe):
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
        if not rule.search(rule.fires_on):
            print(f"SELFTEST: {rule.name} no longer fires on "
                  f"{rule.fires_on!r}", file=sys.stderr)
            bad += 1
        if rule.clean and rule.search(rule.clean):
            print(f"SELFTEST: {rule.name} fires on the FIXED string "
                  f"{rule.clean!r}", file=sys.stderr)
            bad += 1
    # The name scan is its own half and fails its own way: it reads paths
    # rather than sentences, so a rule that works on prose can still miss a
    # filename, where the word arrives between a slash and a dash.
    probe = re.sub(r"[/_.-]", " ", "docs/media/wallet-home.png")
    fixed = re.sub(r"[/_.-]", " ", "docs/media/signer-home.png")
    wallet = next((r for r in RULES if r.name == "WALLET"), None)
    if not wallet or not wallet.search(probe):
        print("SELFTEST: the name scan no longer fires on "
              "docs/media/wallet-home.png", file=sys.stderr)
        bad += 1
    elif wallet.search(fixed):
        print("SELFTEST: the name scan fires on the FIXED name "
              "docs/media/signer-home.png", file=sys.stderr)
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
    named = scan_names()

    backlogged = sum(len(r.backlog) for r in RULES)
    for rule, label, name in named:
        print(f"ERROR: {rule.name} {label} {name}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key, text in found:
        print(f"ERROR: {rule.name} {key}: {text[:72]!r}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key in stale:
        print(f"note: {rule.name} backlog entry {key} no longer fires -- "
              f"drop it from tools/check_vocab.py")

    print(f"vocabulary: {len(strings)} strings, {len(found)} new, "
          f"{backlogged} backlogged, {len(stale)} backlog entries to retire")
    print(f"vocabulary: {len(names())} names checked, {len(named)} bad")
    return 1 if (found or named) else 0


if __name__ == "__main__":
    sys.exit(main())
