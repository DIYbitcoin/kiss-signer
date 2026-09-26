#!/usr/bin/env python3
"""The boards the baked art is drawn for, in one table. Not run directly.

Every board gets its own copy of each baked picture, drawn at its own canvas
by the same generator (--board <id>) into files that define the same symbols
and share the wide files' headers; the build compiles one board's set. This
table is the one place that says which boards exist, so a board is added here
once and every generator that takes --board offers it.

A generator asks two kinds of question, and they are kept apart on purpose:

  * What the firmware also asks. NARROW is main/kiss_board.h's KISS_NARROW,
    which main.c keys the live home tiles, the chip and the game over rows
    on. A baked picture has to take the same arm as the live chrome drawn
    over it, so the generators ask that question the way main.c does, and a
    board answers it here the way its arm in kiss_board.h does.
  * What was tuned by eye on one board's glass: a logo line's size, where a
    fruit accent sits. That is not a property of a board, and the generator
    keeps it in a table of its own with one row per board, read through
    per_board(). A table that does not name every board here is refused, so
    a new board stops each generator at the first row nobody has drawn for
    it instead of quietly taking another board's.
"""
from collections import namedtuple

# The design canvas: every length in the generators is written for it, and
# X()/Y() floor it onto a board's canvas exactly as SX()/SY() do in
# main/kiss_board.h.
DESIGN_W, DESIGN_H = 800, 480

Board = namedtuple("Board", "w h suffix narrow")

BOARDS = {
    # The Guition 4.3in draws the design canvas itself, and its files carry
    # no suffix: they are the wide files every other board's copy stands in for.
    "guition": Board(w=800, h=480, suffix="", narrow=False),
    # The Waveshare 3.5in.
    "ws35": Board(w=480, h=320, suffix="_ws35", narrow=True),
    # The Guition 7in: the 4.3in's screens, scaled up.
    "jc1060": Board(w=1024, h=600, suffix="_jc1060", narrow=False),
}


def per_board(board, rows):
    """rows[board], from a table that names every board in BOARDS and no other.

    The whole table is checked, not only the row asked for: a table missing
    the 7in's row is as wrong on a 4.3in run as on a 7in one, and the 4.3in
    run is the one somebody makes first. The traceback names the table."""
    missing = sorted(set(BOARDS) - set(rows))
    if missing:
        raise ValueError("this per-board table has no row for %s"
                         % ", ".join(missing))
    extra = sorted(set(rows) - set(BOARDS))
    if extra:
        raise ValueError("this per-board table has a row for %s, which "
                         "boards.py does not list" % ", ".join(extra))
    return rows[board]
