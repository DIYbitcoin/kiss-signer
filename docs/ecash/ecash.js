// KISS ecash vault: a coordinator for docs/specs/ecash-spend-request.md.
//
// What it keeps is locked to the signer's keys (NUT-11 P2PK, SIG_ALL, keys
// from the signer's NUT-13 export), so a copy of this page's storage cannot
// spend it. Bearer ecash passes through only on its way somewhere: a pasted
// token until its swap into locked ecash finishes, and a payment the signer
// approved until the owner has handed it over.
import { blindMessage, unblindSignature, pointFromHex, createP2PKsecret,
  getEncodedToken, getDecodedToken, hashToCurve, hasValidDleq,
  PaymentRequest } from './vendor/cashu-cashu-ts/lib/cashu-ts.es.js';
import { SimplePool } from './vendor/nostr-tools/pool.js';
import { unwrapEvent } from './vendor/nostr-tools/nip59.js';
import { generateSecretKey, getPublicKey } from './vendor/nostr-tools/pure.js';
import { nprofileEncode } from './vendor/nostr-tools/nip19.js';
import { HDKey } from '@scure/bip32';
import { sha256 } from '@noble/hashes/sha2.js';

const MINT = 'https://testnut.cashu.space';
const STORE = 'kiss-ecash-v1';
const REQ_MAX = 4096;          // the signer's QR transport limit
const FRAME_CHARS = 200;       // base64 characters per animated frame
const SIGALL = [['sigflag', 'SIG_ALL']];

const $ = (id) => document.getElementById(id);
const enc = (s) => new TextEncoder().encode(s);
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const sats = (n) => `${n} ${n === 1 ? 'sat' : 'sats'}`;
const rand32 = () => hex(crypto.getRandomValues(new Uint8Array(32)));
const tag = (secret) => hex(sha256(enc(secret))).slice(0, 32);
const yOf = (secret) => hashToCurve(enc(secret)).toHex(true);

// A refusal is final: the input itself is wrong, and retrying cannot help.
// Anything else (the mint or the network failing) may work the next time.
class Refusal extends Error {}

// ---- state ----
// The vault is one JSON record in this browser's storage, and only one tab
// at a time runs it (see the lock at the end of this file), so no other tab
// can write between this one's read and its write. Every change goes
// through commit(), which still rereads the record, refuses if the vault
// was forgotten or paired again since the change began (each pairing gets
// a new gen), and reports a failed write instead of carrying on.
let st = null;
let owner = false;
function load() {
  try { return JSON.parse(localStorage.getItem(STORE)) || null; } catch { return null; }
}
function save(v) {
  try { localStorage.setItem(STORE, JSON.stringify(v)); } catch (e) {
    throw new Error(`could not save the vault in this browser (${e.message || e}); reload the page before doing anything else`);
  }
}
function commit(gen, fn) {
  const cur = load();
  if (!cur || cur.gen !== gen) throw new Refusal('this vault was forgotten or replaced; reload the page');
  const out = fn(cur);
  save(cur);
  st = cur;
  render();
  return out;
}
// Older copies of this page did not take the lock; follow what they write.
window.addEventListener('storage', (e) => {
  if (e.key !== STORE || !owner) return;
  const was = st && st.gen;
  st = load();
  if (!st || st.gen !== was) { stopJar(); clearSignerViews(); }
  render();
});

// Bring a stored vault up to this version of the page: fields it lacks,
// one copy of each proof, C in the one spelling the signer reads, and a
// mark on any proof the signer would refuse, so it is never picked for a
// spend (its secret is kept as it is).
function tidy() {
  const cur = load();
  if (!cur) return;
  cur.gen = cur.gen || rand32();
  for (const k of ['pending', 'outbox', 'done', 'spent', 'proofs']) cur[k] = cur[k] || [];
  const seen = new Set(), pubs = new Map();
  cur.proofs = cur.proofs.filter((p) => !seen.has(p.secret) && seen.add(p.secret));
  for (const p of cur.proofs) {
    delete p.bad;
    try { p.C = pointFromHex(p.C).toHex(true); } catch { p.bad = true; continue; }
    if (!pubs.has(p.counter)) pubs.set(p.counter, pubAt(cur.xpub, p.counter));
    if (!signerLock(pubs.get(p.counter), p.secret)) p.bad = true;
  }
  for (const r of cur.pending) {
    if (r.kind) continue;
    // Records from before kinds: a bearer swap, a spend or a test mint.
    if (r.bearer) { r.kind = 'bearer'; r.inputs = r.bearer; delete r.bearer; }
    else r.kind = r.spends ? 'spend' : 'mint';
  }
  save(cur);
  st = cur;
}

// ---- mint ----
let keyset = null;
const keyCache = new Map();
// Mint errors carry the mint's code (0 when it gave none). An error with no
// code at all means no answer came back: the request may or may not have
// reached the mint.
async function api(path, body) {
  const r = await fetch(MINT + path, body ? { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) } : {});
  const t = await r.text();
  let j = {};
  try { j = JSON.parse(t); } catch { /* some mints answer errors in plain text */ }
  if (!r.ok) {
    const e = new Error(`mint said: ${j.detail || j.error || t.slice(0, 200) || r.status}`);
    e.code = Number.isInteger(j.code) ? j.code : 0;
    throw e;
  }
  return j;
}
async function ks() {
  if (keyset) return keyset;
  const all = (await api('/v1/keysets')).keysets;
  const k = all.find((x) => x.unit === 'sat' && x.active);
  const keys = await keysFor(k.id);
  const ppk = {};
  for (const x of all) if (x.unit === 'sat') ppk[x.id] = x.input_fee_ppk || 0;
  keyset = { id: k.id, keys, ids: all.map((x) => x.id), ppk };
  return keyset;
}
// The denomination keys of one keyset, active or not: a proof is unblinded
// and checked with the keys of the keyset that issued it.
async function keysFor(id) {
  if (keyCache.has(id)) return keyCache.get(id);
  const k = ((await api(`/v1/keys/${id}`)).keysets || []).find((x) => x.id === id);
  if (!k || k.unit !== 'sat' || !k.keys) throw new Error(`the mint has no sat keys for keyset ${id}`);
  keyCache.set(id, k.keys);
  return k.keys;
}
// The mint's input fee: each input's own keyset rate, summed, rounded up once.
function fee(proofs) {
  let ppk = 0;
  for (const p of proofs) {
    if (!(p.id in keyset.ppk)) throw new Refusal(`ecash from keyset ${p.id}, which this mint does not list for sat`);
    ppk += keyset.ppk[p.id];
  }
  return Math.ceil(ppk / 1000);
}
const MAX_PROOFS = 64;          // per receive: more than any real token
function split(amount) {
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('that amount is not a whole number of sats');
  const out = [];
  const denoms = Object.keys(keyset.keys).map(Number).sort((a, b) => b - a);
  for (const d of denoms) {
    const n = Math.floor(amount / d);
    if (out.length + n > MAX_PROOFS) throw new Refusal('that amount needs more pieces than this page makes at once');
    for (let i = 0; i < n; i++) out.push(d);
    amount -= n * d;
  }
  if (amount) throw new Refusal('amount cannot be split into this mint\'s denominations');
  return out;
}
// The states of these Ys, matched one by one ('UNKNOWN' when the mint did
// not answer for one exactly once).
async function stateOf(Ys) {
  const rows = (await api('/v1/checkstate', { Ys })).states;
  if (!Array.isArray(rows)) throw new Error('the mint gave a malformed answer');
  return Ys.map((y) => {
    const m = rows.filter((x) => x && x.Y === y);
    return m.length === 1 ? m[0].state : 'UNKNOWN';
  });
}

// ---- keys ----
function pubAt(xpubStr, i) {
  const xpub = xpubStr.slice(xpubStr.indexOf(']') + 1);
  return hex(HDKey.fromExtendedKey(xpub).deriveChild(i).publicKey);
}
// The change lock exactly as the signer rebuilds it (spec: no spaces).
const changeSecret = (pub, nonce) =>
  `["P2PK",{"nonce":"${nonce}","data":"${pub}","tags":[["sigflag","SIG_ALL"]]}]`;
const reserveCounter = (gen) => commit(gen, (s) => s.next++);

// A lock the signer will sign for, under the signer's own parsing rules:
// exactly ["P2PK",{nonce,data,tags}] with data this key and tags
// [["sigflag","SIG_ALL"]], no spaces or escapes, and a nonce of at most 131
// printable characters. Ecash the signer refuses is ecash nobody can spend,
// and in a spend it would block every other proof beside it.
function signerLock(pub, secret) {
  if (typeof secret !== 'string' || secret.length > 1024 || secret.includes('\\')) return false;
  let v;
  try { v = JSON.parse(secret); } catch { return false; }
  if (!Array.isArray(v) || v.length !== 2 || v[0] !== 'P2PK' || JSON.stringify(v) !== secret) return false;
  const o = v[1];
  return o && typeof o === 'object' && !Array.isArray(o) && Object.keys(o).sort().join() === 'data,nonce,tags' &&
    typeof o.nonce === 'string' && /^[\x20-\x7e]{1,131}$/.test(o.nonce) && o.data === pub &&
    JSON.stringify(o.tags) === JSON.stringify(SIGALL);
}

// Outputs are kept with their blinding factor as hex and the keyset they
// are for, so a record written before a swap can rebuild every proof
// after it, even once the mint has moved to a new keyset.
function blind(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, id: keyset.id, secret, r: bm.r.toString(16).padStart(64, '0'), B_: bm.B_.toHex(true) };
}
const wire = (o) => ({ amount: o.amount, id: o.id || keyset.id, B_: o.B_ });

// Fresh outputs locked to one new key of the signer.
function lockedOutputs(gen, amount) {
  const pieces = split(amount);
  const counter = reserveCounter(gen);
  const pub = pubAt(st.xpub, counter);
  return pieces.map((a) => ({ ...blind(a, createP2PKsecret(pub, SIGALL)), keep: counter }));
}

// The mint's signatures, checked before anything is saved: one per output,
// in order, each for the amount and keyset asked, each with the mint's
// NUT-12 proof that it signed this output's own B_ with that keyset's key
// for that amount. A swap answer that fails any of this saves nothing.
async function checkPromises(outs, sigs) {
  if (!Array.isArray(sigs) || sigs.length !== outs.length) throw new Error('the mint answered for a different number of outputs');
  const proofs = [];
  for (let i = 0; i < outs.length; i++) {
    const o = outs[i], s = sigs[i], id = o.id || keyset.id;
    if (!s || typeof s.C_ !== 'string' || Number(String(s.amount)) !== o.amount || s.id !== id ||
        !s.dleq || typeof s.dleq.e !== 'string' || typeof s.dleq.s !== 'string')
      throw new Error('the mint\'s answer does not match the outputs it was given');
    const keys = await keysFor(id);
    if (!(String(o.amount) in keys)) throw new Error(`keyset ${id} has no key for ${o.amount}`);
    let p;
    try {
      const C = unblindSignature(pointFromHex(s.C_), BigInt('0x' + o.r), pointFromHex(keys[o.amount]));
      p = { amount: o.amount, id, secret: o.secret, C: C.toHex(true) };
    } catch { throw new Error('the mint\'s answer holds a point that is not on the curve'); }
    if (!hasValidDleq({ ...p, dleq: { e: s.dleq.e, s: s.dleq.s, r: o.r } }, { id, keys }))
      throw new Error('the mint\'s signature on an output did not check out');
    proofs.push(p);
  }
  return proofs;
}

// ---- operations in flight ----
// Before any call that makes the mint sign outputs, the whole operation is
// saved as a pending record: its outputs (secrets, blinding factors,
// keyset, which key they belong to) and what it takes to send it again
// (the quote, the inputs, the signed inputs of a spend). Each operation has
// one record for life (rid: the quote, the inputs, or the signed hash), so
// sending it twice cannot make two. A record is settled only when the
// mint's signatures on every output have checked out, or when the mint has
// shown the operation can never happen (its inputs are spent elsewhere, or
// it refuses them for good while they are unspent). Anything less leaves
// the record for the next try.
function startPending(gen, rec) {
  rec.id = rand32();
  commit(gen, (s) => {
    if (s.done.includes(rec.rid)) throw new Refusal('this page already did that; nothing more to do');
    if (s.pending.some((r) => r.rid === rec.rid)) throw new Refusal('that is already on its way to the mint');
    s.pending.push(rec);
  });
  return rec;
}
// Settle a record: the spent inputs go, kept outputs join the vault, a
// payment goes to the outbox, where it stays until the owner says it was
// handed over, all in one write.
function finish(gen, rec, proofs) {
  return commit(gen, (s) => {
    if (!s.pending.some((r) => r.id === rec.id)) return null;
    s.pending = s.pending.filter((r) => r.id !== rec.id);
    const spentNow = new Set(rec.spends || []);
    s.proofs = s.proofs.filter((p) => !spentNow.has(p.secret));
    s.spent = [...s.spent, ...[...spentNow].map(tag)].slice(-4000);
    const have = new Set(s.proofs.map((p) => p.secret)), gone = new Set(s.spent);
    const pay = [];
    rec.outs.forEach((o, i) => {
      const p = proofs[i];
      if (o.keep === null || o.keep === undefined) pay.push(p);
      else if (!have.has(p.secret) && !gone.has(tag(p.secret))) s.proofs.push({ ...p, counter: o.keep });
    });
    if (pay.length) s.outbox.push({ id: rec.id, token: getEncodedToken({ mint: MINT, unit: 'sat', proofs: pay }) });
    if (rec.rid) s.done = [...s.done, rec.rid].slice(-500);
    return { pay };
  });
}
// The mint's signatures on every output of a record (NUT-09 restore), null
// when it signed none of them yet.
async function restored(rec) {
  const r = await api('/v1/restore', { outputs: rec.outs.map(wire) });
  const outs = r.outputs, sigs = r.signatures || r.promises;
  if (!Array.isArray(outs) || !Array.isArray(sigs) || outs.length !== sigs.length) throw new Error('the mint gave a malformed restore answer');
  const byB = new Map();
  outs.forEach((o, i) => { if (o && typeof o.B_ === 'string') byB.set(o.B_, sigs[i]); });
  const found = rec.outs.filter((o) => byB.has(o.B_)).length;
  if (found === 0) return null;
  if (found !== rec.outs.length) throw new Error('the mint knows only part of an interrupted swap; it is kept to try again');
  return rec.outs.map((o) => byB.get(o.B_));
}
async function send(rec) {
  const outputs = rec.outs.map(wire);
  if (rec.kind === 'mint') {
    if (!rec.quote) throw Object.assign(new Error('an old test mint record'), { code: 'old' });
    return (await api('/v1/mint/bolt11', { quote: rec.quote, outputs })).signatures;
  }
  if (!rec.inputs) throw Object.assign(new Error('an old spend record'), { code: 'old' });
  return (await api('/v1/swap', { inputs: rec.inputs, outputs })).signatures;
}
// The mint refused and signed nothing: settle the record only if the mint
// shows it can never happen, otherwise keep it.
const FINAL = new Set([10003, 11002, 11005, 11006, 11007, 11008, 12001, 12002, 'old']);
async function refused(gen, rec, err) {
  let why = null, gone = [];
  if (rec.kind === 'mint') {
    const q = rec.quote ? await api(`/v1/mint/quote/bolt11/${rec.quote}`) : null;
    if (!q || ['UNPAID', 'ISSUED', 'EXPIRED'].includes(q.state)) why = 'the test mint will not issue it';
  } else {
    const secrets = rec.kind === 'spend' ? rec.spends : rec.inputs.map((p) => p.secret);
    const states = await stateOf(secrets.map(yOf));
    if (states.every((x) => x === 'SPENT')) {
      const sigs = await restored(rec);     // spent by this very swap, signed since
      if (sigs) return sigs;
      why = 'that ecash was already spent somewhere else';
      gone = secrets;
    } else if (states.every((x) => x === 'UNSPENT') && FINAL.has(err.code)) why = `the mint refused it (${err.message})`;
  }
  if (!why) throw err;
  commit(gen, (s) => {
    s.pending = s.pending.filter((r) => r.id !== rec.id);
    s.proofs = s.proofs.filter((p) => !gone.includes(p.secret));
  });
  throw new Refusal(why);
}
// Carry one record as far as the mint lets it. A fresh one is sent first; a
// recovered one asks the mint what it already signed before sending again,
// the same request each time.
async function drive(rec, fresh) {
  const gen = st.gen;
  await ks();
  let sigs = fresh ? null : await restored(rec);
  if (!sigs) {
    try { sigs = await send(rec); } catch (e) {
      if (e.code === undefined) throw e;    // no answer: keep it
      sigs = await restored(rec) || await refused(gen, rec, e);
    }
  }
  return finish(gen, rec, await checkPromises(rec.outs, sigs));
}
const LATER = 'this page kept everything it needs and finishes it the next time it loads';
async function recoverPending() {
  for (const rec of [...st.pending]) {
    try {
      if (await drive(rec, false)) msg('recv-msg', 'Finished a swap that was interrupted.', 'ok');
    } catch (e) {
      msg('recv-msg', e instanceof Refusal ? `An interrupted swap was dropped: ${e.message}`
        : `An interrupted swap is still waiting for the mint (${e.message || e}); ${LATER}.`, 'bad');
    }
  }
}
// Run a fresh operation; on anything but a final refusal, say it is kept.
async function runFresh(rec) {
  try { return await drive(rec, true); } catch (e) {
    if (e instanceof Refusal) throw e;
    throw new Error(`the mint did not finish (${e.message || e}); ${LATER}`);
  }
}

// ---- receive ----
async function mintTest() {
  const amount = Number($('mint-amt').value);
  const gen = st.gen;
  await ks();
  if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 100000) throw new Refusal('ask for 1 to 100000 test sats');
  msg('recv-msg', 'Asking the test mint...');
  const q = await api('/v1/mint/quote/bolt11', { amount, unit: 'sat' });
  for (let i = 0; i < 30; i++) {
    if ((await api(`/v1/mint/quote/bolt11/${q.quote}`)).state === 'PAID') break;
    await new Promise((r) => setTimeout(r, 500));
  }
  const outs = lockedOutputs(gen, amount);
  await runFresh(startPending(gen, { kind: 'mint', rid: `q:${q.quote}`, quote: q.quote, outs }));
  msg('recv-msg', `${sats(amount)} locked to signer key ${outs[0].keep}.`, 'ok');
}

// Proofs as they arrive, from a token or a Nostr payload, made plain and
// bounded before anything is computed from them.
function cleanProofs(list) {
  if (!Array.isArray(list) || list.length === 0) throw new Refusal('there is no ecash in that');
  if (list.length > MAX_PROOFS) throw new Refusal(`that holds ${list.length} proofs; this page takes up to ${MAX_PROOFS} at once`);
  const out = list.map((p) => {
    const amount = Number(String(p && p.amount));
    if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 2 ** 40) throw new Refusal('that ecash has an amount no mint issues');
    if (typeof p.secret !== 'string' || typeof p.C !== 'string' || typeof p.id !== 'string') throw new Refusal('that ecash is malformed');
    return { amount, id: p.id, secret: p.secret, C: p.C, dleq: p.dleq };
  });
  if (new Set(out.map((p) => p.secret)).size !== out.length) throw new Refusal('that ecash repeats a proof');
  if (new Set(out.map((p) => p.id)).size > 4) throw new Refusal('that ecash comes from too many keysets');
  return out;
}
// Each proof's amount must be one its keyset has a key for.
async function checkDenoms(proofs) {
  for (const p of proofs) {
    if (!(p.id in keyset.ppk)) throw new Refusal(`ecash from keyset ${p.id}, which this mint does not list for sat`);
    if (!(String(p.amount) in await keysFor(p.id))) throw new Refusal('that ecash has an amount its keyset does not issue');
  }
}

// Unlocked ecash is bearer cash, so it is swapped at once into ecash locked
// to the signer. Pasted only: one sent over Nostr is refused (see onWrap).
async function swapBearer(gen, proofs) {
  await checkDenoms(proofs);
  const plain = proofs.map(({ dleq, ...p }) => p);
  const total = plain.reduce((s, p) => s + p.amount, 0);
  const keep = total - fee(plain);
  if (keep <= 0) throw new Refusal('that ecash is smaller than the mint fee');
  const outs = lockedOutputs(gen, keep);
  const rid = 'in:' + tag(plain.map((p) => p.secret).sort().join('\n'));
  await runFresh(startPending(gen, { kind: 'bearer', rid, inputs: plain, outs }));
  return `${sats(keep)} arrived unlocked and is now locked to signer key ${outs[0].keep} (mint fee ${sats(total - keep)}).`;
}

async function receiveToken() {
  const gen = st.gen;
  await ks();
  const tok = getDecodedToken($('token-in').value.trim(), keyset.ids);
  if (tok.mint.replace(/\/$/, '') !== MINT) throw new Refusal(`that token is from ${tok.mint}; this page uses ${MINT}`);
  const proofs = cleanProofs(tok.proofs);
  const locked = proofs.filter((p) => p.secret.startsWith('['));
  let said;
  if (locked.length) {
    if (!st.jar) throw new Refusal('that ecash is locked to something other than your tip jar, so this page cannot take it');
    said = await keepLockedTip(gen, st.jar.counter, proofs) || 'that tip is already in the vault.';
  } else said = await swapBearer(gen, proofs);
  $('token-in').value = '';
  msg('recv-msg', said, 'ok');
}

// ---- tip jar ----
// One printable NUT-18 payment request whose nut10 field asks the payer's
// wallet to lock the tip to one key of the signer, SIG_ALL, nothing else.
// Every tip shares that key, which is what lets one spend take many of them
// (NUT-11 wants every SIG_ALL input on the same key); the cost is that the
// mint can link the tips to each other.
//
// Delivery is Nostr (NIP-17), the one route wallets such as cashu.me pay a
// request by: the tip comes to a Nostr key this page makes and keeps. Only
// tips locked to the signer are taken that way, so a copy of that key, or
// of this page's storage, can see tips arrive and spend none of the ones
// kept. (A payer who sends an unlocked tip anyway sends bearer cash to that
// key; the page leaves it, and a copy of the key could take it.)
const RELAYS = ['wss://relay.primal.net', 'wss://relay.damus.io', 'wss://relay.snort.social'];
function jarKeys() {
  if (!st.jar || !st.jar.nsk) {
    const gen = st.gen;
    const counter = st.jar ? st.jar.counter : reserveCounter(gen);
    const nsk = hex(generateSecretKey());
    commit(gen, (s) => { if (!s.jar || !s.jar.nsk) s.jar = { counter, nsk, seen: [] }; });
  }
  const sk = Uint8Array.from(st.jar.nsk.match(/../g), (b) => parseInt(b, 16));
  return { sk, pk: getPublicKey(sk) };
}
function jarRequest() {
  const { pk } = jarKeys();
  const nostr = { type: 'nostr', target: nprofileEncode({ pubkey: pk, relays: RELAYS }), tags: [['n', '17']] };
  const pr = new PaymentRequest([nostr], `kiss${st.fp}`, undefined, 'sat', [MINT],
    'tips to a KISS Signer vault', false,
    { kind: 'P2PK', data: pubAt(st.xpub, st.jar.counter), tags: SIGALL });
  return pr.toEncodedRequest();
}
function showJar() {
  const creq = jarRequest();
  const t = $('jar-text');
  t.value = creq;
  drawQR($('jar-qr'), creq);
  drawQR($('jar-qr-inline'), creq);
  $('jar-box').classList.remove('hidden');
  t.style.height = 'auto';
  t.style.height = t.scrollHeight + 2 + 'px';
  listenJar();
}

// Gift wraps addressed to the jar key, all of them: NIP-17 backdates a wrap
// by up to two days, so a time window would drop tips that arrived while the
// page was closed. A wrap is marked handled only once its tip is kept or
// refused for good; one that met a mint or network failure is tried again a
// little later, and the next time the page opens. The listener belongs to
// one pairing and one jar key: forget the signer and it closes, and work it
// started is refused if either changed meanwhile.
let jarPool = null, jarQueue = Promise.resolve();
function listenJar() {
  if (jarPool || !owner || !st || !st.jar || !st.jar.nsk) return;
  const { sk, pk } = jarKeys();
  const w = { gen: st.gen, counter: st.jar.counter, sk };
  jarPool = new SimplePool();
  jarPool.subscribe(RELAYS, { kinds: [1059], '#p': [pk] }, {
    onevent: (ev) => { jarQueue = jarQueue.then(() => onWrap(w, ev, 0)); },
  });
  msg('jar-msg', 'Listening for tips over Nostr while this page is open.');
}
function stopJar() {
  if (jarPool) { try { jarPool.close(RELAYS); } catch { /* already closed */ } }
  jarPool = null;
}
const markSeen = (w, id) => commit(w.gen, (s) => {
  if (!s.jar || s.jar.counter !== w.counter) return;
  s.jar.seen = [...(s.jar.seen || []).filter((x) => x !== id), id].slice(-500);
});
const live = (w) => st && st.gen === w.gen && st.jar && st.jar.counter === w.counter;
async function onWrap(w, ev, attempt) {
  if (!live(w) || (st.jar.seen || []).includes(ev.id)) return;
  let payload;
  try { payload = JSON.parse(unwrapEvent(ev, w.sk).content); } catch { return; }   // not for us, or not a tip
  try {
    await ks();
    if (!payload || typeof payload !== 'object') throw new Refusal('a Nostr message that is not a tip');
    if (String(payload.mint || '').replace(/\/$/, '') !== MINT) throw new Refusal(`a tip from another mint (${payload.mint})`);
    const proofs = cleanProofs(payload.proofs);
    const pub = pubAt(st.xpub, w.counter);
    if (!proofs.every((p) => signerLock(pub, p.secret)))
      throw new Refusal('a tip came over Nostr without the lock the tip jar asks for, and was left; ask the sender to pay the tip jar again');
    const said = await keepLockedTip(w.gen, w.counter, proofs);
    markSeen(w, ev.id);
    if (said) msg('jar-msg', `Tip over Nostr: ${said}`, 'ok');
  } catch (e) {
    if (!live(w)) return;
    if (e instanceof Refusal) {
      try { markSeen(w, ev.id); } catch { /* vault gone */ }
      msg('jar-msg', `A tip arrived over Nostr but was not kept: ${e.message || e}`, 'bad');
    } else {
      msg('jar-msg', `A tip arrived over Nostr and is waiting for the mint (${e.message || e}); it is tried again.`, 'bad');
      if (attempt < 10) setTimeout(() => { jarQueue = jarQueue.then(() => onWrap(w, ev, attempt + 1)); }, 30000);
    }
  }
}

// A tip that came locked is kept as it is, with no swap: the payer cannot
// take it back, since only the signer can spend it. Checked first, because
// the page cannot spend it to find out later: the lock (above), the mint's
// own proof that it signed each one (NUT-12 DLEQ, with the keys of the
// keyset that issued it), and the mint's word that each one, matched by its
// Y, is unspent (NUT-07). Proofs already in the vault, or spent from it,
// are skipped, so the same tip delivered twice is kept once; null when
// nothing in it is new. C is stored in the one spelling the signer reads:
// compressed, lowercase. Only SPENT is a final no: a proof the mint calls
// PENDING, or does not answer for, is tried again.
async function keepLockedTip(gen, counter, proofs) {
  const pub = pubAt(st.xpub, counter);
  if (!proofs.every((p) => signerLock(pub, p.secret)))
    throw new Refusal('that ecash is locked to something other than your tip jar, so this page cannot take it');
  await checkDenoms(proofs);
  const known = (s) => (p) => s.proofs.some((q) => q.secret === p.secret) || s.spent.includes(tag(p.secret));
  const fresh = proofs.filter((p) => !known(st)(p));
  if (!fresh.length) return null;
  const kept = [];
  for (const p of fresh) {
    const keys = await keysFor(p.id);
    if (!p.dleq || !hasValidDleq(p, { id: p.id, keys }))
      throw new Refusal('that tip carries no proof the mint signed it; ask the sender to send it again');
    kept.push({ amount: p.amount, id: p.id, secret: p.secret, C: pointFromHex(p.C).toHex(true) });
  }
  const states = await stateOf(kept.map((p) => yOf(p.secret)));
  if (states.includes('SPENT')) throw new Refusal('the mint says part of that tip is already spent');
  if (!states.every((x) => x === 'UNSPENT')) throw new Error('the mint did not say every proof is unspent yet');
  const added = commit(gen, (s) => {
    if (!s.jar || s.jar.counter !== counter) throw new Refusal('the tip jar changed while this tip was checked');
    const add = kept.filter((p) => !known(s)(p));
    s.proofs.push(...add.map((p) => ({ ...p, counter })));
    return add;
  });
  if (!added.length) return null;
  const total = added.reduce((s, p) => s + p.amount, 0);
  return `${sats(total)} arrived locked to your signer; kept as it is, no fee.`;
}

// ---- send ----
let openReq = null;
const MAX_IN = 32, MAX_OUT = 48;   // the signer's limits (kiss_ecash.h)

// NUT-11 SIG_ALL: the hash the signer's reply must carry.
function sigallHash(inputs, outs) {
  let m = '';
  for (const p of inputs) m += p.secret + p.C;
  for (const o of outs) m += String(o.amount) + o.B_;
  return hex(sha256(enc(m)));
}

// SIG_ALL spends proofs that share one key. For each key, a few ways to
// cover the payment and its fee: largest first (fewest inputs), smallest
// first (least change), and the one smallest proof that covers it alone.
// makeRequest takes the first that fits the signer's limits. Proofs the
// signer would refuse, or that a pending spend already uses, are left out.
function chooseInputs(amount) {
  const busy = new Set(st.pending.flatMap((r) => r.spends || []));
  const tries = [];
  const add = (counter, take) => {
    const total = take.reduce((s, p) => s + p.amount, 0);
    if (take.length && take.length <= MAX_IN && total >= amount + fee(take)) tries.push({ counter, proofs: take, total });
  };
  const greedy = (list) => {
    const take = [];
    let total = 0;
    for (const p of list) {
      if (take.length >= MAX_IN || total >= amount + fee(take)) break;
      take.push(p);
      total += p.amount;
    }
    return take;
  };
  for (const [counter, all] of groupBy().entries()) {
    const ok = all.filter((p) => !p.bad && !busy.has(p.secret));
    const desc = [...ok].sort((a, b) => b.amount - a.amount), asc = [...desc].reverse();
    add(counter, greedy(desc));
    add(counter, greedy(asc));
    const one = asc.find((p) => p.amount >= amount + fee([p]));
    if (one) add(counter, [one]);
  }
  return tries.sort((a, b) => a.proofs.length - b.proofs.length || a.total - b.total);
}

async function makeRequest() {
  const gen = st.gen;
  await ks();
  const amount = Number($('send-amt').value);
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('enter an amount');
  const tries = chooseInputs(amount);
  if (!tries.length) throw new Refusal('no single signer key holds that much yet; send a smaller amount');
  let made = null;
  for (const pick of tries) {
    const change = pick.total - amount - fee(pick.proofs);
    let pay, back;
    try { pay = split(amount); back = change > 0 ? split(change) : []; } catch { continue; }
    if (pay.length + back.length > MAX_OUT) continue;
    const outs = pay.map((a) => ({ ...blind(a, rand32()), keep: null }));
    let cc = null;
    for (const a of back) {
      if (cc === null) cc = reserveCounter(gen);
      const nonce = rand32();
      outs.push({ ...blind(a, changeSecret(pubAt(st.xpub, cc), nonce)), keep: cc, nonce });
    }
    const req = JSON.stringify({ v: 1, mint: MINT, unit: 'sat', counter: pick.counter,
      inputs: pick.proofs.map((p) => ({ secret: p.secret, C: p.C })),
      outputs: outs.map((o) => (o.keep === null ? { amount: o.amount, B_: o.B_ }
        : { amount: o.amount, B_: o.B_, change: { counter: o.keep, nonce: o.nonce, r: o.r } })) });
    if (enc(req).length > REQ_MAX) continue;
    made = { pick, outs, req, change };
    break;
  }
  if (!made) throw new Refusal(`that spend needs a request bigger than the ${REQ_MAX} bytes the signer reads; send a smaller amount`);
  const { pick, outs, req, change } = made;
  openReq = { gen, inputs: pick.proofs, outs, hash: sigallHash(pick.proofs, outs) };
  $('req-text').value = req;
  showFrames(req);
  $('req-box').classList.remove('hidden');
  msg('send-msg', `${sats(amount)} to send, ${sats(change)} back to the vault, mint fee ${sats(fee(pick.proofs))}. Your signer will show the same.`);
}

async function submitReply(text) {
  if (!openReq) throw new Refusal('no spend request open');
  let rep;
  try { rep = JSON.parse(text.trim()); } catch { throw new Refusal('that is not the signer\'s answer'); }
  if (rep.v !== 1 || rep.hash !== openReq.hash || !/^[0-9a-f]{128}$/.test(rep.sig || ''))
    throw new Refusal('that answer is for a different request');
  const { gen, outs, hash } = openReq;
  const inputs = openReq.inputs.map((p, i) => {
    const q = { amount: p.amount, id: p.id, secret: p.secret, C: p.C };
    if (i === 0) q.witness = JSON.stringify({ signatures: [rep.sig] });
    return q;
  });
  const rec = startPending(gen, { kind: 'spend', rid: `sig:${hash}`, inputs, outs, spends: inputs.map((p) => p.secret) });
  openReq = null;
  stopFrames();
  $('req-box').classList.add('hidden');
  $('reply-in').value = '';
  msg('send-msg', 'Sending to the mint...');
  await runFresh(rec);
  msg('send-msg', '');
}

// The oldest payment not yet handed over. It stays, across reloads, until
// the owner says it was sent.
let shownToken = null;
function showToken(token) {
  shownToken = token;
  $('token-box').classList.remove('hidden');
  const out = $('token-out');
  out.value = token;
  // Tall enough for the whole token: a scrolled box hid its first lines.
  out.style.height = 'auto';
  out.style.height = out.scrollHeight + 2 + 'px';
  drawQR($('token-qr'), token);
}
function renderOutbox() {
  const ob = (st && st.outbox) || [];
  if (ob.length) {
    if (shownToken !== ob[0].token) showToken(ob[0].token);
    $('token-more').textContent = ob.length > 1 ? `${ob.length - 1} more payment${ob.length > 2 ? 's' : ''} after this one.` : '';
  } else if (shownToken !== null) {
    shownToken = null;
    $('token-box').classList.add('hidden');
  }
}

// ---- QR out ----
let frameTimer = null;
function drawQR(canvas, text) {
  const q = qrcode(0, 'L');
  q.addData(text, 'Byte');
  q.make();
  const n = q.getModuleCount(), pad = 4, px = 6;
  canvas.width = canvas.height = (n + 2 * pad) * px;
  const g = canvas.getContext('2d');
  g.fillStyle = '#fff'; g.fillRect(0, 0, canvas.width, canvas.height);
  g.fillStyle = '#000';
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
    if (q.isDark(y, x)) g.fillRect((x + pad) * px, (y + pad) * px, px, px);
}
// "pMofN <base64>" frames: the animated format the signer already reads. A
// short request is still sent as "p1of1": the signer reads bare base64 only
// when it is a PSBT, so anything else must come framed.
function frames(text) {
  const b64 = btoa(String.fromCharCode(...enc(text)));
  const parts = [];
  for (let i = 0; i < b64.length; i += FRAME_CHARS) parts.push(b64.slice(i, i + FRAME_CHARS));
  return parts.map((p, k) => `p${k + 1}of${parts.length} ${p}`);
}
// What the signer answers with its ecash key; it must match the signer's
// KEY_REQUEST byte for byte.
const KEY_REQUEST = '{"v":1,"want":"key"}';
function showFrames(text) {
  stopFrames();
  const parts = frames(text);
  let k = 0;
  const step = () => {
    drawQR($('req-qr'), parts[k]);
    $('req-frame').textContent = parts.length === 1 ? '' : `frame ${k + 1} of ${parts.length}`;
    k = (k + 1) % parts.length;
  };
  step();
  if (parts.length > 1) frameTimer = setInterval(step, 350);
}
function stopFrames() { if (frameTimer) clearInterval(frameTimer); frameTimer = null; }

// ---- camera ----
let camStream = null, camWant = null;
async function scan(want) {
  camWant = want;
  $('cam-card').classList.remove('hidden');
  // On a phone the panel is above the steps: bring the preview into view, or
  // it runs where nobody can see it and aiming is guesswork.
  $('cam-card').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  if (!camStream) {
    camStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } });
    const v = $('cam');
    v.srcObject = camStream;
    await v.play();
    const c = document.createElement('canvas');
    const g = c.getContext('2d', { willReadFrequently: true });
    const tick = () => {
      if (!camStream) return;
      if (v.videoWidth) {
        c.width = v.videoWidth; c.height = v.videoHeight;
        g.drawImage(v, 0, 0);
        const r = jsQR(g.getImageData(0, 0, c.width, c.height).data, c.width, c.height);
        if (r && r.data && camWant) {
          const w = camWant;
          camWant = null;
          stopCam();
          w(r.data);
          return;
        }
      }
      requestAnimationFrame(tick);
    };
    tick();
  }
}
function stopCam() {
  if (camStream) camStream.getTracks().forEach((t) => t.stop());
  camStream = null;
  $('cam-card').classList.add('hidden');
}


// ---- pairing ----
// A new pairing never replaces a vault that still holds anything: that
// takes Forget first, which asks.
const holds = (v) => v && ((v.proofs || []).length || (v.pending || []).length || (v.outbox || []).length);
function pair(text) {
  const t = text.trim();
  const m = /^\[([0-9a-f]{8})\/129373h\/10h\/0h\/0h\](xpub[1-9A-HJ-NP-Za-km-z]{100,112})$/.exec(t);
  if (!m) throw new Error('that is not a KISS Signer ecash key');
  HDKey.fromExtendedKey(m[2]);   // throws on a bad checksum
  const cur = load();
  if (cur && cur.xpub === t) { tidy(); render(); return; }
  if (holds(cur)) throw new Refusal('this page still holds another signer\'s vault; forget it first');
  stopJar();
  clearSignerViews();
  save({ xpub: t, fp: m[1], gen: rand32(), next: 0, proofs: [], pending: [], outbox: [], done: [], spent: [] });
  tidy();
  render();
}
function forget() {
  const cur = load();
  if (holds(cur) && !confirm('This vault still holds ecash. Forget it anyway? Your signer can still spend it, but this page will no longer know where it is.')) return;
  try { localStorage.removeItem(STORE); } catch (e) {
    alert(`Could not forget this vault in this browser (${e.message || e}). Nothing was removed.`);
    return;
  }
  stopJar();
  clearSignerViews();
  st = null;
  render();
}
// Nothing of the old signer stays on screen once it is forgotten or replaced.
function clearSignerViews() {
  openReq = null;
  stopFrames();
  for (const id of ['req-box', 'jar-box', 'token-box']) $(id).classList.add('hidden');
  for (const id of ['jar-qr', 'jar-qr-inline', 'req-qr', 'token-qr']) $(id).width = 0;
  for (const id of ['jar-text', 'req-text', 'token-out', 'reply-in']) $(id).value = '';
  shownToken = null;
}

// ---- view ----
function groupBy() {
  const g = new Map();
  for (const p of st.proofs) { if (!g.has(p.counter)) g.set(p.counter, []); g.get(p.counter).push(p); }
  return g;
}
function msg(id, text, cls) {
  const e = $(id);
  e.textContent = text;
  e.className = cls || 'mut';
}
function render() {
  const paired = owner && !!st;
  for (const id of ['vault-card', 'recv-card', 'send-card', 'jar-card']) $(id).classList.toggle('hidden', !paired);
  $('pair-card').classList.toggle('hidden', !owner || paired);
  if (!paired) return;
  const bal = st.proofs.reduce((s, p) => s + p.amount, 0);
  $('bal').textContent = bal;
  $('bal-unit').textContent = bal === 1 ? 'sat' : 'sats';
  $('mint-url').textContent = MINT;
  $('fp').textContent = st.fp;
  const g = [...groupBy().entries()].map(([c, ps]) => {
    const bad = ps.filter((p) => p.bad).reduce((s, p) => s + p.amount, 0);
    return `key ${c}: ${ps.reduce((s, p) => s + p.amount, 0)}${bad ? ` (${bad} your signer cannot spend)` : ''}`;
  });
  $('groups').textContent = g.length ? g.join(', ') : 'nothing yet';
  renderOutbox();
}

const guard = (id, f) => async (...a) => {
  try { await f(...a); } catch (e) { msg(id, e.message || String(e), 'bad'); }
};
$('pair-btn').onclick = guard('pair-msg', () => pair($('xpub-in').value));
$('pair-scan').onclick = guard('pair-msg', () => scan(guard('pair-msg', pair)));
$('unpair').onclick = forget;
$('mint-btn').onclick = guard('recv-msg', mintTest);
$('token-btn').onclick = guard('recv-msg', receiveToken);
$('send-btn').onclick = guard('send-msg', makeRequest);
$('reply-btn').onclick = guard('send-msg', () => submitReply($('reply-in').value));
$('reply-scan').onclick = guard('send-msg', () => scan(guard('send-msg', submitReply)));
$('req-cancel').onclick = () => { openReq = null; stopFrames(); stopCam(); $('req-box').classList.add('hidden'); msg('send-msg', ''); };
$('token-done').onclick = guard('send-msg', () => {
  if (!st.outbox.length) return;
  const id = st.outbox[0].id;
  commit(st.gen, (s) => { s.outbox = s.outbox.filter((x) => x.id !== id); });
});
$('cam-stop').onclick = stopCam;
$('jar-btn').onclick = guard('jar-msg', showJar);
$('jar-hide').onclick = () => $('jar-box').classList.add('hidden');
drawQR($('key-qr'), frames(KEY_REQUEST)[0]);

// The right hand panel follows the steps: each of its cards shows while the
// step it belongs to is open, so no toggle elsewhere has to know it exists.
// The camera takes the panel when it is on, since by then the signer has
// already read whatever QR was there.
function syncStage() {
  const on = (id) => !$(id).classList.contains('hidden');
  const cam = on('cam-card');
  const show = {
    'st-key': on('pair-card') && !cam,
    'st-req': on('req-box') && on('send-card') && !cam,
    'st-token': on('token-box') && on('send-card') && !on('req-box') && !cam,
    'st-jar': on('jar-box') && on('jar-card') && !cam,
  };
  let opened = null;
  for (const [id, v] of Object.entries(show)) {
    if (v && $(id).classList.contains('hidden')) opened = id;
    $(id).classList.toggle('hidden', !v);
  }
  $('st-idle').classList.toggle('hidden', cam || Object.values(show).some(Boolean));
  // On a phone the panel sits above the steps, out of sight of the button
  // that filled it: bring a new QR to the screen.
  if (opened && opened !== 'st-jar' && narrow.matches) $(opened).scrollIntoView({ behavior: 'smooth', block: 'start' });
}
const narrow = matchMedia('(max-width: 899px)');
const stageWatch = new MutationObserver(syncStage);
for (const id of ['pair-card', 'send-card', 'req-box', 'token-box', 'cam-card', 'jar-card', 'jar-box'])
  stageWatch.observe($(id), { attributes: true, attributeFilter: ['class'] });
render();
syncStage();

// ---- one tab at a time ----
// The vault runs in the one tab that holds this lock (Web Locks, shared by
// every tab of this site), for as long as that tab is open. Another tab
// shows only a note, so two copies of the page never act on one vault. A
// reload can briefly overlap the closing page, hence the few retries.
function start() {
  owner = true;
  $('tab-card').classList.add('hidden');
  tidy();
  render();
  if (!st) return;
  listenJar();
  recoverPending().catch((e) => msg('recv-msg', `An interrupted swap is still waiting for the mint (${e.message || e}); ${LATER}.`, 'bad'));
}
function blocked(text) {
  $('tab-msg').textContent = text;
  $('tab-card').classList.remove('hidden');
}
async function takeLock(tries) {
  const got = await new Promise((resolve) => {
    navigator.locks.request('kiss-ecash-vault', { ifAvailable: true }, (lock) => {
      resolve(!!lock);
      return lock ? new Promise(() => {}) : undefined;   // held until the tab closes
    });
  });
  if (got) return start();
  if (tries > 0) return setTimeout(() => takeLock(tries - 1), 700);
  blocked('This vault is already open in another tab or window. Use that one, or close it and reload this page.');
}
if (navigator.locks && navigator.locks.request) takeLock(4);
else blocked('This browser cannot keep two tabs of the vault apart. Open the page in Brave or Chrome.');
