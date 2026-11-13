// sim/ecash_mint_interop.mjs: the ecash spend signer against a real mint.
//
// Mints 64 fake sats on Cashu's public test mint as proofs locked to the
// DEV seed's key 0, then spends them through sim/ecash_cli.c: the mint must
// refuse the swap unsigned and with an output changed after signing, accept
// it signed, and the change must come back locked to key 1 and be spendable
// by a second signed swap. Test mint only; never a real seed or real sats.
//
//   KISS_SIM_TMP=/tmp/kiss-x bash sim/build_test.sh && /tmp/kiss-x/kisstest
//   KISS_SIM_TMP=/tmp/kiss-x bash sim/build_ecash_cli.sh
//   npm install --ignore-scripts @cashu/cashu-ts@4.13.1   (in a scratch folder)
//   KISS_SIM_TMP=/tmp/kiss-x node sim/ecash_mint_interop.mjs <scratch>/node_modules /tmp/kiss-x/ecashcli
//
// Passed against testnut.cashu.space (cdk-mintd 0.18.1) on 2026-11-13.
import { execFileSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';

const NM = process.argv[2];               // node_modules folder
const CLI = process.argv[3];              // ecashcli binary
const MINT = 'https://testnut.cashu.space';
const ct = await import(`${NM}/@cashu/cashu-ts/lib/cashu-ts.es.js`);
const { blindMessage, unblindSignature, pointFromHex, createP2PKsecret } = ct;

const hex = (b) => Buffer.from(b).toString('hex');
const enc = (s) => new TextEncoder().encode(s);
async function api(path, body) {
  const r = await fetch(MINT + path, body ? {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body) } : {});
  const j = await r.json();
  return { ok: r.ok, j };
}
const cli = (args, input) => execFileSync(CLI, args, { input, encoding: 'utf8' }).trim();

const ks = (await api('/v1/keysets')).j.keysets.find((k) => k.unit === 'sat' && k.active);
const keys = (await api(`/v1/keys/${ks.id}`)).j.keysets[0].keys;
const fee = (n) => Math.ceil((ks.input_fee_ppk * n) / 1000);
console.log('keyset', ks.id, 'fee ppk', ks.input_fee_ppk);

const k0 = cli(['pub', '0']);
const k1 = cli(['pub', '1']);
const SIGALL = [['sigflag', 'SIG_ALL']];

// A blinded output for a given secret string; r random.
function out(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, secret, r: bm.r, B_: bm.B_.toHex(true) };
}
function unblind(o, sig) {
  const C = unblindSignature(pointFromHex(sig.C_), o.r, pointFromHex(keys[o.amount]));
  return { amount: o.amount, id: ks.id, secret: o.secret, C: C.toHex(true) };
}
// The change secret exactly as the signer builds it.
const changeSecret = (pub, nonce) =>
  `["P2PK",{"nonce":"${nonce}","data":"${pub}","tags":[["sigflag","SIG_ALL"]]}]`;

// 1. Mint 64 fake sats as proofs locked to key 0, SIG_ALL.
const q = (await api('/v1/mint/quote/bolt11', { amount: 64, unit: 'sat' })).j;
for (let i = 0; i < 20; i++) {
  const s = (await api(`/v1/mint/quote/bolt11/${q.quote}`)).j;
  if (s.state === 'PAID') break;
  await new Promise((r) => setTimeout(r, 500));
}
const lockOuts = [32, 16, 16].map((a) => out(a, createP2PKsecret(k0, SIGALL)));
const m = await api('/v1/mint/bolt11', {
  quote: q.quote, outputs: lockOuts.map((o) => ({ amount: o.amount, id: ks.id, B_: o.B_ })) });
if (!m.ok) throw new Error('mint: ' + JSON.stringify(m.j));
const locked = lockOuts.map((o, i) => unblind(o, m.j.signatures[i]));
console.log('minted', locked.length, 'proofs locked to key 0; sample secret:', locked[0].secret);

// 2. Without a signature the mint must refuse to spend them.
const plain = (a) => out(a, hex(randomBytes(32)));
{
  const outs = [plain(32), plain(16), plain(8), plain(4), plain(2), plain(1)];   // 63 + 1 fee
  const r = await api('/v1/swap', { inputs: locked, outputs: outs.map((o) => ({ amount: o.amount, id: ks.id, B_: o.B_ })) });
  console.log('swap with no signature refused:', !r.ok, r.j.detail || r.j.code || '');
}

// 3. Spend: 40 leaves, change comes back locked to key 1, fee from inputs.
async function spend(inputs, counter, leaving, changeAmts, changePub, changeCounter) {
  const total = inputs.reduce((s, p) => s + p.amount, 0);
  const outs = [];
  for (const a of leaving) outs.push({ ...plain(a), change: null });
  for (const a of changeAmts) {
    const nonce = hex(randomBytes(32));
    const o = out(a, changeSecret(changePub, nonce));
    outs.push({ ...o, change: { counter: changeCounter, nonce, r: o.r.toString(16).padStart(64, '0') } });
  }
  const sumOut = outs.reduce((s, o) => s + o.amount, 0);
  if (sumOut + fee(inputs.length) !== total) throw new Error(`amounts: ${total} in, ${sumOut} out, fee ${fee(inputs.length)}`);
  const req = JSON.stringify({ v: 1, mint: MINT, unit: 'sat', counter,
    inputs: inputs.map((p) => ({ secret: p.secret, C: p.C })),
    outputs: outs.map((o) => (o.change ? { amount: o.amount, B_: o.B_, change: o.change } : { amount: o.amount, B_: o.B_ })) });
  const reply = JSON.parse(cli(['sign'], req));
  const wire = outs.map((o) => ({ amount: o.amount, id: ks.id, B_: o.B_ }));
  // a tampered swap: one leaving output's B_ replaced after signing
  const bad = wire.map((w, i) => (i === 0 ? { ...w, B_: plain(w.amount).B_ } : w));
  const withSig = inputs.map((p, i) => (i === 0 ? { ...p, witness: JSON.stringify({ signatures: [reply.sig] }) } : p));
  const t = await api('/v1/swap', { inputs: withSig, outputs: bad });
  console.log('  swap with an output swapped after signing refused:', !t.ok, t.j.detail || t.j.code || '');
  const r = await api('/v1/swap', { inputs: withSig, outputs: wire });
  if (!r.ok) throw new Error('swap: ' + JSON.stringify(r.j));
  console.log('  signed swap accepted:', r.ok, `(${req.length} byte request)`);
  return outs.map((o, i) => ({ o, p: unblind(o, r.j.signatures[i]) }));
}

console.log('spend 1: inputs locked to key 0');
const res1 = await spend(locked, 0, [32, 8], [16, 4, 2, 1], k1, 1);   // 64 = 40 + 23 + 1 fee
const change = res1.filter((x) => x.o.change).map((x) => x.p);
console.log('change back to key 1:', change.map((p) => p.amount).join('+'));

console.log('spend 2: the change, locked to key 1');
await spend(change, 1, [16, 4, 2], [], k1, 1);                      // 23 = 22 + 1 fee
