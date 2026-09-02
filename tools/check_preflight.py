#!/usr/bin/env python3
"""Every desktop CI step is also run by tools/preflight.sh, or is listed here.

preflight.sh opens by claiming it is "everything the desktop CI lane checks, in
one command". That claim is the whole value of the file -- a green preflight is
only worth running if it means the push will not come back red -- and nothing
kept the claim true. It was already false on the day it landed: four CI steps
had no preflight line, among them check_sim_taps.py, which exists because a
scripted tap can silently stop hitting anything.

This is the same question tools/check_gates.py asks about checkers ("is it run
by ANYTHING?"), aimed one level in: is it run by the thing that says it runs
everything?

    python3 tools/check_preflight.py
    CHECKPREFLIGHT_SELFTEST=1 python3 tools/check_preflight.py

WHAT IT READS. Only what CI actually EXECUTES: the shell inside `run:` blocks,
with comments stripped. Scanning the file as plain text does not work and the
failure is not theoretical -- three of the eight tokens a plain grep reported
missing (tools/build_wasm.sh, tools/make_web_release.sh, tools/nvs_seed_check.sh)
appear ONLY inside explanatory comments. Demanding preflight run those would
have been three false findings in the checker written to prevent false findings.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CI = os.path.join(ROOT, ".github", "workflows", "desktop-tests.yml")
PF = os.path.join(ROOT, "tools", "preflight.sh")

# A script CI runs that preflight deliberately does not, and why. A reason is
# mandatory: an entry with no reason is how a real gap gets parked as a
# decision. preflight.sh's own header states the same three exclusions in
# prose -- these are the mechanical half of it.
#
# Shrink-only. An entry naming something CI no longer runs is reported so it
# gets deleted rather than sitting here explaining a step that is gone.
SKIP = {
    "sim/build_simapp.sh":
        "the interactive simulator needs libsdl2-dev, which CI installs and a "
        "working tree may not have; a missing SDL2 would fail preflight for "
        "everyone who never runs the interactive sim",
}

TOKEN = re.compile(r"\b(?:tools|sim)/[A-Za-z0-9_]+\.(?:py|sh)\b")
RUN = re.compile(r"^(\s*)run:\s*(.*)$")
NAME = re.compile(r"^\s*-\s*name:\s*(.*)$")


def strip_shell_comments(text):
    """Drop what the shell would not execute.

    Both a whole-line `# ...` and a trailing ` # ...`. A '#' that opens a
    comment is at the start of a word, so a bare '#' glued to a previous
    character (a colour literal, a printf format) is left alone.
    """
    out = []
    for line in text.splitlines():
        s = line.lstrip()
        if s.startswith("#"):
            continue
        line = re.sub(r"(?:(?<=\s)|(?<=^))#.*$", "", line)
        out.append(line)
    return "\n".join(out)


def ci_steps(path):
    """[(step name, executed shell)] for every `run:` in the workflow."""
    lines = open(path, encoding="utf-8").read().splitlines()
    steps, name = [], "?"
    i = 0
    while i < len(lines):
        m = NAME.match(lines[i])
        if m:
            name = m.group(1).strip()
        m = RUN.match(lines[i])
        if not m:
            i += 1
            continue
        indent, inline = len(m.group(1)), m.group(2).strip()
        if inline and inline not in ("|", ">", "|-", ">-"):
            steps.append((name, inline))
            i += 1
            continue
        body, i = [], i + 1
        while i < len(lines):
            ln = lines[i]
            if ln.strip() and (len(ln) - len(ln.lstrip())) <= indent:
                break
            body.append(ln)
            i += 1
        steps.append((name, "\n".join(body)))
    return steps


def scan():
    steps = ci_steps(CI)
    if not steps:
        return None, f"parsed 0 run: blocks out of {CI} -- the parser is broken"

    pf_text = strip_shell_comments(open(PF, encoding="utf-8").read())
    have = set(TOKEN.findall(pf_text))

    wanted = {}          # token -> the CI step that runs it
    for name, shell in steps:
        for tok in TOKEN.findall(strip_shell_comments(shell)):
            wanted.setdefault(tok, name)

    missing = sorted(t for t in wanted if t not in have and t not in SKIP)
    stale = sorted(t for t in SKIP if t not in wanted)
    return (steps, wanted, have, missing, stale), None


def selftest():
    """Both directions, plus the comment rule that three tokens turned on."""
    cases, bad = 0, 0

    # 1. a token CI runs and preflight does not is reported
    got = TOKEN.findall(strip_shell_comments("          python3 tools/check_nope.py"))
    cases += 1
    if got != ["tools/check_nope.py"]:
        print(f"selftest: an executed token was not seen: {got}")
        bad += 1

    # 2. a token that appears ONLY in a comment is not demanded. This is the
    #    one that matters: it is why the checker reads run: blocks at all.
    for text in ("      # tools/build_wasm.sh is byte for byte reproducible",
                 "          python3 tools/x.py  # not tools/make_web_release.sh"):
        cases += 1
        got = TOKEN.findall(strip_shell_comments(text))
        if "tools/build_wasm.sh" in got or "tools/make_web_release.sh" in got:
            print(f"selftest: a commented-out token was demanded: {got}")
            bad += 1

    # 3. the workflow still parses into steps that carry shell
    steps = ci_steps(CI)
    cases += 1
    if len(steps) < 10 or not any("python3" in s for _, s in steps):
        print(f"selftest: parsed {len(steps)} run: blocks, expected many with shell")
        bad += 1

    # 4. every SKIP entry gives a reason
    for tok, why in SKIP.items():
        cases += 1
        if not why or len(why) < 20:
            print(f"selftest: SKIP[{tok}] has no real reason")
            bad += 1

    print(f"preflight selftest: {cases} cases, {bad} broken")
    return 1 if bad else 0


def main():
    if os.environ.get("CHECKPREFLIGHT_SELFTEST"):
        if selftest():
            return 1

    res, err = scan()
    if err:
        print(f"preflight coverage: {err}", file=sys.stderr)
        return 1
    steps, wanted, have, missing, stale = res

    print(f"preflight coverage: {len(steps)} CI steps, {len(wanted)} scripts they "
          f"run, {len(wanted) - len(missing) - len(SKIP)} also in preflight, "
          f"{len(missing)} missing, {len(SKIP)} skipped")

    for tok in stale:
        print(f"  SKIP entry to retire: {tok} -- CI no longer runs it")
    for tok in missing:
        print(f"  MISSING from tools/preflight.sh: {tok}")
        print(f"      CI runs it in: {wanted[tok]}")

    if missing or stale:
        print("\npreflight.sh says it is everything the desktop CI lane checks.")
        print("Add a run line for each, or add it to SKIP with the reason why not.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
