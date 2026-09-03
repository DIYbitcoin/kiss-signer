#!/usr/bin/env python3
"""Every refusal an owner can be shown has words in their language.

Why this is a script and not a paragraph
----------------------------------------
`tr_reason` in main/kiss_sign.c turns the internal reason string a refusal
carries into a translated one, and ends `return r;` -- the C string itself
-- when it has no entry for it. That fallback is silent by construction: the
screen renders, nothing overlaps, no gate has an opinion, and the only
evidence is an owner reading developer English on the one screen whose whole
job is telling them why their money is not moving.

Three reasons were in that state at once, and one of them was
"input derivation pubkey does not match" -- the ownership check firing,
which is the refusal that matters most on the whole device. Nothing found
them. A frame would not have: the walk renders one refusal, in English,
where the fallback and a correct translation are the same pixels.

So the question gets asked mechanically. It is the same shape as
check_vocab.py: fires, self tests, and refuses to report if it has gone
dead.

Two directions, both real:

  MISSING -- a reason with no entry. Developer English in 21 locales.
  ORPHAN  -- an entry no reason raises. Dead code holding an i18n key
             alive, so check_i18n_orphans.py stays quiet about it.

Reading the ARGUMENT, not the line. Six of the reasons are set through a
ternary:

    stop(s, kiss_testnet() ? "wrong network: mainnet transaction"
                           : "unsupported input derivation path");

so a grep for `stop(s, "` finds one of the two arms and misses the other, on
a line that looks completely ordinary. This walks balanced parentheses and
takes every literal inside, which is why the ORPHAN direction can be an
error rather than a shrug.

Scope is STOP reasons. caution() writes s->reason too, but only the STOP
branch of verify_screen renders it, so a caution reason has no way to reach
glass and checking it would report on a path that does not exist.

    python3 tools/check_stop_reasons.py
    CHECKSTOP_SELFTEST=1 python3 tools/check_stop_reasons.py
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PSBT = ROOT / "main" / "kiss_psbt.c"
SIGN = ROOT / "main" / "kiss_sign.c"

LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def strip_comments(src):
    """Blank out comments, keeping length and newlines so nothing shifts.

    Done with a scanner rather than a regex because both files carry `//` and
    `"` inside each other constantly -- the MAP's own comment quotes a reason
    string, and kiss_psbt.c's comments quote wally error codes.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\":
                    if i + 1 < n:
                        out.append(src[i + 1])
                        i += 2
                        continue
                elif src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def call_args(src, name):
    """Yield (line, argument text) for every `name(...)` call in src."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src):
        i = m.end()
        depth, n = 1, len(src)
        while i < n and depth:
            c = src[i]
            if c == '"' or c == "'":
                q = c
                i += 1
                while i < n:
                    if src[i] == "\\":
                        i += 2
                        continue
                    if src[i] == q:
                        break
                    i += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if not depth:
                    break
            i += 1
        yield src.count("\n", 0, m.start()) + 1, src[m.end():i]


def unquote(lit):
    return lit[1:-1].replace('\\"', '"').replace("\\\\", "\\")


def reasons(src):
    """{reason: line} for every literal that can reach s->reason via stop()."""
    out = {}
    for line, args in call_args(src, "stop"):
        for lit in LITERAL.findall(args):
            out.setdefault(unquote(lit), line)
    return out


def mapped(src):
    """{reason: line} for every entry in tr_reason's MAP."""
    at = src.find("tr_reason")
    if at < 0:
        return {}
    start = src.find("MAP[] = {", at)
    end = src.find("};", start)
    if start < 0 or end < 0:
        return {}
    out = {}
    for m in re.finditer(r"\{\s*(" + LITERAL.pattern + r")\s*,", src[start:end]):
        line = src.count("\n", 0, start + m.start()) + 1
        out.setdefault(unquote(m.group(1)), line)
    return out


def compare(psbt_src, sign_src):
    raised, shown = reasons(psbt_src), mapped(sign_src)
    missing = [(r, raised[r]) for r in sorted(raised) if r not in shown]
    orphan = [(r, shown[r]) for r in sorted(shown) if r not in raised]
    return raised, shown, missing, orphan


# A reason kiss_psbt.c does not raise and tr_reason maps anyway. There is one:
# the caution path seeds s->reason with its own wording, and this single entry
# is how the verify screen names the unproven-input refusal. Everything else in
# the MAP has a stop() behind it.
ORPHAN_ALLOW = {
    "input amounts not proven":
        "raised by caution(), mapped here so the refusal and the caution "
        "share one string",
}

SELF_MISSING = "a reason nothing translates"
SELF_MAPPED = "sighash is not ALL"


def selftest():
    """Both directions fire on a synthesised file, and stay quiet on the
    fixed one. A checker that reports nothing proves nothing unless a dead
    one would have been caught -- and the ternary arm is asserted on its own,
    because reading the line instead of the argument is the exact mistake
    this was written to survive."""
    bad = 0
    psbt = ('// stop(s, "a comment quoting a reason");\n'
            'stop(s, "%s");\n'
            'stop(s, x ? "%s" : "%s");\n' % (SELF_MAPPED, SELF_MISSING, SELF_MAPPED))
    sign = ('static const char *tr_reason(const char *r)\n'
            '{\n    static const struct { const char *en; int id; } MAP[] = {\n'
            '        {"%s", STR_P_SIGHASH},\n'
            '        {"a reason nothing raises", STR_P_MALFORMED},\n'
            '    };\n}\n' % SELF_MAPPED)
    psbt, sign = strip_comments(psbt), strip_comments(sign)

    raised, _, missing, orphan = compare(psbt, sign)
    if SELF_MAPPED not in raised:
        print("SELFTEST: a plain stop() literal is no longer read",
              file=sys.stderr)
        bad += 1
    if SELF_MISSING not in [r for r, _ in missing]:
        print("SELFTEST: MISSING no longer fires on an unmapped reason",
              file=sys.stderr)
        bad += 1
    if SELF_MAPPED in [r for r, _ in missing]:
        print("SELFTEST: MISSING fires on a reason that IS mapped",
              file=sys.stderr)
        bad += 1
    if "a reason nothing raises" not in [r for r, _ in orphan]:
        print("SELFTEST: ORPHAN no longer fires on an unraised entry",
              file=sys.stderr)
        bad += 1
    if SELF_MAPPED in [r for r, _ in orphan]:
        print("SELFTEST: ORPHAN fires on an entry that IS raised",
              file=sys.stderr)
        bad += 1
    # A ternary arm read as part of the argument, not the line it sits on.
    tern = reasons(strip_comments('stop(s, x ? "left arm" : "right arm");\n'))
    if "left arm" not in tern or "right arm" not in tern:
        print("SELFTEST: a ternary arm is no longer read -- the checker is "
              "back to grepping lines", file=sys.stderr)
        bad += 1
    # A quoted reason inside a comment must not count as raised.
    if reasons(strip_comments('// stop(s, "commented out");\n')):
        print("SELFTEST: a reason inside a comment counts as raised",
              file=sys.stderr)
        bad += 1

    print(f"stop-reason selftest: {bad} broken")
    return 1 if bad else 0


def main():
    if os.environ.get("CHECKSTOP_SELFTEST"):
        return selftest()
    if selftest():
        print("stop-reason gate: refusing to report, the check is dead",
              file=sys.stderr)
        return 1

    psbt = strip_comments(PSBT.read_text(encoding="utf-8"))
    sign = strip_comments(SIGN.read_text(encoding="utf-8"))
    raised, shown, missing, orphan = compare(psbt, sign)
    orphan = [(r, l) for r, l in orphan if r not in ORPHAN_ALLOW]

    for r, line in missing:
        print(f"ERROR: MISSING {r!r}\n"
              f"    raised at main/kiss_psbt.c:{line}\n"
              f"    -> add it to tr_reason's MAP in main/kiss_sign.c\n"
              f"       without one it reaches the refusal screen as this "
              f"English, in all 21 locales", file=sys.stderr)
    for r, line in orphan:
        print(f"ERROR: ORPHAN {r!r}\n"
              f"    mapped at main/kiss_sign.c:{line}\n"
              f"    -> nothing in kiss_psbt.c raises it: drop the entry, or "
              f"add it to ORPHAN_ALLOW with the reason", file=sys.stderr)

    print(f"stop reasons: {len(raised)} raised, {len(shown)} translated, "
          f"{len(missing)} missing, {len(orphan)} orphaned")
    return 1 if (missing or orphan) else 0


if __name__ == "__main__":
    sys.exit(main())
