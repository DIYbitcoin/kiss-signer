#!/usr/bin/env python3
"""The simulator's LVGL config still matches the device's.

sim/lv_conf.h is the LVGL v9.5.0 template with FIVE settings overridden. The
device never reads it -- sdkconfig sets CONFIG_LV_CONF_SKIP=y and the firmware's
LVGL is configured from Kconfig -- so each of the five has a counterpart over
there, and the two are a pair. Raise CONFIG_LV_MEM_SIZE_KILOBYTES and the
simulator keeps its old pool: nothing fails, nothing warns, and every heap
number measured in the sim quietly stops describing the device. That number is
load bearing right now, with LVGL sitting near its ceiling, and it is exactly
the kind of drift a comment cannot hold.

The comment at the top of sim/lv_conf.h says all this. This is the mechanism,
because the house rule about writing it down is that writing it down does not
work.

Two questions, both answered against files that already ship in this tree:

  MISMATCH   an override disagrees with its sdkconfig counterpart.
  UNEXPECTED sim/lv_conf.h diverges from the template somewhere the list below
             does not mention -- a sixth override nobody recorded, or a bump
             that re-vendored the template and left a setting behind.

The second is why this reads the template rather than hardcoding five values:
a check that only knows what it was told cannot see what it was not.

    python3 tools/check_lv_conf.py
    LVCONF_SELFTEST=1 python3 tools/check_lv_conf.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = "managed_components/lvgl__lvgl/lv_conf_template.h"
SIMCONF = "sim/lv_conf.h"
SDKCONFIG = "sdkconfig"

# The five, and how each maps onto Kconfig. A bool is "y" on the device and 1 in
# the header; LV_MEM_SIZE is bytes here and kilobytes there.
OVERRIDES = {
    "LV_MEM_SIZE": ("CONFIG_LV_MEM_SIZE_KILOBYTES", "kb"),
    "LV_FONT_MONTSERRAT_28": ("CONFIG_LV_FONT_MONTSERRAT_28", "bool"),
    "LV_FONT_MONTSERRAT_40": ("CONFIG_LV_FONT_MONTSERRAT_40", "bool"),
    "LV_FONT_MONTSERRAT_48": ("CONFIG_LV_FONT_MONTSERRAT_48", "bool"),
    "LV_USE_QRCODE": ("CONFIG_LV_USE_QRCODE", "bool"),
}

DEFINE = re.compile(r"^\s*#\s*define\s+(LV_[A-Z0-9_]+)\s+(.*)$")


def read(rel):
    """The file, or a plain word about the one that is not in a fresh clone."""
    path = os.path.join(ROOT, rel)
    if rel == TEMPLATE and not os.path.exists(path):
        print(f"{TEMPLATE} is not here. managed_components/ is gitignored and\n"
              "the IDF component manager populates it during a device build, so\n"
              "a fresh clone has no LVGL yet. Build the sim once, or clone the\n"
              "version dependencies.lock pins, then run this again.", file=sys.stderr)
        sys.exit(2)
    with open(path, encoding="utf-8") as f:
        return f.read()


def defines(text):
    """{name: value} for every #define LV_*, trailing comments stripped."""
    out = {}
    for line in text.splitlines():
        m = DEFINE.match(line)
        if not m:
            continue
        val = m.group(2)
        for cut in ("/*", "//"):
            if cut in val:
                val = val.split(cut)[0]
        out[m.group(1)] = " ".join(val.split())
    return out


def kconfig(text):
    """{CONFIG_LV_*: value} from sdkconfig, unset lines ignored."""
    out = {}
    for line in text.splitlines():
        if line.startswith("CONFIG_LV_") and "=" in line:
            k, v = line.split("=", 1)
            out[k] = v.strip()
    return out


def number(val):
    """The integer in a value like `(128 * 1024U)` or `128`, else None."""
    nums = re.findall(r"\d+", val)
    return int(nums[0]) if nums else None


def check(tmpl_txt, sim_txt, sdk_txt):
    """Returns (mismatch, unexpected) as lists of printable lines."""
    tmpl, sim, sdk = defines(tmpl_txt), defines(sim_txt), kconfig(sdk_txt)
    mismatch, unexpected = [], []

    for name, (key, kind) in OVERRIDES.items():
        if name not in sim:
            mismatch.append(f"{name}: gone from {SIMCONF}")
            continue
        have, want = sim[name], sdk.get(key)
        if want is None:
            mismatch.append(f"{name}: {key} is not set in {SDKCONFIG}")
        elif kind == "bool":
            if number(have) != 1 or want != "y":
                mismatch.append(f"{name} = {have}  but  {key}={want}")
        else:
            kb = number(have)
            kb = kb // 1024 if kb and kb >= 1024 else kb
            if str(kb) != want:
                mismatch.append(f"{name} = {have} ({kb}K)  but  {key}={want}")

    for name, val in sim.items():
        if name in OVERRIDES or name not in tmpl:
            continue
        if tmpl[name] != val:
            unexpected.append(f"{name}: template {tmpl[name]!r} -> sim {val!r}")

    return mismatch, unexpected


def selftest(tmpl_txt, sim_txt, sdk_txt):
    """Both checks still fire, and both stay quiet on the real files."""
    broken = []

    if check(tmpl_txt, sim_txt, sdk_txt) != ([], []):
        broken.append("SELFTEST: the real tree is not clean; fix that first")

    # MISMATCH: the sim keeps a 64K pool while the device asks for 128K, which
    # is the drift this exists for.
    bent = sim_txt.replace("LV_MEM_SIZE (128 * 1024U)", "LV_MEM_SIZE (64 * 1024U)")
    if bent == sim_txt:
        broken.append("SELFTEST: LV_MEM_SIZE no longer reads as 128K; the case is dead")
    elif not check(tmpl_txt, bent, sdk_txt)[0]:
        broken.append("SELFTEST: MISMATCH no longer fires on a pool that disagrees")

    # UNEXPECTED: a sixth override, recorded nowhere.
    sixth = sim_txt.replace("#define LV_USE_LOG 0", "#define LV_USE_LOG 1")
    if sixth == sim_txt:
        broken.append("SELFTEST: LV_USE_LOG is not 0 in the template pair; pick another")
    elif not check(tmpl_txt, sixth, sdk_txt)[1]:
        broken.append("SELFTEST: UNEXPECTED no longer fires on a sixth override")

    # And it must not fire on the five that ARE recorded.
    if check(tmpl_txt, sim_txt, sdk_txt)[1]:
        broken.append("SELFTEST: UNEXPECTED fires on an override that is listed")

    for line in broken:
        print(line, file=sys.stderr)
    print(f"lv_conf selftest: 3 cases, {len(broken)} broken")
    return 1 if broken else 0


def main():
    tmpl_txt, sim_txt, sdk_txt = read(TEMPLATE), read(SIMCONF), read(SDKCONFIG)

    if os.environ.get("LVCONF_SELFTEST"):
        if selftest(tmpl_txt, sim_txt, sdk_txt):
            print("SELFTEST FAILED: the check no longer fires; not reporting",
                  file=sys.stderr)
            return 1

    mismatch, unexpected = check(tmpl_txt, sim_txt, sdk_txt)

    for line in mismatch:
        print(f"MISMATCH   {line}", file=sys.stderr)
    for line in unexpected:
        print(f"UNEXPECTED {line}", file=sys.stderr)

    if mismatch:
        print(f"\n{SIMCONF} and {SDKCONFIG} disagree. The simulator is no longer\n"
              "standing in for the device; any heap or font measurement taken in\n"
              "it describes something that is not shipping.", file=sys.stderr)
    if unexpected:
        print(f"\n{SIMCONF} diverges from the template somewhere OVERRIDES does\n"
              "not name. Record it there with its Kconfig counterpart, or drop it.",
              file=sys.stderr)

    n = len(OVERRIDES)
    print(f"lv_conf: {n} overrides checked against {SDKCONFIG}, "
          f"{len(mismatch)} mismatched, {len(unexpected)} unrecorded")
    return 1 if (mismatch or unexpected) else 0


if __name__ == "__main__":
    sys.exit(main())
