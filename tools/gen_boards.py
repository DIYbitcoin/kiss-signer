#!/usr/bin/env python3
"""Write the install page's board list from docs/boards.json.

The rows between the boards:start and boards:end markers in docs/index.html
are generated here, so adding a board is adding one entry to the JSON. The
JSON only labels boards; what can be flashed is still decided by the signed
docs/installer/release.json, and this tool refuses a release board that has no
entry, or an entry marked release that the release does not carry.

Usage:
    python3 tools/gen_boards.py           rewrite the rows in docs/index.html
    python3 tools/gen_boards.py --check   exit 1 if the rows are out of date
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "docs" / "boards.json"
INDEX = ROOT / "docs" / "index.html"
RELEASE = ROOT / "docs" / "installer" / "release.json"

BLOCK = re.compile(r"(<!-- boards:start[^>]*-->\n)(.*?)(^[ \t]*<!-- boards:end -->)", re.S | re.M)
INDENT = "          "


def label(b):
    w, h = b["screen"]
    # "an 800", "a 1024": the article follows how the number is said.
    article = "an" if str(w)[0] == "8" or str(w).startswith("11") or str(w).startswith("18") else "a"
    return (f'<b>{b["name"]}</b>, the {b["model"]}, with {article} {w} by {h} screen. '
            f'This release is for {b["chips"]} chips only.')


def maker_list(boards):
    """Waveshare 4.3in or 5in: one maker's boards share its name once."""
    groups = []
    for b in boards:
        maker, _, size = b["name"].partition(" ")
        if groups and groups[-1][0] == maker:
            groups[-1][1].append(size)
        else:
            groups.append((maker, [size]))
    parts = [f"{maker} {' or '.join(sizes)}" for maker, sizes in groups]
    return ", ".join(parts)


def render(boards):
    out = []
    for b in boards:
        if b["build"] != "release":
            continue
        out += [
            f'{INDENT}<label class="ack-row">',
            f'{INDENT}  <input type="radio" name="board" value="{b["id"]}">',
            f'{INDENT}  <span>{label(b)}</span>',
            f'{INDENT}</label>',
        ]
    tests = [b for b in boards if b["build"] == "test"]
    if tests:
        out.append(f'{INDENT}<p class="hint">{maker_list(tests)}, or a board with a v3.x chip? '
                   'Those have unsigned test builds, not this release, on the '
                   '<a href="test/">test installer</a>.</p>')
    return "\n".join(out) + "\n"


def problems(boards):
    found = []
    ids = [b["id"] for b in boards]
    if len(ids) != len(set(ids)):
        found.append("docs/boards.json lists a board id twice")
    for b in boards:
        if b.get("build") not in ("release", "test"):
            found.append(f"board {b.get('id')!r}: build must be release or test")
    try:
        release = json.loads(RELEASE.read_text())
    except (OSError, ValueError) as exc:
        return found + [f"cannot read {RELEASE.relative_to(ROOT)}: {exc}"]
    carried = {b.get("id") for b in release.get("boards", [])}
    listed = {b["id"] for b in boards if b.get("build") == "release"}
    for missing in sorted(carried - listed):
        found.append(f"release.json carries {missing!r}, which docs/boards.json does not list as release")
    for extra in sorted(listed - carried):
        found.append(f"docs/boards.json marks {extra!r} release, which release.json does not carry")
    return found


def main():
    check = "--check" in sys.argv[1:]
    boards = json.loads(DATA.read_text())["boards"]
    bad = problems(boards)
    page = INDEX.read_text()
    m = BLOCK.search(page)
    if not m:
        bad.append("docs/index.html has no boards:start and boards:end markers")
    if bad:
        for p in bad:
            print("FAIL:", p)
        return 1
    want = render(boards)
    if m.group(2) == want:
        print(f"PASS: the install page lists the boards in docs/boards.json")
        return 0
    if check:
        print("FAIL: the board list in docs/index.html is out of date. Run:\n"
              "    python3 tools/gen_boards.py")
        return 1
    INDEX.write_text(page[:m.start(2)] + want + page[m.end(2):])
    print("wrote the board list in docs/index.html")
    return 0


if __name__ == "__main__":
    sys.exit(main())
