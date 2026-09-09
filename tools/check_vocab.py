#!/usr/bin/env python3
"""The words on screen and in the docs, against i18n/GLOSSARY.md.

Why this is a script and not a paragraph
----------------------------------------
GLOSSARY.md already said it, in these words: bare "words", used as if it
named the thing, "reads as a house term and has to be unlearned the first
time an owner opens anything else". The house rules have a Vocabulary section
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


SEP = "\x00"   # a unit break that is not a line break; see _strip_html

# Tags that sit inside a sentence rather than around one.
INLINE = ["a", "b", "i", "em", "strong", "span", "sub", "sup", "small",
          "abbr", "kbd", "mark", "u", "s", "q", "cite", "time", "var"]


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


# What "words" is allowed to mean when it is not the seed. Written as a list
# rather than folded into one regex because each entry is a different claim
# and the next person has to be able to disagree with one of them.
#
# The rule started as `(your|the|these|those|my) words` and the owner widened
# it: never bare "words", always "seed words" or "BIP39 mnemonic phrase".
# That is a
# rule about the NAME, and a name does not stop being the name because the
# determiner in front of it changed -- "SHOW WORDS AGAIN", "how many words?"
# and "Words that rebuild your keys" all named the thing and none of them
# matched the old pattern.
WORDS_OK = (
    r"seed", r"recovery",                       # the name, said in full
    r"plain",                                   # "plain words card", the tone
    r"\d+", r"English",                         # the 2048 on the list
)
# ... and the phrases where the list, not the seed, is the subject. Only
# docs/blind-draw.md needs these: it is the page about making the list.
WORDS_OK_AFTER = (r"out\b", r"on the list\b")


def bare_words(text):
    """Bare "words" where the name belongs. Returns the offending phrase."""
    for m in re.finditer(r"\bwords\b", text, re.I):
        before = text[:m.start()].rstrip()
        if any(re.search(r"(?i)\b%s$" % w, before) for w in WORDS_OK):
            continue
        after = text[m.end():].lstrip()
        if any(re.match(r"(?i)%s" % w, after) for w in WORDS_OK_AFTER):
            continue
        return text[max(0, m.start() - 24):m.end() + 8].strip()
    return None


RULES = [
    Measure(
        "BARE-WORDS",
        bare_words,
        'say "seed words" (what they are), "recovery words" (the backup) or '
        '"BIP39 mnemonic phrase"',
        'GLOSSARY.md: bare "words" reads as a house term and has to be '
        "unlearned the first time an owner opens anything else. It is the "
        "name of the thing, so it is written in full every time",
        allow={
            "L_WEAK_ACK": "the PASSPHRASE, not the seed: a few words you "
                          "remember beat one short one",
        },
        fires_on="SHOW WORDS AGAIN",
        clean="SHOW SEED WORDS AGAIN",
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
        "`wallet` names ONLY the coordinator's object -- a key set "
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
    Rule(
        "PAYMENT",
        r"(?<!silent )\bpayments?\b",
        'say "transaction"',
        "GLOSSARY.md: a payment is what one person sends another; a "
        "transaction is the object with inputs, outputs, change, a fee and a "
        "txid, which is what every row on the signing screen already says. It "
        "also quietly narrows -- a consolidation, a sweep and a coinjoin are "
        "all transactions and none of them is a payment. Silent payments are "
        "the exception: BIP352's own name, and the only place the word stays",
        allow={
            "K_SPGATE_GOES": "the silent-payment scan key gate: privacy of "
                             "payments to the silent address",
            "K_SP_SUB":      "what a scan key is for, on the silent-payment row",
            "R_SP_EXPORT_NOTE": "silent payments, the sentence names the "
                                "silent address in its next line",
            "R_SP_FACT_FIND":   "the silent-payment fact strip",
            "R_SP_FRESH":       "the silent-payment fact strip",
            "R_SP_WHY_B":       "how a silent payment derives its address",
            "S_SP_ONCHAIN_FMT": "a payment TO a silent address, on the badge "
                                "that explains the mismatch",
        },
        fires_on="What you pay to get this payment mined.",
        clean="What you pay to get this transaction mined.",
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
            "S_COINS_HELP_B", "W_CARDS_HELP_B",
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


# ---- the same vocabulary rule, in the other twenty languages -----------
#
# This gate read i18n/en.json and nothing else, which is why one English word
# could be fixed on the glass and stay wrong on twenty screens. "payment" went
# out of the English strings in one pass; the German still said Zahlung, the
# French paiement, the Japanese 支払い, and no check in the tree could see any
# of them. A vocabulary rule that only speaks English is a rule about one
# locale, not about the product.
#
# Only PAYMENT is swept this way, because it is the only rule whose word has a
# clean equivalent in every language here. The named rules above are about
# English house terms and do not translate.
#
# Silent payments keep the word in every language, same as in English -- BIP352
# names itself that way and every coordinator repeats it. Keyed off the KEY
# rather than the text, because "silent" is not the adjacent word in most of
# these languages and a lookbehind cannot find it.
SP_KEY = re.compile(r"_SP_|_SPGATE")

LOCALE_WORDS = {
    "cs-CZ": r"platb|platby|platbu",
    "da-DK": r"betaling",
    "de":    r"zahlung",
    "es-ES": r"pago",
    "es-MX": r"pago",
    "fr":    r"paiement",
    "hr-HR": r"plaćanj|placanj",
    "it":    r"pagament",
    "ja":    r"支払",
    "ko":    r"결제",
    "nb-NO": r"betaling",
    "nl":    r"betaling",
    "pl":    r"płatnoś|platnos",
    "pt-BR": r"pagament",
    "pt-PT": r"pagament",
    "ru":    r"платеж|платёж",
    "sv-SE": r"betalning",
    "tr":    r"ödeme|odeme",
    "vi":    r"thanh toán|thanh toan",
    "zh-CN": r"付款",
}

# EMPTY, and that is the state to keep it in. It held thirteen keys across
# twenty locales -- 228 translated strings still saying payment after the
# English had stopped -- and they were swept in three passes: English, the
# sixteen Latin-script locales, then Japanese, Korean, Chinese and Russian.
#
# Two of those keys were ones the English never had. "raise the fee there if
# it stalls" came back as "it says when the PAYMENT confirms", and "practice
# with a tiny send" as "a small PAYMENT". No English-only rule could have
# found either: the word entered the product through the translations, which
# is the whole reason this sweep reads twenty files instead of one.
#
# Anything that lands here again is a translation that reintroduced it.
LOCALE_BACKLOG = set()


def scan_locales():
    """[(lang, key)] for every translated string still saying payment."""
    found = []
    for lang, word in LOCALE_WORDS.items():
        path = ROOT / "i18n" / f"{lang}.json"
        if not path.exists():
            continue
        rx = re.compile(word, re.I)
        with open(path, encoding="utf-8") as f:
            strings = json.load(f)
        for key, text in strings.items():
            if SP_KEY.search(key) or not isinstance(text, str):
                continue
            if rx.search(text):
                found.append((lang, key))
    return found


def report_locales():
    """0 when nothing new says payment in a language other than English."""
    found = scan_locales()
    new = [(lang, key) for lang, key in found if key not in LOCALE_BACKLOG]
    still = {key for _, key in found}

    for lang, key in new:
        print(f"  {lang}/{key}: says payment where the English says "
              f"transaction")
    for key in sorted(LOCALE_BACKLOG - still):
        print(f"  retire {key} from LOCALE_BACKLOG: swept in every locale")

    print(f"vocabulary: {len(LOCALE_WORDS)} locales, {len(new)} new, "
          f"{len(found)} backlogged strings across "
          f"{len(still & LOCALE_BACKLOG)} keys, "
          f"{len(LOCALE_BACKLOG - still)} backlog entries to retire")
    return 1 if new else 0


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
    "SECURITY.md",
    "docs/index.html",
    "docs/guide.html",
    "docs/verify-release.html",
    "docs/walkthrough.md",
    "docs/blind-draw.md",
]

# CHANGELOG.md is deliberately NOT here, and this is the reason rather than an
# oversight. It carries about forty-five uses of the word, and the entry that
# announced the vocabulary itself -- "this is a signing device, what it holds
# is keys, and a wallet is the thing your coordinator watches" -- is one of
# them. Every entry above it uses the new words because it was written after;
# every entry below uses the old ones because it was written before. Sweeping
# it would make the history claim this device always said keys, which is the
# one thing the changelog exists to be honest about.
#
# ROADMAP.md is out for a weaker reason worth stating too: it is a table of
# what is planned, and its uses are wallet TYPES ("taproot key path wallet"),
# which is the coordinator's sense. If it grows prose an owner reads, add it.

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
# the binary", "BIP39 turns the mnemonic phrase into a binary seed" are the
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
    "lets your wallet find those payments":
        "the coordinator scanning for silent payments",
    # The other legitimate sense, and the one W_WHATSEED_B is already allowed
    # for on the glass: every BIP39 wallet that exists, said about the shared
    # wordlist rather than about this box.
    "every wallet on earth": "every BIP39 wallet there is, not this one",
    "every wallet project": "every BIP39 implementation, not this one",
    "no wallet will accept": "no BIP39 implementation, not this one",
}

# NOTE: the WALLET rule is `\bwallet\b`, so it does not see the plural. The
# three plural uses in docs/index.html ("test wallets only", "disposable
# wallets only") are all the coordinator sense and correct, so widening it
# today would buy one ALLOW entry per correct sentence and nothing else.
# Written down rather than fixed, so the next person deciding knows it was a
# decision.


def _blank(m):
    """Delete a match but keep its newlines. Every removal below spans lines
    -- a fenced block, a <pre>, an HTML comment -- and collapsing one to a
    space renumbers every line after it, so the file:line a finding reports
    stops naming the line the word is on. Three findings pointed at a `serve`
    command, a fee bullet and a table rule before this existed."""
    return "\n" * m.group(0).count("\n")


def _strip_html(raw):
    """Tags out, prose in, and every line still where it was.

    alt= is put back in PLACE rather than collected at the top, because a
    finding has to name the line the word is on: hoisting the alt text
    renumbers the whole file. It is copy either way -- it is what somebody
    who cannot see the picture is read instead."""
    raw = re.sub(r"(?is)<!--.*?-->", _blank, raw)
    raw = re.sub(r"(?is)<(script|style|pre|code)\b.*?</\1>", _blank, raw)
    raw = re.sub(r'(?i)<img\b[^>]*?\balt="([^"]*)"[^>]*>', r" \1 ", raw)
    # An INLINE tag becomes a space and a BLOCK tag a newline. Turning every
    # tag into a newline splits "Seed <strong>words</strong>" into two lines
    # and the second one reads as a bare "words" that nobody wrote.
    raw = re.sub(r"(?is)</?(%s)\b[^>]*>" % "|".join(INLINE), " ", raw)
    # A block tag ends the unit but must NOT add a line: turning it into a
    # newline grew README by 71 lines and docs/index.html by 205, and every
    # file:line after the first tag named the wrong place. SEP is the break;
    # only real newlines count as lines.
    return re.sub(r"(?is)<[^>]+>",
                  lambda m: SEP + "\n" * m.group(0).count("\n"), raw)


def _md_text(raw):
    raw = re.sub(r"(?s)```.*?```", _blank, raw)
    raw = re.sub(r"`[^`\n]*`", " ", raw)       # one line: [^`]* eats newlines
    raw = re.sub(r"!\[([^\]\n]*)\]\([^)\n]*\)", r"\1", raw)   # alt text stays
    raw = re.sub(r"\[([^\]\n]*)\]\([^)\n]*\)", r"\1", raw)    # link text stays
    # A blockquote marker is punctuation, not a word. Left in, it lands
    # between "Seed" and "words" when a [!WARNING] block wraps.
    raw = re.sub(r"(?m)^[ \t]{0,3}>[ \t]?", "", raw)   # \s would eat the newline
    # README embeds raw HTML for its picture tables, so the same pass runs
    # here: without it an <img src="setup-2-words.png"> reads as prose and
    # the gate reports a filename as a bare "words".
    return _strip_html(raw)


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
        text = _strip_html(raw) if rel.endswith(".html") else _md_text(raw)
        # A PARAGRAPH, not a line. Prose is hard wrapped, so "Seed\nwords
        # already on the device" is one sentence written correctly and two
        # lines, the second of which opens with a bare "words". Reading a
        # line at a time invents that defect and then demands an ALLOW entry
        # to excuse it. The key still names the line the paragraph starts on,
        # because a person has to be able to open it.
        start, buf = 0, []
        units = []
        for i, line in enumerate(text.split("\n"), 1):
            for frag in line.split(SEP):
                units.append((i, frag))
            if SEP in line:
                units.append((i, ""))     # a block tag ends the paragraph
        for i, line in units:
            line = re.sub(r"\s+", " ", line).strip()
            # A table row, a list item or a heading is its own unit and never
            # wraps, so joining one to the line above only moves the reported
            # line number away from the word being reported on.
            if line and buf and re.match(r"[|\-*#<]|\d+\.", line):
                out["%s:%d" % (rel, start)] = " ".join(buf)
                start, buf = i, [line]
                continue
            if line:
                if not buf:
                    start = i
                buf.append(line)
                continue
            if buf and len(" ".join(buf)) > 2:
                out["%s:%d" % (rel, start)] = " ".join(buf)
            buf = []
        if buf and len(" ".join(buf)) > 2:
            out["%s:%d" % (rel, start)] = " ".join(buf)
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
    # Every extractor here is line preserving, and that is not a nicety: the
    # whole value of this lane is a file:line somebody can open. It broke four
    # separate ways while it was being written -- a fenced block collapsed to
    # a space, an inline-code regex that ate newlines, a block tag that ADDED
    # one, a multi-line <meta> that swallowed three -- and each time the gate
    # still reported, pointing at a `serve` command or a fee bullet. So the
    # invariant is asserted rather than trusted.
    for rel in DOCS:
        f = ROOT / rel
        if not f.exists():
            continue
        raw = f.read_text(encoding="utf-8")
        text = _strip_html(raw) if rel.endswith(".html") else _md_text(raw)
        if raw.count("\n") != text.count("\n"):
            print(f"SELFTEST: extracting {rel} moved its lines "
                  f"({raw.count(chr(10))} -> {text.count(chr(10))}), so every "
                  f"file:line it reports names the wrong line", file=sys.stderr)
            bad += 1

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
    locales_bad = report_locales()
    return 1 if (found or named or doc_found or locales_bad) else 0


if __name__ == "__main__":
    sys.exit(main())
