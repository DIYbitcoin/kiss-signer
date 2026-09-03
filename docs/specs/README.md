# Specs

A spec goes here when a change needs design reasoning **before** the code, and
the reasoning is bigger than the comment beside it will hold: a format on the
wire or the card, a security property, a one way step, a migration -- anything
with a seam somebody else has to build against later. A deliberate deferral
counts too: setup-module-split.md exists to say why a split raised by an
outside review is not being done yet, which is a decision that would otherwise
have to be re-argued from scratch every time somebody opens the file.

It is not a place for a summary of finished work. The code says what the code
does, `docs/decisions.md` says what was tried and rejected, and the commit
message says why the change was made. A spec says what the shape must be, what
holds the property, and what is deliberately left out.

**Every one opens with a Status line and a date** -- implemented, approved and
not implemented, deferred -- because a spec whose state you cannot read in one
line gets read as a description of the device, and half of these describe a
device that does not exist yet.

The shelf:

| | |
| --- | --- |
| [flash-encryption-rollout.md](flash-encryption-rollout.md) | how the one way eFuse burn gets earned, rehearsal first |
| [kef-backup.md](kef-backup.md) | the password locked export, and how it differs from the sealed blob |
| [sd-seed-storage.md](sd-seed-storage.md) | keys held on the card instead of in NVS |
| [setup-module-split.md](setup-module-split.md) | splitting kiss_setup.c: deferred, and what would make it worth doing |
| [signature-fingerprint.md](signature-fingerprint.md) | the short code that identifies a signature |
| [tap-entropy.md](tap-entropy.md) | entropy from a finger, and what it is worth |
| [verifiable-determinism.md](verifiable-determinism.md) | signatures a compromised signer cannot fake |
