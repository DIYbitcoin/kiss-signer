#!/usr/bin/env python3
"""Apply a {locale: {key: text}} patch to i18n/*.json in place.

Key order and file formatting are preserved: only the values of existing keys
are replaced, so a patch can never reorder or introduce a key (gen_i18n.py
would reject that anyway, but failing here is a clearer error).
"""
import json
import pathlib
import sys
from collections import OrderedDict

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT / "i18n"


def apply(patch, allow_new=False):
    en = json.loads((I18N / "en.json").read_text(encoding="utf-8"),
                    object_pairs_hook=OrderedDict)
    for loc, kv in patch.items():
        path = I18N / f"{loc}.json"
        data = json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=OrderedDict)
        for k, v in kv.items():
            if k not in data:
                if not allow_new:
                    sys.exit(f"{loc}: unknown key {k}")
                if k not in en:
                    sys.exit(f"{loc}: {k} is not in en.json either")
            data[k] = v
        # keep every locale in en.json's order, so a diff between two locales
        # lines up and gen_i18n's key-set check reads cleanly
        ordered = OrderedDict((k, data[k]) for k in en if k in data)
        for k in data:                       # anything en dropped stays visible
            ordered.setdefault(k, data[k])
        path.write_text(json.dumps(ordered, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
        print(f"{loc}: {len(kv)} keys")


if __name__ == "__main__":
    args = sys.argv[1:]
    allow_new = "--new" in args           # a patch that introduces keys
    for arg in [a for a in args if a != "--new"]:
        apply(json.loads(pathlib.Path(arg).read_text(encoding="utf-8")), allow_new)
