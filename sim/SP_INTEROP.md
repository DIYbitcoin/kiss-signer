# Offline Silent Payment interoperability

With the companion tests in a local kiss-bdk checkout:

```sh
bash sim/run_sp_interop.sh ../kiss-bdk
```

Requires clang and kiss-bdk's Rust toolchain/dependencies. The script builds
the current `kiss_psbt.c` and `kiss_sp.c`, runs the explicitly ignored Rust
integration tests, and removes its temporary binary. Each signer invocation
uses a separate temporary directory and the published `abandon … about` seed
on testnet. No network service, wallet database or physical device is needed;
Cargo may fetch build dependencies if they are not cached.

The BIP375 case builds a coordinator request, signs it with the C code, and
verifies transaction intent, DLEQ proof, derived output and ECDSA signatures
in Rust. Mutated proofs, output scripts and signed amounts, and missing
signatures, must fail verification.

The BIP376 cases cover both output-key parities and a two-input spend, verify
Schnorr signatures and finalize/extract the transaction in Rust. A foreign
tweak must produce an explicit signer STOP and no output file. Process or
parse errors cannot stand in for that refusal.

This tests the KISS pair's wire compatibility and independent C/Rust checks.
SPDK/Caravan validation, the upstream bip375-interop worker protocol, device
UI/QR/SD transport and multiparty signing are not covered. No transaction is
broadcast. DEVICE TEST NOT REQUIRED for this test-only change; it does not
change production signing or PSBT serialization.

For a separate runner, build with
`bash sim/build_sp_interop.sh /absolute/path/to/signer` and set
`KISS_INTEROP_SIGNER` to that binary when running the three `c_signer` tests.
The bridge refuses to overwrite its output and only signs a READY request;
exit 3 means an explicit STOP, while exit 2 is an operational/test failure.

## Shared sends (two signers, BIP375 per input shares)

The bridge also plays one of the two signers of a shared silent payment
send. Set `SP_BRIDGE_MNEMONIC` to the BIP-39 words of the wallet it should
be (the default is the `abandon ... about` seed); the account is
m/84'/1'/0' on testnet, and `--descriptor` prints that wallet's descriptor.

```sh
bash sim/build_sp_interop.sh "$PWD/tmp-shared/bridge"
KISS_SIM_TMP="$PWD/tmp-shared" SP_BRIDGE_MNEMONIC="legal winner thank year wave sausage worth useful legal winner thank yellow" \
    tmp-shared/bridge request.psbt response.psbt [review.json]
```

On success the last line on stdout is `SHARE` (a share only pass: this
signer's per input shares and proofs added, the silent payment output still
has no script, nothing signed) or `SIGNED`. Exit 0 on success, 3 when the
signer refused (the reason on stderr), 2 for anything else. A shared send
always carries its own caution, which the bridge accepts; any other caution
still exits 2, as it always did. The review file gains `share_only`,
`foreign_inputs` and `own_input_sats`.

## Continuous integration

The `sp-interop` job in CI runs this suite on every push and pull request.
The reproducible release workflow also requires it before either release build,
including when that workflow is dispatched directly. The main CI run therefore
checks interoperability both independently and inside the release gate.

The signer under review is tested against `kkdao/kiss-bdk` main. The reusable
workflow and manual dispatch accept `bdk_ref` to reproduce a run against a
specific commit. Both checkout revisions are printed. Publish the coordinator's
runner and all three tests before enabling this workflow: an older companion
checkout must fail, never silently skip the suite. Require the CI check in
repository branch protection separately; this patch does not change server
settings. The coordinator's own CI tests its changes against a pinned signer.

The combined spend/send test covers both input-key parities, two inputs,
separate payment and change codes, and paying our own code twice. It verifies
both derived outputs and signatures, rejects redirected outputs and altered
change amounts, finalizes the transaction, and scans it to recover the change.
This remains an offline regression check; no firmware or wallet behavior is
changed.
