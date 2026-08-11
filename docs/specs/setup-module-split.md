# Splitting kiss_setup.c: deferred, and what would make it worth doing

Status: deferred 2026-08-11, deliberately. Not scheduled, not refused. Raised by
an external review of the firmware; the reasoning for waiting is below and is
the part worth keeping.

## What it is

`main/kiss_setup.c` is 3410 lines and 67 screen builders. It is not a grab bag:
it already carries its own section rules, and they are close to the seams a
split would cut along.

| line | section |
| --- | --- |
| 305 | verify an existing backup |
| 437 | quiz (prove the backup) |
| 525 | words on screen |
| 760 | entropy, the NEW path |
| 801 | tap entropy |
| 1487 | PROVE IT |
| 1807 | dice |
| 2365 | restore: keyboard + autocomplete |
| 2472 | cards (BLIND DRAW) |

Five state machines with almost nothing to say to each other — entropy, restore,
backup, dice, cards — behind one `kiss_setup_open`. The review's suggestion was
to give each its own file.

## Why not now

It is a **pure refactor**: no behaviour changes, so nothing about it can be
proven right by a test that was not already passing. What it does change is
every line number and most of the walk's frames, and those frames are the
baseline a hardware test compares against.

The device has never completed the two flows that matter (post-update boot
through `rot_flush`, and installing an update from a card — see the DEVICE TEST
notes on those changes). Until one board has been through them, the walk's
frames are the only known-good reference this project has, and churning them to
move code between files spends that reference for nothing.

Put plainly: a refactor that cannot fail a gate, in a file whose gates are the
only thing standing in for hardware, is the wrong thing to do first.

## What would change the answer

- A board that has been through the update flows, so the walk frames are no
  longer the only evidence of correctness.
- A second reason to open the file — a real feature landing in one of the five
  machines. Splitting while already in there costs a fraction of splitting for
  its own sake.

## If it is done

Cut on the section rules above rather than by line count, keep `kiss_setup.h`
as the single entry point so callers do not learn the split, and do it as one
commit that moves code and changes nothing else — so the diff can be read as
"these lines moved" and the walk frames can be diffed to prove it.
