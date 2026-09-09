#!/usr/bin/env python3
"""The seed path reaches the chip RNG through Espressif's own objects.

    check_rng_provenance.py <linker.map>
    check_rng_provenance.py --selftest

WHY. The Coldcard seed bug was not a weak RNG. The STM32 TRNG code was in the
binary and had been reviewed. A macro bound seed generation to MicroPython's
software PRNG instead, a function with the same signature, and for five years
nothing asked which of the two the seed path resolved to. Coinkite's own
post-mortem: review "verified that code but did not verify end-to-end symbol
resolution and call reachability from wallet seed generation".

That is a linker question, and the linker map answers it. Built with --cref,
the map lists for every symbol the object that DEFINES it first and every
object that REFERENCES it after. So the map can say, of the shipped image:

  * esp_fill_random and esp_random are Espressif's hw_random.c, and
    bootloader_random_enable is Espressif's bootloader_random_esp32p4.c --
    not something in main/ with the same name;
  * kiss_setup.c, the one place a seed comes into existence, references
    esp_fill_random, and so do the SD device key and the KEF IV writer;
  * kiss_crypto.c references bootloader_random_enable, the call that switches
    the SAR ADC noise source on (main/kiss_crypto.h says why that is needed
    at all), and main.c references kiss_trng_start, so boot makes that call;
  * nothing in main/ references bootloader_random_disable;
  * each of those has an address in the memory map, so the linker kept it.

WHAT IT CANNOT SAY. Whether the call ran, or whether the register it writes
came up. A reference in the map is a call compiled into the image, not a call
executed at boot. That half is a hardware question and stays one.

Every (symbol, object) pair below is a promise a named file makes. Move the
chip read out of kiss_setup.c and this fails until the list says where it
went. That is the point: the Coldcard rebinding was a refactor nobody meant.

Exit 0 when the map keeps every promise. A map with no cross reference table
is a failure, not a pass: it cannot answer the question.
"""
import re
import sys

# The object each RNG symbol must be DEFINED in. Any other definer, a file in
# main/ above all, is a same-signature substitute standing where the vendor's
# implementation belongs. Matched on the archive(member) tail, since the path
# in front of it is the build directory's.
PROVIDERS = {
    "esp_random":               "libesp_hw_support.a(hw_random.c.obj)",
    "esp_fill_random":          "libesp_hw_support.a(hw_random.c.obj)",
    "bootloader_random_enable":
        "libbootloader_support.a(bootloader_random_esp32p4.c.obj)",
}

# Who must REFERENCE what. The seed path, object by object:
#   main.c         -> kiss_trng_start           boot switches the source on
#   kiss_crypto.c  -> bootloader_random_enable  and this is the switch
#   kiss_setup.c   -> esp_fill_random           the seed fold's chip leg
#   camera_spike.c -> esp_fill_random           the chip read at capture
#   kiss_seed_sd.c, kiss_kef_crypto.c -> esp_fill_random   other key material
#   kiss_setup.c, kiss_seed_sd.c, kiss_kef_crypto.c -> kiss_trng_live
#                                               the provenance gate at each
REFERENCES = (
    ("kiss_trng_start",          "libmain.a(main.c.obj)"),
    ("bootloader_random_enable", "libmain.a(kiss_crypto.c.obj)"),
    ("esp_fill_random",          "libmain.a(kiss_setup.c.obj)"),
    ("esp_fill_random",          "libmain.a(camera_spike.c.obj)"),
    ("esp_fill_random",          "libmain.a(kiss_seed_sd.c.obj)"),
    ("esp_fill_random",          "libmain.a(kiss_kef_crypto.c.obj)"),
    ("kiss_trng_live",           "libmain.a(kiss_setup.c.obj)"),
    ("kiss_trng_live",           "libmain.a(kiss_seed_sd.c.obj)"),
    ("kiss_trng_live",           "libmain.a(kiss_kef_crypto.c.obj)"),
)

# Nothing in main may switch the source OFF. kiss_crypto.c: enabled once and
# never disabled, because disabling is only needed before the ADC or the radio
# and this firmware uses neither.
FORBIDDEN = (
    ("bootloader_random_disable", "libmain.a("),
)

# Symbols that must be IN the image, not only in the cross reference. A
# section the linker discarded still lists its referencers in the cref; what
# it does not have is an address in the memory map.
KEPT = ("esp_random", "esp_fill_random", "bootloader_random_enable",
        "kiss_trng_start")


def split(text):
    """(memory map, cross reference table), or (None, None) without a cref."""
    i = text.find("\nLinker script and memory map")
    j = text.find("\nCross Reference Table")
    if i < 0 or j < 0 or j < i:
        return None, None
    return text[i:j], text[j:]


def cref(text):
    """symbol -> [defining object, referencing object, ...].

    GNU ld prints the symbol and its definer on one line and each referencer
    indented below. A symbol wider than the column goes on a line of its own
    with the definer wrapped to the next indented line; that case lands the
    definer at index 0 as well.
    """
    out = {}
    cur = None
    for line in text.splitlines()[1:]:
        if not line.strip() or line.startswith("Symbol"):
            continue
        if line[0] not in " \t":
            parts = line.split()
            cur = parts[0]
            out[cur] = parts[1:2]
        elif cur is not None:
            out[cur].append(line.strip())
    return out


def kept(mem, sym):
    m = re.search(r"^\s+0x([0-9a-fA-F]+)\s+%s\s*$" % re.escape(sym), mem, re.M)
    return bool(m) and int(m.group(1), 16) != 0


def judge(text):
    """Every promise the map breaks, one line each. Empty means PASS."""
    mem, xref = split(text)
    if xref is None:
        return ["no Cross Reference Table in the map (the link did not run "
                "with --cref), so nothing here can be judged"]
    refs = cref(xref)
    fails = []

    for sym, provider in PROVIDERS.items():
        files = refs.get(sym)
        if not files:
            fails.append("%s: not in the map at all" % sym)
        elif not files[0].endswith(provider):
            fails.append("%s: defined in %s, not %s"
                         % (sym, files[0], provider))

    for sym, obj in REFERENCES:
        if not any(f.endswith(obj) for f in refs.get(sym, [])[1:]):
            fails.append("%s: not referenced by %s" % (sym, obj))

    for sym, prefix in FORBIDDEN:
        for f in refs.get(sym, [])[1:]:
            if prefix in f:
                fails.append("%s: referenced by %s, and nothing in main may "
                             "switch the source off" % (sym, f))

    for sym in KEPT:
        if not kept(mem, sym):
            fails.append("%s: not in the image (no address in the memory map)"
                         % sym)
    return fails


# ---- selftest -------------------------------------------------------------
# A map with the shape of the real one, cut to the lines this reads. Each case
# breaks one promise by swapping a whole cref entry or memory-map section
# for a broken one, and expects the matching line back. A swap whose anchor
# is missing raises, so a stale fixture cannot quietly test nothing.

HW = "esp-idf/esp_hw_support/libesp_hw_support.a(hw_random.c.obj)"
BL = ("esp-idf/bootloader_support/libbootloader_support.a"
      "(bootloader_random_esp32p4.c.obj)")
NVS = "esp-idf/nvs_sec_provider/libnvs_sec_provider.a(nvs_sec_provider.c.obj)"
STACK = "esp-idf/esp_system/libesp_system.a(stack_check.c.obj)"


def _main(obj):
    return "esp-idf/main/libmain.a(%s.obj)" % obj


def _entry(sym, definer, *referencers):
    lines = ["%-50s%s" % (sym, definer)]
    lines += ["%50s%s" % ("", r) for r in referencers]
    return "\n".join(lines) + "\n"


def _section(name, addr, size, obj, sym):
    return (" .text.%s\n%16s0x%08x %10s %s\n%16s0x%08x%16s%s\n"
            % (name, "", addr, "0x%x" % size, obj, "", addr, "", sym))


ENABLE_KEPT = _section("bootloader_random_enable", 0x40134956, 0x218, BL,
                       "bootloader_random_enable")
ENABLE_REFS = _entry("bootloader_random_enable", BL, _main("kiss_crypto.c"),
                     NVS)
DISABLE_REFS = _entry("bootloader_random_disable", BL, NVS)
FILL_REFS = _entry("esp_fill_random", HW, _main("camera_spike.c"),
                   _main("kiss_kef_crypto.c"), _main("kiss_seed_sd.c"),
                   _main("kiss_setup.c"), _main("kiss_crypto.c"))
START_REFS = _entry("kiss_trng_start", _main("kiss_crypto.c"), _main("main.c"))

GOOD = (
    "Archive member included to satisfy reference by file (symbol)\n\n"
    + HW + "\n"
    + "%30s%s (esp_random)\n\n" % ("", STACK)
    + "Discarded input sections\n\n"
    + " .text.bootloader_random_disable\n"
    + "%16s0x00000000      0x198 %s\n\n" % ("", BL)
    + "Linker script and memory map\n\n"
    + " .iram1.0       0x4ff01646       0x8a %s\n" % HW
    + "%16s0x4ff01646%16sesp_random\n" % ("", "")
    + _section("esp_fill_random", 0x4000596a, 0x9a, HW, "esp_fill_random")
    + ENABLE_KEPT
    + _section("kiss_trng_start", 0x4001b77a, 0x1c, _main("kiss_crypto.c"),
               "kiss_trng_start")
    + "\nCross Reference Table\n\n"
    + "%-50s%s\n" % ("Symbol", "File")
    + DISABLE_REFS
    + ENABLE_REFS
    + FILL_REFS
    + _entry("esp_random", HW, STACK)
    + _entry("kiss_trng_live", _main("kiss_crypto.c"),
             _main("kiss_kef_crypto.c"), _main("kiss_seed_sd.c"),
             _main("kiss_setup.c"))
    + START_REFS
)


def _swap(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


def selftest():
    bad = 0

    def case(name, text, want):
        nonlocal bad
        fails = judge(text)
        if want is None:
            ok = not fails
            got = "clean" if ok else fails[0]
        else:
            ok = any(want in f for f in fails)
            got = fails[0] if fails else "passed"
        print("  %-52s %s (%s)" % (name, "ok" if ok else "FAILED", got))
        bad += not ok

    case("the shipped shape passes", GOOD, None)

    # The Coldcard shape: same name, wrong provider.
    case("a same-signature substitute in main",
         _swap(GOOD, FILL_REFS,
               _entry("esp_fill_random", _main("kiss_crypto.c"),
                      _main("camera_spike.c"), _main("kiss_setup.c"))),
         "esp_fill_random: defined in")

    # The switch is in the image and nothing of ours calls it.
    case("the switch nobody calls",
         _swap(GOOD, ENABLE_REFS, _entry("bootloader_random_enable", BL, NVS)),
         "bootloader_random_enable: not referenced by")

    # Boot stops making the call.
    case("boot skips the switch",
         _swap(GOOD, START_REFS,
               _entry("kiss_trng_start", _main("kiss_crypto.c"))),
         "kiss_trng_start: not referenced by")

    # The seed screen stops reading the chip.
    case("the seed fold loses its chip leg",
         _swap(GOOD, FILL_REFS,
               _entry("esp_fill_random", HW, _main("camera_spike.c"),
                      _main("kiss_kef_crypto.c"), _main("kiss_seed_sd.c"),
                      _main("kiss_crypto.c"))),
         "esp_fill_random: not referenced by libmain.a(kiss_setup.c.obj)")

    # Something in main switches the source off.
    case("main switches the source off",
         _swap(GOOD, DISABLE_REFS,
               _entry("bootloader_random_disable", BL, NVS,
                      _main("kiss_ui.c"))),
         "bootloader_random_disable: referenced by")

    # Referenced in the cref, discarded by the linker.
    case("a call the linker threw away",
         _swap(GOOD, ENABLE_KEPT, ""),
         "bootloader_random_enable: not in the image")

    # No cref, no verdict.
    case("a map with no cross reference table",
         GOOD.split("\nCross Reference Table")[0],
         "no Cross Reference Table")

    print("rng provenance selftest: 8 cases, %d broken" % bad)
    return 1 if bad else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) != 2:
        sys.exit(__doc__)
    text = open(argv[1], encoding="utf-8", errors="ignore").read()
    fails = judge(text)
    for f in fails:
        print("FAIL: rng provenance: " + f)
    if fails:
        return 1
    print("PASS: rng provenance: %d RNG symbols defined by Espressif's "
          "objects, %d seed-path references present, nothing in main "
          "switches the source off (linker map)"
          % (len(PROVIDERS), len(REFERENCES)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
