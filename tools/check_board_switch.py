#!/usr/bin/env python3
"""A board test that names one board and lets the rest fall through.

    python3 tools/check_board_switch.py
    python3 tools/check_board_switch.py --selftest

The tree built two boards for a while, and "is it the 3.5in? else the Guition"
was a true sentence about it in some thirty places: `#ifdef KISS_BOARD_WS35 ...
#else`, `case ws35) ... *)` with the Guition as the star, `if [ "$KISS_BOARD"
= ws35 ]`. Every one of them was right until a third board arrived, and then
every one of them handed the new board the Guition's arm without a word -- a
portrait framebuffer map on a landscape panel, the Guition's ceilings for
another board's lanes, the Guition's sdkconfig under another board's name.

main/kiss_board.h is the one place that asks which board this is. Everything
else asks the question it actually has through a capability macro the header
defines for every board, or lists every board it knows and stops on the rest.
This holds that line, in the three languages that choose a board:

  C (main/, sim/)  A preprocessor chain that names a board -- KISS_BOARD_<ID> or
                   CONFIG_KISS_BOARD_<ID> -- outside main/kiss_board.h must end
                   in `#else` followed directly by `#error`.
  CMake            An if() chain that names CONFIG_KISS_BOARD_<ID> must end in
                   else() followed directly by message(FATAL_ERROR ...); and
                   `KISS_BOARD STREQUAL` may only compare against "guition", the
                   one board whose profile is the committed sdkconfig.
  shell            A `case` on $KISS_BOARD needs a `*)` arm that exits or
                   returns non-zero; `[ "$KISS_BOARD" = <id> ]` may only name
                   guition or the pseudo-id all.

An assertion over a VARIABLE board (CONFIG_KISS_BOARD_${id}) names no board and
is not a switch, so it passes. Exit 1 on any finding.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = "main/kiss_board.h"

C_BOARD = re.compile(r"\b(?:CONFIG_)?KISS_BOARD_[A-Z0-9]+\b")
C_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif|error)\b")
CM_BOARD = re.compile(r"\bCONFIG_KISS_BOARD_[A-Z0-9]+\b")
CM_OPEN = re.compile(r"^\s*if\s*\(", re.I)
CM_ELIF = re.compile(r"^\s*elseif\s*\(", re.I)
CM_ELSE = re.compile(r"^\s*else\s*\(", re.I)
CM_END = re.compile(r"^\s*endif\s*\(", re.I)
CM_STREQ = re.compile(r'\bKISS_BOARD\s+STREQUAL\s+"?([A-Za-z0-9_]+)"?')
SH_CASE = re.compile(r'^\s*case\s+"?\$\{?KISS_BOARD\b')
SH_TEST = re.compile(r'"\$\{?KISS_BOARD[^"]*"\s*!?==?\s*"?([A-Za-z0-9_]+)"?')
SH_OK_IDS = {"guition", "all"}


def c_findings(path, text):
    if path == HEADER:
        return []
    out = []
    lines = text.split("\n")
    stack = []          # per open chain: [start line, names a board, else line]
    want_error = None   # chain whose #else must be followed by #error
    in_block = False
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if not s or s.startswith("//"):
            continue
        if s.startswith("/*"):
            in_block = "*/" not in s
            continue
        m = C_DIRECTIVE.match(raw)
        if want_error is not None:
            if not (m and m.group(1) == "error"):
                out.append(f"{path}:{want_error[0]}: a board test's #else (line "
                           f"{want_error[1]}) is not followed by #error")
            want_error = None
        if not m:
            continue
        kind = m.group(1)
        if kind in ("if", "ifdef", "ifndef"):
            stack.append([n, bool(C_BOARD.search(raw)), None])
        elif kind == "elif" and stack:
            stack[-1][1] = stack[-1][1] or bool(C_BOARD.search(raw))
        elif kind == "else" and stack:
            stack[-1][2] = n
            if stack[-1][1]:
                want_error = (stack[-1][0], n)
        elif kind == "endif" and stack:
            start, board, els = stack.pop()
            if board and els is None:
                out.append(f"{path}:{start}: a board test with no #else #error: "
                           "a board it does not name falls through")
    return out


def cmake_findings(path, text):
    out = []
    stack = []
    want_fatal = None
    for n, raw in enumerate(text.split("\n"), 1):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        if want_fatal is not None:
            if "FATAL_ERROR" not in s:
                out.append(f"{path}:{want_fatal[0]}: a board test's else() (line "
                           f"{want_fatal[1]}) is not followed by message(FATAL_ERROR)")
            want_fatal = None
        for m in CM_STREQ.finditer(raw):
            if m.group(1) != "guition":
                out.append(f'{path}:{n}: KISS_BOARD STREQUAL "{m.group(1)}": name '
                           "every board or none; only guition is special")
        if CM_OPEN.match(raw):
            stack.append([n, bool(CM_BOARD.search(raw)), None])
        elif CM_ELIF.match(raw) and stack:
            stack[-1][1] = stack[-1][1] or bool(CM_BOARD.search(raw))
        elif CM_ELSE.match(raw) and stack:
            stack[-1][2] = n
            if stack[-1][1]:
                want_fatal = (stack[-1][0], n)
        elif CM_END.match(raw) and stack:
            start, board, els = stack.pop()
            if board and els is None:
                out.append(f"{path}:{start}: a board test with no else() "
                           "FATAL_ERROR: a board it does not name falls through")
    return out


def shell_findings(path, text):
    out = []
    lines = text.split("\n")
    for n, raw in enumerate(lines, 1):
        code = raw.split(" #")[0] if not raw.lstrip().startswith("#") else ""
        for m in SH_TEST.finditer(code):
            if m.group(1) not in SH_OK_IDS:
                out.append(f'{path}:{n}: a test for KISS_BOARD = {m.group(1)}: its '
                           "else is every other board; list them in a case")
        if not SH_CASE.match(code):
            continue
        ok = False
        for j in range(n, len(lines)):
            arm = lines[j]
            if re.match(r"^\s*esac\b", arm):
                break
            if re.match(r"^\s*\*\)", arm):
                body = arm
                k = j
                while ";;" not in body and k + 1 < len(lines):
                    k += 1
                    body += "\n" + lines[k]
                ok = bool(re.search(r"\b(?:exit|return)\s+[1-9]", body))
        if not ok:
            out.append(f"{path}:{n}: a case on KISS_BOARD with no *) arm that "
                       "stops: a board it does not list falls through")
    return out


def tracked():
    r = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                       text=True, check=True)
    return r.stdout.split()


def scan(files, read):
    out = []
    for f in files:
        if os.path.dirname(f) in ("main", "sim") and f.endswith((".c", ".h")):
            out += c_findings(f, read(f))
        elif os.path.basename(f) == "CMakeLists.txt" and f.count("/") <= 1:
            out += cmake_findings(f, read(f))
        elif f.endswith(".sh"):
            out += shell_findings(f, read(f))
    return out


def selftest():
    bad_c = "#ifdef KISS_BOARD_WS35\nint a;\n#else\nint b;\n#endif\n"
    good_c = ("#if defined(KISS_BOARD_WS35)\nint a;\n#elif defined(KISS_BOARD_GUITION)\n"
              "int b;\n#else\n// why\n#error \"no\"\n#endif\n")
    alone_c = "#ifdef KISS_BOARD_WS35\nint a;\n#endif\n"
    cap_c = "#if KISS_PANEL_SPI\nint a;\n#else\nint b;\n#endif\n"
    assert c_findings("main/x.c", bad_c), "C fall-through not caught"
    assert c_findings("main/x.c", alone_c), "C one-board block not caught"
    assert not c_findings("main/x.c", good_c), "C named chain flagged"
    assert not c_findings("main/x.c", cap_c), "C capability test flagged"
    assert not c_findings(HEADER, bad_c), "the header is the one place allowed"
    bad_cm = "if(CONFIG_KISS_BOARD_WS35)\n  set(a 1)\nelse()\n  set(a 2)\nendif()\n"
    good_cm = ("if(CONFIG_KISS_BOARD_WS35)\n  set(a 1)\nelseif(CONFIG_KISS_BOARD_GUITION)\n"
               "  set(a 2)\nelse()\n  message(FATAL_ERROR \"no\")\nendif()\n")
    var_cm = "if(NOT CONFIG_KISS_BOARD_${b})\n  message(FATAL_ERROR x)\nendif()\n"
    assert cmake_findings("CMakeLists.txt", bad_cm), "CMake fall-through not caught"
    assert not cmake_findings("CMakeLists.txt", good_cm), "CMake named chain flagged"
    assert not cmake_findings("CMakeLists.txt", var_cm), "CMake assertion flagged"
    assert cmake_findings("CMakeLists.txt", 'if(KISS_BOARD STREQUAL "ws35")\nendif()\n')
    assert not cmake_findings("CMakeLists.txt", 'if(NOT KISS_BOARD STREQUAL "guition")\nendif()\n')
    bad_sh = 'case "$KISS_BOARD" in\n  ws35) a=1 ;;\n  *) a=2 ;;\nesac\n'
    good_sh = 'case "${KISS_BOARD:-guition}" in\n  ws35) a=1 ;;\n  *) echo no; exit 1 ;;\nesac\n'
    assert shell_findings("x.sh", bad_sh), "shell fall-through not caught"
    assert not shell_findings("x.sh", good_sh), "shell listed case flagged"
    assert shell_findings("x.sh", 'if [ "$KISS_BOARD" = ws35 ]; then :; fi\n')
    assert not shell_findings("x.sh", 'if [ "$KISS_BOARD" != "guition" ]; then :; fi\n')
    print("check_board_switch selftest: ok")


def main():
    if "--selftest" in sys.argv[1:]:
        selftest()
    files = tracked()
    out = scan(files, lambda f: open(os.path.join(ROOT, f), encoding="utf-8",
                                     errors="replace").read())
    for line in out:
        print(line)
    print(f"board switches: {len(out)} finding(s)")
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main())
