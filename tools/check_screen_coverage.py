#!/usr/bin/env python3
"""Which screens has no gate ever looked at?

overlapcheck asks seven questions per STOP. A screen with no stop is a screen
nobody has ever asked about -- and that is not hypothetical: whatseed_open was a
title, a subtitle and one 704x232 paragraph, BARE by rule 1, on the screen a
newcomer opens to find out what a seed is. Every gate reported clean for its
entire life because no walk stop rendered it. It was found by accident.

Two halves, because one alone is blind:

  built but never captured   the walk opens the screen and never photographs it
                             (reported by sim_main itself, see wt_sim_uncaptured)
  never built at all         the walk never opens it, so the check above cannot
                             see it either -- this script's job

The second half needs the set of screens that EXIST, which only the source
knows. Every screen is wt_screen(parent, title, sub) or one of the mk_screen
wrappers, and the title is nearly always tr(STR_KEY), so the keys are greppable.
Titles built from a variable are counted as unresolvable and printed, because a
gate that quietly ignores what it cannot parse is the thing being fixed.

    python3 tools/check_screen_coverage.py          report
    SCREENCOVER_STRICT=1 python3 tools/...          exit 1 on any uncovered

Run after sim/build_sim.sh; this drives /tmp/fruitsim itself.
"""
import contextlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM = Path("/tmp/fruitsim")

# One gate run at a time on this machine. /tmp/simsd is a single fake card and
# every gate wipes it before each walk, so two runs at once means one of them has
# its fixtures deleted mid-walk: the file list comes up short, the coordinate
# taps land on rows that moved, and the walk derails. Every stop after that
# prints "clean" for a screen it never reached, which is worse than a red run --
# it is a green one that measured nothing.
#
# Same lock and same protocol as sim/run_overlapcheck.sh: the directory IS the
# lock, mkdir is atomic, and a holder that died takes its lock with it after
# LOCK_STALE. mkdir rather than fcntl so the two runners can hold each other off.
LOCK = Path("/tmp/kiss-sim-gate.lock")
LOCK_STALE = 900


@contextlib.contextmanager
def gate_lock():
    waited = False
    while True:
        try:
            LOCK.mkdir()
            break
        except FileExistsError:
            try:
                age = time.time() - LOCK.stat().st_mtime
            except OSError:
                continue
            if age > LOCK_STALE:
                print(f"note: removing a stale gate lock ({int(age)}s old)",
                      file=sys.stderr)
                try:
                    LOCK.rmdir()
                except OSError:
                    pass
                continue
            if not waited:
                print("waiting for another gate run to finish...", file=sys.stderr)
                waited = True
            time.sleep(2)
    try:
        yield
    finally:
        try:
            LOCK.rmdir()
        except OSError:
            pass

# Screen constructors are not signature-compatible: setup's mk_screen takes
# (title, sub), while signing's takes (parent, title, sub). Treating the name as
# globally arg0 left every signing screen outside the gate. Registered wrappers
# are scanned at their call sites; their one forwarding implementation is then
# ignored below so a variable named `title` is not mistaken for an unknown
# screen.
BASE_CALLS = (("wt_screen", 1),)
FILE_CALLS = {
    "kiss_setup.c": (("mk_screen", 0), ("mk_screen2", 0),
                     ("cards_verdict_screen", 0)),
    "kiss_sign.c": (("mk_screen", 1),),
    "kiss_fw_ui.c": (("fresh", 0),),
}

# (file, callee, normalized title expression) -> keys chosen in the enclosing
# function. Each declaration must be consumed exactly once or strict mode
# fails: a stale exception is only another silent hole.
DYNAMIC_TITLES = {
    ("kiss_settings.c", "wt_screen", "title"): {
        "G_STORAGE_OK_T", "G_STORAGE_CLEANUP_T", "G_STORAGE_FAIL_T",
    },
    ("kiss_fw_ui.c", "fresh", "title"): {
        "G_FW_OK_T", "G_FW_FAIL_T",
    },
}

# Verified forwarding bodies. Their callers are covered by FILE_CALLS above.
FORWARDER_IMPLS = {
    ("kiss_setup.c", "wt_screen", "title"),
    ("kiss_sign.c", "wt_screen", "title"),
    ("kiss_fw_ui.c", "wt_screen", "title"),
    ("kiss_setup.c", "mk_screen", "tr(title)"),
}
KEY = re.compile(r"\bSTR_([A-Z0-9_]+)\b")
IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


def c_mask(text):
    """Blank C strings/comments while preserving positions and newlines."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i + 2)
            if j < 0:
                j = n
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif text[i] in ('"', "'"):
            quote = text[i]
            out[i] = " "
            i += 1
            while i < n:
                c = text[i]
                if c == "\\" and i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                if c == quote:
                    out[i] = " "
                    i += 1
                    break
                if c != "\n":
                    out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def split_args(text):
    """Top level comma split, so tr(STR_X) stays in one piece."""
    out, depth, start = [], 0, 0
    masked = c_mask(text)
    for i, ch in enumerate(masked):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(text[start:i])
            start = i + 1
    out.append(text[start:])
    return out


def iter_calls(src, name):
    """Yield (args, line, is_definition) for balanced C calls named `name`."""
    masked = c_mask(src)
    pattern = re.compile(rf"\b{re.escape(name)}\s*\(")
    for match in pattern.finditer(masked):
        open_at = masked.find("(", match.start())
        depth, i = 1, open_at + 1
        while i < len(masked) and depth:
            if masked[i] == "(":
                depth += 1
            elif masked[i] == ")":
                depth -= 1
            i += 1
        if depth:
            continue
        end = i - 1
        j = i
        while j < len(masked) and masked[j].isspace():
            j += 1
        yield src[open_at + 1:end], src[:match.start()].count("\n") + 1, \
              j < len(masked) and masked[j] == "{"


def titles_in_source():
    """Translated keys, literal titles, and genuinely unresolved call sites."""
    found, literals, murky = {}, {}, []
    dynamic_hits = {decl: 0 for decl in DYNAMIC_TITLES}
    for path in sorted((ROOT / "main").glob("*.c")):
        if path.name in ("kiss_theme.c", "i18n_tables.c"):
            continue          # the definition itself, and the generated table
        src = path.read_text(encoding="utf-8", errors="replace")
        specs = BASE_CALLS + FILE_CALLS.get(path.name, ())
        for callee, argno in specs:
            for arg_text, line, is_definition in iter_calls(src, callee):
                if is_definition:
                    continue
                args = split_args(arg_text)
                if len(args) <= argno:
                    murky.append(f"{path.relative_to(ROOT)}:{line} ({callee}: no title arg)")
                    continue
                title_expr = args[argno].strip()
                normalized = re.sub(r"\s+", "", title_expr)
                site = (path.name, callee, normalized)
                if site in FORWARDER_IMPLS:
                    continue
                keys = KEY.findall(title_expr)
                if not keys:
                    dynamic = DYNAMIC_TITLES.get(site)
                    if dynamic:
                        dynamic_hits[site] += 1
                        keys = sorted(dynamic)
                    else:
                        literal = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', title_expr)
                        if literal:
                            text = bytes(literal.group(1), "utf-8").decode("unicode_escape")
                            literals.setdefault(text, []).append(path.name)
                            continue
                        murky.append(f"{path.relative_to(ROOT)}:{line} "
                                     f"({callee} title={title_expr!r})")
                        continue
                for k in keys:
                    found.setdefault(k, []).append(path.name)

    for site, count in dynamic_hits.items():
        if count != 1:
            murky.append(f"dynamic declaration {site} consumed {count} times")
    return found, literals, murky


def drive_sim(**extra_env):
    """Run the walk once and hand back its stdout, or die saying why not.

    The return code and the walk's own end marker are BOTH checked, and the
    reason is the failure this script exists to prevent, one level up. A sim
    that dies halfway prints no BUILT lines, and a caller that only reads
    stdout cannot tell that from a walk that opened nothing -- so the report
    became "every screen is uncovered", 40 rows of it, with no hint that the
    binary never ran. Under STRICT that is a red build nobody can read.

    It happens for dull reasons: a concurrent build_sim.sh rewriting the binary
    mid-run, or the shared /tmp state kisstest and the walk both own. Neither
    is a screen with no stop, and neither should be reported as one.
    """
    if not SIM.exists():
        sys.exit(f"{SIM} is missing: run bash sim/build_sim.sh first")
    run = subprocess.run([str(SIM)], capture_output=True, text=True,
                         env=dict(os.environ, **extra_env))
    if run.returncode != 0 or "sim done" not in run.stdout:
        how = (f"exit {run.returncode}" if run.returncode
               else "exited 0 without reaching the end of the walk")
        sys.exit(f"FAILED: {SIM} {how} with {' '.join(extra_env)} set. The walk "
                 f"did not finish, so its screen list means nothing.\n"
                 f"--- last 20 lines of the walk ---\n"
                 + "\n".join(run.stdout.splitlines()[-20:])
                 + ("\n--- stderr ---\n" + run.stderr[-2000:] if run.stderr else ""))
    return run.stdout


def walk_report():
    """One walk run: (built titles, UNCHECKED lines).

    The UNCHECKED lines are the walk's own "built but never captured" report
    (sim_main.c): a screen the walk OPENED and never photographed. It caught
    a real miss -- a firmware-rejected stop saved its frame before the
    deferred result screen landed, so the walk reached the screen and no gate
    asked it anything. That report used to exist only in a log nobody parsed,
    one level down from the gates this script already complains about.
    """
    out = drive_sim(SCREENCOVER_LIST="1")
    built = {l.split("\t", 1)[1] for l in out.splitlines() if l.startswith("BUILT\t")}
    unchecked = [l.strip() for l in out.splitlines() if l.startswith("  UNCHECKED")]
    return built, unchecked


def safe_boot_report():
    """Literal titles reached only by a pre-game boot failure.

    wt_screen's ordinary registry intentionally keys translated table pointers,
    so a literal title cannot appear in BUILT/UNCHECKED. Drive every non-OK
    settings-load status in a fresh process and consume the explicit marker the
    focused harness prints after checking the title, cause, and absence of the
    game screen.
    """
    seen = set()
    for status in range(1, 6):
        out = drive_sim(SCREENCOVER_SAFEBOOT=str(status))
        rows = [line.split("\t") for line in out.splitlines()
                if line.startswith("SAFEBOOT\t")]
        if len(rows) != 1 or len(rows[0]) < 3:
            sys.exit(f"FAILED: safe-boot status {status} produced no unique marker")
        seen.add(rows[0][1])
    return seen


def selftest():
    """Prove the 'built but never captured' half can still fire.

    Same rule run_overlapcheck.sh applies to WALL and ROLE: a count of zero
    means nothing unless the check still reports something when it should. The
    sim builds a screen titled OK and never saves it; that must come back.
    """
    out = drive_sim(SCREENCOVER_SELFTEST="1")
    return any(l.startswith("  UNCHECKED") and '"OK"' in l for l in out.splitlines())


def parser_selftest():
    """Definitions, nested calls, strings and arg positions stay distinguishable."""
    src = '''
      static void mk_screen(lv_obj_t *parent, const char *title) { helper(); }
      void f(void) { mk_screen(parent, tr(STR_DEMO), "comma, (inside)"); }
    '''
    calls = [(split_args(args), is_def)
             for args, _line, is_def in iter_calls(src, "mk_screen")]
    return (len(calls) == 2 and calls[0][1] and not calls[1][1] and
            len(calls[1][0]) == 3 and "STR_DEMO" in calls[1][0][1])


def main():
    if not parser_selftest():
        print("FAILED: the source-call parser self test no longer distinguishes "
              "definitions, nested arguments and C strings.")
        return 1
    if not selftest():
        print("FAILED: the coverage self test no longer reports, so a clean run "
              "means nothing.")
        return 1

    found, literals, murky = titles_in_source()
    built, unchecked = walk_report()
    safe_built = safe_boot_report()

    # The source gives keys, the sim gives English strings, so meet in the
    # middle through the same table the firmware reads.
    # FIRST occurrence only. The generated file holds 21 tables one after
    # another and every key appears in all of them, so dict() over the whole
    # file silently hands back Croatian.
    tables = (ROOT / "main" / "i18n_tables.c").read_text(encoding="utf-8")
    en = {}
    for key, text in re.findall(r'\[STR_([A-Z0-9_]+)\]\s*=\s*"((?:[^"\\]|\\.)*)"', tables):
        en.setdefault(key, text)

    missing, unknown_keys = [], []
    for key in sorted(found):
        text = en.get(key)
        if text is None:
            unknown_keys.append(key)
            continue
        if text.encode().decode("unicode_escape") not in built and text not in built:
            missing.append((key, text, sorted(set(found[key]))))
    missing_literals = sorted(set(literals) - safe_built)

    print(f"screen coverage: {len(found)} titles in source, {len(built)} opened by the walk")
    for key, text, files in missing:
        print(f'  NEVER OPENED  STR_{key}  "{text}"  ({", ".join(files)})')
    if unchecked:
        print(f"screen coverage: {len(unchecked)} built and never captured:")
        for l in unchecked:
            print(f"  {l}")
    for title in missing_literals:
        print(f'  NEVER OPENED  literal "{title}"  '
              f'({", ".join(sorted(set(literals[title])))})')
    for key in unknown_keys:
        print(f"  UNKNOWN KEY  STR_{key} (not in the English table)")
    if murky:
        print(f"  {len(murky)} unresolved title construction site(s):")
        for site in murky:
            print(f"    {site}")
    print(f"screen coverage: {len(missing)} translated and "
          f"{len(missing_literals)} literal screens never opened; "
          f"{len(murky)} unresolved sites")

    if (missing or missing_literals or unchecked or murky or unknown_keys) and \
       os.environ.get("SCREENCOVER_STRICT"):
        print(f"\nFAILED: SCREENCOVER_STRICT is set and a screen has no honest stop: "
              f"{len(missing)} translated never opened, "
              f"{len(missing_literals)} literal never opened, "
              f"{len(unchecked)} built but never captured, "
              f"{len(murky)} unresolved, {len(unknown_keys)} unknown keys.")
        return 1
    return 0


if __name__ == "__main__":
    with gate_lock():
        sys.exit(main())
