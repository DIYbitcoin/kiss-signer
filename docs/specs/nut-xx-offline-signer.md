# Draft: NUT-XX Offline signers for SIG_ALL signing packages

Status: draft proposal for cashubtc/nuts, not submitted. Builds on the
`SigAllSigningPackage` (`sigallA`) format proposed in cashubtc/nuts#345.
Reference implementation: KISS Signer (`main/kiss_ecash.c`), whose own
request format ([ecash spend request](ecash-spend-request.md)) carries the
same fields and maps onto this one field for field.

`optional`

`depends on: NUT-11, NUT-13, SigAllSigningPackage (cashubtc/nuts#345)`

---

This NUT lets a wallet hand a `SIG_ALL` swap to an offline signer: a device
that holds the [NUT-13][13] P2PK keys but no proofs, reads the package as a
QR code, shows its owner what the swap moves, and returns a signature.

## Motivation

[NUT-13][13] derives P2PK keys at `m/129373'/10'/0'/0'/{counter}` with the
counter not hardened, so a wallet holding only the account xpub can lock
every incoming proof to a fresh key of a seed it cannot spend from. The seed
can then live on an offline device, the way a bitcoin signer holds keys for a
watch only wallet.

A `sigallA` package already carries everything needed to build the
[NUT-11][11] message. What it cannot tell a signer is which outputs come back
to it. Every output is a blinded message, so a signer could only show the
total of all outputs, and an owner approving "spend 21 sat" could not see
whether the change was 2 sat or 2,000. This NUT adds the data that lets the
signer prove its own change, and fixes what the signer must check before it
signs.

## Package extension

Two optional members are added to the `SigAllSigningPackage`, and one
optional member to each output.

```json
{
  "version": "sigallA",
  "type": "swap",
  "inputs": [ { "secret": <str>, "C": <hex_str> } ],
  "outputs": [
    {
      "amount": <int>, "id": <hex_str>, "B_": <hex_str>,
      "change": {             // optional: this output comes back to the signer
        "counter": <int>,     // NUT-13 counter of the key it is locked to
        "nonce": <hex_str>,   // 32 bytes
        "r": <hex_str>        // the blinding factor, 32 bytes
      }
    }
  ],
  "signer": {                 // optional: which key the inputs are locked to
    "counter": <int>          // NUT-13 counter
  },
  "display": {                // optional: shown to the owner, never trusted
    "mint": <str>,
    "unit": <str>
  }
}
```

The keys keep the order of the `sigallA` serialization, with `signer` and
`display` after `outputs` and before `witness`.

A `change` output's secret is not sent. The signer builds it as

```
["P2PK",{"nonce":"<nonce>","data":"<pubkey at counter>","tags":[["sigflag","SIG_ALL"]]}]
```

with no whitespace, and the wallet MUST use exactly this string when it
unblinds and stores the output.

## Signer rules

Before signing, a signer:

1. MUST derive the message itself from `inputs` and `outputs`, as
   `sigallA` already requires.
2. MUST check that every input's secret is a NUT-10 `P2PK` secret whose
   `data` is its key at `signer.counter` and whose tags are exactly
   `[["sigflag","SIG_ALL"]]`, and refuse any other lock (other keys, a
   locktime, refund keys, `n_sigs`, `SIG_INPUTS`). Inputs that share data
   and tags are already a NUT-11 requirement for `SIG_ALL`.
3. MUST refuse two inputs with the same secret.
4. For each output with `change`: MUST rebuild the secret above, compute
   `B_ = hash_to_curve(secret) + r*G` ([NUT-00][00]), and refuse the package
   if it differs from the output's `B_`. MUST refuse `r = 0` or `r >= n`,
   since `r = 0` hands the mint the unblinded point.
5. MUST refuse a change secret equal to an input's secret or to another
   change output's, since the mint would sign it and the output could never
   be redeemed.
6. MUST count every output without a valid `change` as leaving, and SHOULD
   show the owner the total leaving and the total coming back before
   signing.
7. MUST present `display.mint` and `display.unit` as the wallet's claim. A
   signer that shows amounts in a unit SHOULD accept only units it can show
   truthfully: the unit is not in the signed message, so a wallet claiming
   `msat` for `sat` proofs would shrink every amount on screen while the same
   signature moves the same proofs.
8. MUST parse the package strictly: a secret one parser reads differently
   from the mint's (escapes, duplicate keys) must be refused, not repaired.

The reply is the same package with `witness.signatures` set, as defined by
`sigallA`.

## What this does not protect

- Outputs without `change` go to whoever built their blinded messages. The
  signer shows how much leaves, not who receives it.
- The wallet holds every proof and blinding factor, so it can always destroy
  ecash: delete it, or name a `change` nonce whose secret is already spent.
  The signer proves change is locked to it, not that its secret is fresh.
- "Leaving" excludes the mint's input fee, since input amounts are not in
  the package.
- The mint holds the funds behind every proof.

## Transport

Packages are a few kilobytes, so they move as animated QR codes. The
reference implementation splits the serialized package into
`p<M>of<N> <chunk>` frames of base64; any animated QR transport works. A
three input swap is about 2.2 KB.

## Interaction with the SIG_ALL message format

cashubtc/nuts#404 proposes a new SIG_ALL message (typed records, outputs
committed as `SHA256(B_)`). Nothing here depends on the message format: the
signer still receives each `B_`, which rule 4 needs, and signs whichever
message the mint verifies.

## Reference implementation

KISS Signer, an open source ESP32-P4 signing device:

- `main/kiss_ecash.c`: review and sign, the only path to a signature.
- `sim/test_ecash.c`: NUT-00, NUT-11 and NUT-13 vectors, and one test per
  forged field above, each run through the public review and sign calls.
- `sim/ecash_mint_interop.mjs`: an end to end swap against
  testnut.cashu.space, including a tampered output refused by the mint.
- `docs/ecash/`: a web wallet that holds only locked proofs and talks to the
  signer by QR.

Test vectors for this package form are not written yet.

[00]: https://cashubtc.github.io/nuts/00/
[11]: https://cashubtc.github.io/nuts/11/
[13]: https://cashubtc.github.io/nuts/13/
