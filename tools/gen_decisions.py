#!/usr/bin/env python3
"""Collect the REVERSALS out of the source and write dev/decisions.md.

Every screen on this device carries its argument in a comment above the code
that draws it, and that has been enough for whoever is already reading that
file. It is not enough for anyone looking at a FRAME: a reviewer with a
screenshot has no route to the comment, so the same wrong conclusions get filed
again and again. Seven were filed and withdrawn in a single review pass, and
every one of them was answered a few lines above the builder.

So the comments stay where they are -- they are the source of truth and a
second hand written copy of them would go stale the first time one changed --
and this walks the tree for the ones marked `// DECIDED:` and writes an
addressable index of them.

REVERSALS ONLY. The marker means "X was tried, it was wrong, and Y is why",
which is the kind of comment a reviewer needs and cannot guess. A marker on
every interesting comment would produce a document nobody reads, which is the
same as not having one.

    // DECIDED: the KEYS tab strip lost THIS SIGNER. A tab whose whole content
    // is a read only copy of another page is a tab an owner has to check
    // twice.

The marker line and every comment line under it, to the first line that is not
a comment. The first sentence becomes the heading.

    python3 tools/gen_decisions.py            # write dev/decisions.md
    python3 tools/gen_decisions.py --check    # fail if it is out of date
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "dev", "decisions.md")
# Where a decision can live. The kit and the screens; not the gates, whose own
# arguments are about the gate rather than about the product.
DIRS = ("main", "sim")
EXTS = (".c", ".h")

HEAD = """# Decisions

Reversals: something was tried on this device, it was wrong, and this is why
what ships is what ships. Generated from `// DECIDED:` comments by
`tools/gen_decisions.py` -- the comment beside the code is the source of truth
and this is an index of it, so a stale entry here is impossible by
construction.

**Read this before filing a defect from a screenshot.** A frame gives you no
route to the comment that answers it, which is how seven findings were filed
and withdrawn in one review pass.
"""


def collect():
    out = []
    for d in DIRS:
        base = os.path.join(ROOT, d)
        for name in sorted(os.listdir(base)):
            if not name.endswith(EXTS):
                continue
            path = os.path.join(base, name)
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            i = 0
            while i < len(lines):
                m = re.match(r"^(\s*)// DECIDED:\s*(.*)$", lines[i])
                if not m:
                    i += 1
                    continue
                indent, first = m.group(1), m.group(2).strip()
                body = [first]
                j = i + 1
                while j < len(lines):
                    c = re.match(r"^\s*//\s?(.*)$", lines[j])
                    if not c:
                        break
                    body.append(c.group(1).rstrip())
                    j += 1
                out.append({
                    "file": "%s/%s" % (d, name),
                    "line": i + 1,
                    "body": body,
                })
                i = j
    return out


def render(items):
    doc = [HEAD]
    doc.append("\n%d decisions.\n" % len(items))
    cur = None
    for it in items:
        if it["file"] != cur:
            cur = it["file"]
            doc.append("\n## `%s`\n" % cur)
        text = " ".join(x for x in it["body"] if x).strip()
        # The heading is the first sentence, which is what the marker line is
        # written to be. The rest is the argument.
        parts = re.split(r"(?<=\.)\s+", text, maxsplit=1)
        head = parts[0].rstrip(".")
        rest = parts[1] if len(parts) > 1 else ""
        doc.append("\n### %s\n" % head)
        if rest:
            doc.append("\n%s\n" % rest)
        doc.append("\n[`%s:%d`](../%s#L%d)\n"
                   % (it["file"], it["line"], it["file"], it["line"]))
    return "".join(doc)


def main():
    items = collect()
    if not items:
        print("FATAL: no // DECIDED: markers found -- the extractor is dead, "
              "or the marker was renamed", file=sys.stderr)
        return 1
    text = render(items)
    if "--check" in sys.argv:
        try:
            with open(OUT, encoding="utf-8") as f:
                have = f.read()
        except FileNotFoundError:
            have = None
        if have != text:
            print("decisions: dev/decisions.md is out of date -- run "
                  "python3 tools/gen_decisions.py", file=sys.stderr)
            if have is not None:
                d = subprocess.run(["diff", "-u", OUT, "-"], input=text,
                                   capture_output=True, text=True)
                sys.stderr.write(d.stdout[:4000])
            return 1
        print("decisions: %d entries, dev/decisions.md up to date" % len(items))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("decisions: %d entries -> dev/decisions.md" % len(items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
