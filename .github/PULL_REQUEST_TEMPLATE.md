## What this changes

<!-- One or two sentences. The diff says how; say what and why. -->

## Device test

Anything touching display, QR or animation, camera, SD card, buttons or touch
has to run on a real device before it reaches `main`. Desktop CI cannot see a
missing glyph or a frame that never lands. Delete the line that does not apply.

- **REQUIRED** — flows to test:
- **NOT REQUIRED** — because:

Green CI is not the answer to this question. Say why hardware cannot change the
outcome, or say which screens you walked.

## Checklist

- [ ] Based on `develop`, not `main`
- [ ] `desktop-tests.yml` passes
- [ ] Commit messages follow <https://chris.beams.io/git-commit>
- [ ] No new user-facing string left untranslated
