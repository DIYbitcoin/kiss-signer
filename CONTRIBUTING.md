# Contributing

## Branches

Two long-lived branches. The split exists because this repo publishes
reproducible firmware hashes and invites you to rebuild and compare them, so
`main` has to mean something stable.

| | `develop` | `main` |
|---|---|---|
| Desktop CI green | required | required |
| Run on a real device | not required | **required** |
| Tagged | no | yes |
| Reproducible hashes published | no | yes, automatically |

- **`develop`** is the trunk. Everything lands here first. It must always
  pass `desktop-tests.yml`: generator drift, crypto vectors, sim smoke
  walks, BlueWallet interop. It may still be unproven on a device.
- **`main`** is the release line. It only moves when a release is cut,
  merged `--no-ff` from `develop` and tagged. Never commit to it directly.

**Base your pull requests on `develop`.**

```bash
git checkout develop
git checkout -b feat/my-change
# ... work, keep desktop CI green ...
gh pr create -B develop
```

## Device testing

Desktop CI runs everything that proves correctness *without* hardware. That
is deliberately not enough for `main`. Anything touching display, QR or
animation, camera, SD card, buttons, or touch must be run on a real device
before it crosses. A missing glyph draws a blank placeholder box, and CI cannot
see that.

## Cutting a release

```bash
git checkout main
git merge --no-ff develop
git tag -a vX.Y.Z -m "..."
```

Pushing `main` triggers `reproducible-build.yml`, which rebuilds both
release lanes in the pinned Docker ESP-IDF toolchain and publishes the
firmware hashes. Only push work you are willing to stand behind.
