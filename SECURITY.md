# Security policy

KISS Signer holds private keys. A bug here can cost someone their coins, so a
security report is the most valuable thing you can send us.

## Reporting a vulnerability

Email **diybitcoin@protonmail.com**. Please do not open a public issue for
anything that could put funds at risk.

Include what you need to make it reproducible: firmware version (Settings shows
it, or the tag you built), the board, the steps, and what you expected instead.
A simulator repro (`bash sim/build_sim.sh`) is ideal but never required.

If you want the report encrypted, the address is a Proton Mail account: send
from Proton and it is end to end encrypted, or fetch the public key for it from
Proton's directory. The release signing key in `docs/installer/` is not an
option here, it is a sign only key and cannot receive encrypted mail.

What to expect:

| | |
|---|---|
| First reply | within 7 days |
| Assessment and a plan | within 14 days |
| Fix and public advisory | as fast as the severity deserves |

If a week passes with no reply, assume the mail was lost and ping the
[Telegram group](https://t.me/KISS_signer) asking us to check our inbox. Send no
details there.

## Responsible disclosure

We ask for the usual bargain, and we hold up our end of it.

**You:** report privately first, give us a reasonable window to ship a fix, and
do not access, modify or move anyone else's funds or data while researching.
Test against your own device and your own coins.

**Us:** we will confirm we received it, keep you updated, credit you in the
advisory and the changelog unless you would rather stay anonymous, and never
take legal action over research done under this policy. We will not sit on a
fix to protect a release date.

There is no bug bounty. This is a hobby project with no funding behind it.

## Scope

Anything that breaks this signer's promises is in scope, especially:

- key material leaving the device, or reaching the display, logs, SD card or QR
  output when it should not
- signing a transaction that differs from what the screen showed
- the game to signer gesture, the passphrase or the decoy path failing to gate
  access
- PSBT, descriptor or QR parsing that can be driven into memory corruption
- weak or repeated nonces, bad entropy, broken derivation
- the C6 radio leaving reset, or any path that could feed it data
- flash encryption or secure boot being bypassable in a shipped build

Known and accepted, so not vulnerabilities on their own:

- **Physical attacks on an unlocked device.** If someone has your unlocked
  signer, they have your keys. That is true of every signer.
- **Invasive hardware attacks.** Decapping, glitching and probing the die are
  out of the ESP32-P4's threat model and ours. Tell us anyway if you have
  something concrete; we would rather document it than pretend.
- **The C6 boot window.** Between power on and our first instruction, the radio
  chip may briefly run its factory firmware. No secret is decrypted that early.
  A way to exploit it *is* in scope.
- Anything in the desktop simulator that cannot happen on the device. The
  simulator is a development tool, not a signer.

## Supported versions

Beta. Only the latest tag gets fixes; there are no backports to earlier ones.
`main` is the release line and the only branch with published reproducible
hashes, so report against a `main` tag where you can.

## Verifying what you run

Every release publishes a reproducible build hash. Rebuild the tag and compare
before you trust a binary, especially one you did not download from us. If the
hashes disagree, that is a report worth sending.
