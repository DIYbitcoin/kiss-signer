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


def apply(patch):
    for loc, kv in patch.items():
        path = I18N / f"{loc}.json"
        data = json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=OrderedDict)
        for k, v in kv.items():
            if k not in data:
                sys.exit(f"{loc}: unknown key {k}")
            data[k] = v
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
        print(f"{loc}: {len(kv)} keys")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        apply(json.loads(pathlib.Path(arg).read_text(encoding="utf-8")))
