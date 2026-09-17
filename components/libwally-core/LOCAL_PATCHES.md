# Local libwally changes

`upstream/src/sign.c`: KISS's BIP461 draft hardening verifies each ECDSA
signature against its key and message before returning it. Verification
failure clears the output and returns WALLY_ERROR. Low-R grinding also refuses
uint32 counter exhaustion instead of wrapping. Signature bytes are unchanged.

Preserve or replace these checks when updating the vendored library. Coverage:
`sim/test_crypto.c` (BIP461 fixtures and existing end-to-end PSBT tests).
