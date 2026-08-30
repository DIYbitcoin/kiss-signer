#!/usr/bin/env python3
"""Geometry read before the layout that produces it.

lv_obj_get_width/height return what the LAST layout pass computed. On an
object built a few lines earlier and never laid out they return 0, and the
call site cannot tell that from a real measurement -- so the read is silently
wrong and no rendering gate can see it, because the screen it produces looks
plausible.

wt_def_row_help is why this exists. It shrank a row's sub-line to make room
for the "?" chip beside it, read lv_obj_get_width on a label whose width had
been set a moment earlier and never laid out, got 0, failed its own
`if (w > 40)` guard and did nothing at all -- for its entire life, with every
gate green. The "?" sat inside the sub's box on the one row that has both, and
it took a screen losing a row and the text growing long enough to touch the
chip before anything reported it.

The guard is what makes this class invisible: a zero that falls through an
`if` looks exactly like a case that did not need handling.

X AND Y TOO, and the paragraph that used to be here said the opposite: that
lv_obj_get_x/y "read back what lv_obj_set_pos wrote and need no layout". They
do not. lv_obj_set_pos writes LV_STYLE_X/Y and marks the tree dirty;
lv_obj_get_x reads obj->coords, which only the next layout pass fills. On an
object positioned a few lines earlier both return 0, exactly as the width
reads do.

That sentence licensed a real regression: wt_title_cursor dropped its
lv_obj_update_layout (it was looping forever on a Korean title) and kept
reading lv_obj_get_x/y on the title beside it, so the blinking block landed 48
left and 14 high -- a clipped sliver at the top edge of every page with a
head, for four commits, with every gate green. The fix is the style getter,
lv_obj_get_style_x/y, which reads back what was written without a layout.

A function that CREATES objects is a build-time function and is what this
looks at. An event callback reading geometry is reading a settled frame.
"""
import re, sys, glob

CREATES = re.compile(r'lv_(?:label|obj|line|arc|image|bar|spangroup|canvas)_create|wt_lbl\s*\(|wt_card\s*\(|wt_chip\s*\(')
READ    = re.compile(r'lv_obj_get_(?:width|height|content_width|content_height|x|y)\s*\(\s*([^,)]+?)\s*\)')
SETTLE  = re.compile(r'lv_obj_update_layout|lv_refr_now|lv_obj_get_coords|lv_timer_handler')

# Reads that are correct as they stand, each with the reason it is correct.
ALLOW = {
    # Called only from the hold widget's own event handlers, where the fill has
    # been laid out by a previous frame -- and a 0 falls into the non-animated
    # branch, which is the right reset either way.
    ("main/kiss_theme.c", "hold_reset"),
    # The caller lays the column out immediately before calling this; see
    # wt_bundle_page_set.
    ("main/kiss_theme.c", "bundle_relink"),
    # The rows were laid out when the list was built; this runs on a tap.
    ("main/kiss_theme.c", "wt_def_list_open"),
    # A gate walks a rendered tree: lv_refr_now ran before oc_collect.
    ("sim/overlapcheck.c", "oc_check_ragged"),
}

def scan():
    bad = []
    for f in sorted(glob.glob('main/*.c')) + sorted(glob.glob('sim/*.c')):
        src = open(f).read().split('\n')
        depth = 0; start = 0; name = '?'
        for n, l in enumerate(src):
            if depth == 0 and re.match(r'^[a-zA-Z_].*\)\s*$', l.strip()):
                name = l.strip(); start = n
            depth += l.count('{') - l.count('}')
            if depth == 0 and l.count('}'):
                body = src[start:n + 1]
                text = '\n'.join(body)
                if not CREATES.search(text):
                    continue                      # not a build-time function
                fn = re.search(r'(\w+)\s*\(', name)
                fn = fn.group(1) if fn else name
                if (f, fn) in ALLOW:
                    continue
                settled = False
                for m, bl in enumerate(body):
                    if SETTLE.search(bl):
                        settled = True
                    for r in READ.finditer(bl):
                        if not settled:
                            bad.append((f, start + m + 1, fn, r.group(1).strip(),
                                        bl.strip()[:78]))
    return bad

def selftest():
    """The check fires on a shape the tree no longer contains, so a clean run
    proves nothing until it is shown to still report. The first case is
    wt_def_row_help as it actually stood; the second is the same function with
    the settle that fixed it."""
    import tempfile, os, shutil
    bug = """lv_obj_t *fake_row_help(lv_obj_t *list)
{
    lv_obj_t *l = lv_label_create(list);
    int w = lv_obj_get_width(l) - 38;
    if (w > 40) lv_obj_set_width(l, w);
    return l;
}
"""
    ok = bug.replace("    int w =", "    lv_obj_update_layout(l);\n    int w =")
    here = os.getcwd(); tmp = tempfile.mkdtemp()
    try:
        os.makedirs(os.path.join(tmp, 'main')); os.makedirs(os.path.join(tmp, 'sim'))
        bad = 0
        for name, body, want in (("fires on the bug", bug, 1),
                                 ("clear once settled", ok, 0)):
            open(os.path.join(tmp, 'main', 'x.c'), 'w').write(body)
            os.chdir(tmp)
            got = len(scan())
            os.chdir(here)
            good = (got > 0) == (want > 0)
            print("  %-34s %s (%d finding%s)" %
                  (name, "ok" if good else "FAILED", got, "" if got == 1 else "s"))
            bad += 0 if good else 1
        return bad
    finally:
        os.chdir(here); shutil.rmtree(tmp, ignore_errors=True)

if '--selftest' in sys.argv:
    print("layout-read check self test")
    n = selftest()
    print("layout reads self test: %s" %
          ("2 cases, all as expected" if not n else "%d case(s) wrong" % n))
    sys.exit(1 if n else 0)

hits = scan()
for h in hits:
    print("%s:%d  %s() reads %s before any layout pass\n      %s" % h)
print("layout reads: %d unsettled measurement(s) in build-time functions" % len(hits))
sys.exit(1 if hits else 0)
