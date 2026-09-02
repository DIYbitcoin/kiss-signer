#!/usr/bin/env python3
"""The words on screen and in the docs, against i18n/GLOSSARY.md.

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

Three surfaces, not one. i18n/en.json is the glass. The tracked paths and the
walk's save() names are where the next string comes from. And DOCS is the
pages an owner reads before they own the device -- README.md and the
published pages under docs/ -- which had no rule enforced on them at all
until this lane, and had drifted accordingly. See DOCS_RULES for why the
docs read under three of the eight rules rather than all of them.
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
            "G_FW_BIG_B", "G_FW_OK_B", "G_STORAGE_CLEANUP_B",
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


# The card in the box is a surface this project ships, so it is written under
# the same rules as the glass and read by the same gate. It was the one piece
# of owner-facing copy living outside any review: docs/packaging-card.md says
# so itself, and this is the half that makes that true rather than aspirational.
#
# Only the blockquote. The file around it argues for the copy and is prose for
# whoever maintains it, not words an owner ever reads.
CARD = ROOT / "docs" / "packaging-card.md"


def card_strings():
    if not CARD.exists():
        return {}
    out, n, buf = {}, 0, []
    for line in CARD.read_text(encoding="utf-8").split("\n"):
        if not line.startswith(">"):
            continue
        body = line[1:].strip()
        if body.startswith("###"):
            if buf:
                out["CARD_%d_B" % n] = " ".join(buf)
                buf = []
            n += 1
            out["CARD_%d_H" % n] = body.lstrip("#").strip()
        elif body:
            buf.append(body)
    if buf:
        out["CARD_%d_B" % n] = " ".join(buf)
    return out


# ---- docs/: the pages an owner reads BEFORE they own the device ---------
#
# The gate read i18n/en.json, the tracked paths and the walk's save() names,
# and that is the glass plus the names the glass came from. It never read a
# word of prose, so README.md and the published pages under docs/ -- the only
# KISS most people will ever see -- were the one owner-facing surface with no
# rule enforced on it at all. They had drifted exactly where you would expect:
# nine sentences calling this device or its keys a wallet, a storage section
# headed "where the words live" in three places, and two UI paths quoting a
# tab named WALLET that the device has called KEYS for as long as I_T has
# existed. A doc that names a tab wrong is worse than one that reads oddly:
# the reader taps and there is nothing there.
#
# Same contract as CARD above -- owner-facing copy only. Specs, decisions.md,
# the audit and the plans are prose for whoever maintains this, and are not
# read under the copy rules.
DOCS = [
    "README.md",
    "docs/index.html",
    "docs/guide.html",
    "docs/verify-release.html",
    "docs/walkthrough.md",
]

# Three of the eight rules, and the five left out are left out on purpose.
#
# LONG-SENTENCE and LONG-WORD are calibrated for a screen read standing up,
# once, by somebody deciding something. A guide is read sitting down with the
# device in front of you: 78 long words and 64 long sentences, almost all of
# them ordinary. A rule that fires 142 times on correct prose is a rule
# nobody reads, which is the failure this file's own docstring names.
#
# METAPHOR and BARE-SEED are narrower misses but the same shape. Prose about
# a build says "artifacts live on the Releases tab", "read live from eFuse",
# "the pad that sits behind it" -- the ordinary computer senses of words that
# only mislead when a screen uses them about seed words. And "no dev seed in
# the binary", "BIP39 processes the mnemonic into a binary seed" are the
# technical senses BARE-SEED's own why already excuses in code and comments.
# Both would need five or six ALLOW entries to say nothing new.
#
# What is left is the three that caught every real defect in the sweep that
# added this lane, and that cannot be right in prose and wrong on glass.
DOCS_RULES = ["WALLET", "BARE-WORDS", "COINED-WORDS"]

# Stripped before the rules run, exactly as PROPER is, because these are the
# coordinator's object: a key set and the coins it watches, which is the ONE
# surviving use of the word. Narrow phrases rather than a bare "wallet" --
# "your wallet" alone would excuse the next sentence that calls this box one.
#
# Shrink-only, like a rule's BACKLOG: a phrase that stops appearing is
# reported so the list cannot outlive the sentences it was written for.
DOCS_ALLOW = {
    "online wallet": "the coordinator's object, watching the chain",
    "watch-only wallet": "what a coordinator calls the import it gets",
    "any other wallet that reads": "any coordinator, generically",
    "the imported wallet": "BlueWallet's own label, quoted",
    "lets your wallet find those payments":
        "the coordinator scanning for silent payments",
}

# NOTE: the WALLET rule is `\bwallet\b`, so it does not see the plural. The
# three plural uses in docs/index.html ("test wallets only", "disposable
# wallets only") are all the coordinator sense and correct, so widening it
# today would buy one ALLOW entry per correct sentence and nothing else.
# Written down rather than fixed, so the next person deciding knows it was a
# decision.


def _html_text(raw):
    """Visible prose from a page. Code is not copy: a command, a filename or
    a JSON key inside <code> or <pre> is quoted machine text, and reading it
    under the copy rules would fire on every one of them."""
    raw = re.sub(r"(?is)<!--.*?-->", " ", raw)
    raw = re.sub(r"(?is)<(script|style|pre|code)\b.*?</\1>", " ", raw)
    # alt= is read out loud to somebody who cannot see the picture, so it is
    # copy in every sense that matters here.
    alts = re.findall(r'(?i)\balt="([^"]*)"', raw)
    return "\n".join(alts) + "\n" + re.sub(r"(?is)<[^>]+>", "\n", raw)


def _md_text(raw):
    raw = re.sub(r"(?is)<!--.*?-->", " ", raw)
    raw = re.sub(r"(?s)```.*?```", " ", raw)
    raw = re.sub(r"`[^`]*`", " ", raw)
    raw = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", raw)      # alt text stays
    raw = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", raw)       # link text stays
    return raw


def docs_strings():
    """{path:line -> one line of prose} for every owner-facing document.

    A line rather than a sentence: the key has to name a place a person can
    open, and "docs/guide.html:447" does that where a sentence index does
    not."""
    out = {}
    for rel in DOCS:
        f = ROOT / rel
        if not f.exists():
            continue
        raw = f.read_text(encoding="utf-8")
        text = _html_text(raw) if rel.endswith(".html") else _md_text(raw)
        for i, line in enumerate(text.split("\n"), 1):
            line = re.sub(r"\s+", " ", line).strip()
            if len(line) > 2:
                out["%s:%d" % (rel, i)] = line
    return out


def scan_docs():
    """[(rule, key, text)] for the docs lane, plus ALLOW phrases gone stale."""
    rules = [r for r in RULES if r.name in DOCS_RULES]
    found, seen = [], set()
    for key, text in sorted(docs_strings().items()):
        probe = text
        for name in PROPER:
            probe = probe.replace(name, " ")
        for phrase in DOCS_ALLOW:
            if re.search(re.escape(phrase), probe, re.I):
                seen.add(phrase)
                probe = re.sub(re.escape(phrase), " ", probe, flags=re.I)
        for rule in rules:
            if rule.search(probe):
                found.append((rule, key, text))
    stale = sorted(set(DOCS_ALLOW) - seen)
    return found, stale


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

    # The docs lane fails its own way: it can only report what the extractor
    # hands it, so an extractor that quietly returns nothing -- a tag shape
    # that eats the body, a renamed file -- is a green sweep over an unread
    # surface. Assert that it reads prose, that it does NOT read the shell
    # commands beside it, and that the lane still fires.
    docs = docs_strings()
    if len(docs) < 200:
        print(f"SELFTEST: the docs lane extracted {len(docs)} lines, which is "
              f"too few to be reading {len(DOCS)} documents", file=sys.stderr)
        bad += 1
    if any("esptool --chip" in t for t in docs.values()):
        print("SELFTEST: the docs lane is reading fenced commands as copy",
              file=sys.stderr)
        bad += 1
    if not any("passphrase" in t.lower() for t in docs.values()):
        print("SELFTEST: the docs lane read no prose at all", file=sys.stderr)
        bad += 1
    lane = [r for r in RULES if r.name in DOCS_RULES]
    if not any(r.search("KEYS \u2192 BACKUP opens the wallet on this device")
               for r in lane):
        print("SELFTEST: the docs lane no longer fires on a page calling this "
              "device a wallet", file=sys.stderr)
        bad += 1
    if any(r.search("KEYS \u2192 BACKUP opens the keys on this device")
           for r in lane):
        print("SELFTEST: the docs lane fires on the FIXED sentence",
              file=sys.stderr)
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
    strings.update(card_strings())
    found, stale = scan(strings)
    named = scan_names()
    doc_found, doc_stale = scan_docs()

    backlogged = sum(len(r.backlog) for r in RULES)
    for rule, label, name in named:
        print(f"ERROR: {rule.name} {label} {name}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key, text in found:
        print(f"ERROR: {rule.name} {key}: {text[:72]!r}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key, text in doc_found:
        print(f"ERROR: {rule.name} {key}: {text[:72]!r}\n"
              f"    -> {rule.instead}\n"
              f"       {rule.why}", file=sys.stderr)
    for rule, key in stale:
        print(f"note: {rule.name} backlog entry {key} no longer fires -- "
              f"drop it from tools/check_vocab.py")
    for phrase in doc_stale:
        print(f"note: DOCS_ALLOW {phrase!r} no longer appears in any "
              f"document -- drop it from tools/check_vocab.py")

    print(f"vocabulary: {len(strings)} strings, {len(found)} new, "
          f"{backlogged} backlogged, {len(stale)} backlog entries to retire")
    print(f"vocabulary: {len(names())} names checked, {len(named)} bad")
    print(f"vocabulary: {len(docs_strings())} lines of docs prose, "
          f"{len(doc_found)} bad, {len(DOCS_ALLOW)} allowed, "
          f"{len(doc_stale)} allow entries to retire")
    return 1 if (found or named or doc_found) else 0


if __name__ == "__main__":
    sys.exit(main())
