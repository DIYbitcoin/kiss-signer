# Bitcoin Core regtest PSBT round trip

Run the focused host-side round trip with Bitcoin Core 31.1:

```sh
python3 sim/core_regtest.py
```

The command builds the test-only KISS signer bridge, starts a local Core
regtest node in a disposable data directory, and removes the node and its data
when it exits. Set `BITCOIND=/path/to/bitcoind` if the binary is not on `PATH`.
On failure, the runner prints the Core diagnostic log before cleaning up.

The test exports KISS's public native SegWit descriptor, imports its receive
and change branches into a watch-only Core wallet, mines test funds, and asks
Core to create one unsigned PSBT paying an external recipient with wallet
change. KISS's actual host-side PSBT verifier and signer report the review
amounts and change classification. The runner checks those values against
Core's transaction and exact output scripts, then asks Core to finalize the
PSBT using KISS's signatures, checks `testmempoolaccept`, broadcasts, and mines
a confirmation.

Bitcoin Core is used here only as a transaction generator and independent
regtest validator. This test does not recommend Core as a wallet or coordinator.
It does not cover physical-device input, display, QR scanning, or SD transport.

CI runs this against the pinned Bitcoin Core 31.1 release. The normal desktop
unit tests remain independently runnable with `bash sim/build_test.sh`.
