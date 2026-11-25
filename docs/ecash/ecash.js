// KISS ecash vault: a coordinator for docs/specs/ecash-spend-request.md.
//
// Everything it stores is locked to the signer's keys (NUT-11 P2PK, SIG_ALL,
// keys from the signer's NUT-13 export), so a copy of this page's storage
// cannot spend anything. The only bearer ecash it ever holds is a payment on
// its way out, which the signer has already shown the owner.
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

// ---- state ----
// The vault is one JSON record in this browser's storage. Every change goes
// through commit(), which rereads the record first, so a second open tab
// can never write its older copy over proofs this one saved, and a write
// that fails is an error the owner sees, never a silent success. Each
// change is tied to the signer it was started for (its xpub): if that
// signer was forgotten or replaced meanwhile, the change is refused.
let st = load();
function load() {
  try { return JSON.parse(localStorage.getItem(STORE)) || null; } catch { return null; }
}
function commit(xpub, fn) {
  const cur = load();
  if (!cur || cur.xpub !== xpub) throw new Refusal('this vault was forgotten or changed in another tab; reload the page');
  cur.pending = cur.pending || [];
  const out = fn(cur);
  try { localStorage.setItem(STORE, JSON.stringify(cur)); } catch (e) {
    throw new Error(`could not save the vault in this browser (${e.message || e}); nothing changed`);
  }
  st = cur;
  render();
  return out;
}
window.addEventListener('storage', (e) => {
  if (e.key !== STORE) return;
  st = load();
  if (!st) stopJar();
  render();
});

// A refusal is final: the input itself is wrong, and retrying cannot help.
// Anything else (the mint or the network failing) may work the next time.
class Refusal extends Error {}

// ---- mint ----
let keyset = null;
async function api(path, body) {
  const r = await fetch(MINT + path, body ? { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) } : {});
  const t = await r.text();
  let j = {};
  try { j = JSON.parse(t); } catch { /* some mints answer errors in plain text */ }
  if (!r.ok) throw new Error(`mint said: ${j.detail || j.error || t.slice(0, 200) || r.status}`);
  return j;
}
async function ks() {
  if (keyset) return keyset;
  const all = (await api('/v1/keysets')).keysets;
  const k = all.find((x) => x.unit === 'sat' && x.active);
  const keys = (await api(`/v1/keys/${k.id}`)).keysets[0].keys;
  const ppk = {};
  for (const x of all) if (x.unit === 'sat') ppk[x.id] = x.input_fee_ppk || 0;
  keyset = { id: k.id, keys, ids: all.map((x) => x.id), ppk };
  return keyset;
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
  for (const d of denoms) while (amount >= d) { out.push(d); amount -= d; }
  if (amount) throw new Refusal('amount cannot be split into this mint\'s denominations');
  return out;
}

// ---- keys ----
function pubAt(xpubStr, i) {
  const xpub = xpubStr.slice(xpubStr.indexOf(']') + 1);
  return hex(HDKey.fromExtendedKey(xpub).deriveChild(i).publicKey);
}
// The change lock exactly as the signer rebuilds it (spec: no spaces).
const changeSecret = (pub, nonce) =>
  `["P2PK",{"nonce":"${nonce}","data":"${pub}","tags":[["sigflag","SIG_ALL"]]}]`;
const reserveCounter = (xpub) => commit(xpub, (s) => s.next++);

// Outputs are kept with their blinding factor as hex, so a record written
// before a swap can rebuild every proof after it.
function blind(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, secret, r: bm.r.toString(16).padStart(64, '0'), B_: bm.B_.toHex(true) };
}
function unblind(o, sig) {
  const C = unblindSignature(pointFromHex(sig.C_), BigInt('0x' + o.r), pointFromHex(keyset.keys[o.amount]));
  return { amount: o.amount, id: keyset.id, secret: o.secret, C: C.toHex(true) };
}
const wire = (o) => ({ amount: o.amount, id: keyset.id, B_: o.B_ });

// Fresh outputs locked to one new key of the signer.
function lockedOutputs(xpub, amount) {
  const counter = reserveCounter(xpub);
  const pub = pubAt(xpub, counter);
  return split(amount).map((a) => ({ ...blind(a, createP2PKsecret(pub, SIGALL)), keep: counter }));
}

// ---- outputs in flight ----
// Before any call that makes the mint sign outputs, the outputs (secrets,
// blinding factors, which key they belong to) are saved as a pending
// record. If the page closes or the answer is lost, the next load asks the
// mint for its signatures on them (NUT-09 restore) and keeps them; nothing
// the mint signed is lost. Outputs marked keep hold ecash for the vault;
// the others are a payment, shown again as a token.
function startPending(xpub, rec) {
  rec.id = rand32();
  commit(xpub, (s) => { s.pending.push(rec); });
  return rec.id;
}
function finish(xpub, id, sigs, spentSecrets) {
  return commit(xpub, (s) => {
    const rec = s.pending.find((r) => r.id === id);
    if (!rec) return null;   // another tab finished it
    s.pending = s.pending.filter((r) => r.id !== id);
    const spent = new Set(spentSecrets || []);
    s.proofs = s.proofs.filter((p) => !spent.has(p.secret));
    const have = new Set(s.proofs.map((p) => p.secret));
    const pay = [];
    rec.outs.forEach((o, i) => {
      const p = unblind(o, sigs[i]);
      if (o.keep === null || o.keep === undefined) pay.push(p);
      else if (!have.has(p.secret)) s.proofs.push({ ...p, counter: o.keep });
    });
    return { rec, pay };
  });
}
async function recoverPending() {
  if (!st || !st.pending || !st.pending.length) return;
  const xpub = st.xpub;
  await ks();
  for (const rec of [...st.pending]) {
    try {
      const r = await api('/v1/restore', { outputs: rec.outs.map(wire) });
      const sigs = r.signatures || r.promises || [];
      const byB = new Map((r.outputs || []).map((o, i) => [o.B_, sigs[i]]));
      if (rec.outs.every((o) => byB.has(o.B_))) {
        const done = finish(xpub, rec.id, rec.outs.map((o) => byB.get(o.B_)), rec.spends);
        if (done && done.pay.length) showToken(getEncodedToken({ mint: MINT, unit: 'sat', proofs: done.pay }));
        msg('recv-msg', 'Recovered ecash from a swap that was interrupted.', 'ok');
      } else if (rec.bearer) {
        // Never signed: the swap did not happen. Try it once more; if the
        // mint says the ecash is spent, it went somewhere else first.
        const s = await api('/v1/swap', { inputs: rec.bearer, outputs: rec.outs.map(wire) });
        finish(xpub, rec.id, s.signatures, []);
      } else {
        commit(xpub, (s) => { s.pending = s.pending.filter((x) => x.id !== rec.id); });
      }
    } catch (e) {
      if (/spent/i.test(e.message || '')) commit(xpub, (s) => { s.pending = s.pending.filter((x) => x.id !== rec.id); });
      else msg('recv-msg', `An interrupted swap is still waiting for the mint: ${e.message || e}`, 'bad');
    }
  }
}

// ---- receive ----
async function mintTest() {
  const amount = Number($('mint-amt').value);
  const xpub = st.xpub;
  await ks();
  if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 100000) throw new Refusal('ask for 1 to 100000 test sats');
  msg('recv-msg', 'Asking the test mint...');
  const q = await api('/v1/mint/quote/bolt11', { amount, unit: 'sat' });
  for (let i = 0; i < 30; i++) {
    if ((await api(`/v1/mint/quote/bolt11/${q.quote}`)).state === 'PAID') break;
    await new Promise((r) => setTimeout(r, 500));
  }
  const outs = lockedOutputs(xpub, amount);
  const id = startPending(xpub, { outs });
  const m = await api('/v1/mint/bolt11', { quote: q.quote, outputs: outs.map(wire) });
  finish(xpub, id, m.signatures, []);
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
  return out;
}

// Unlocked ecash is bearer cash, so it is swapped at once into ecash locked
// to the signer. Pasted only: one sent over Nostr is refused (see onWrap).
async function swapBearer(xpub, proofs) {
  const plain = proofs.map(({ dleq, ...p }) => p);
  const total = plain.reduce((s, p) => s + p.amount, 0);
  const keep = total - fee(plain);
  if (keep <= 0) throw new Refusal('that ecash is smaller than the mint fee');
  const outs = lockedOutputs(xpub, keep);
  const id = startPending(xpub, { outs, bearer: plain });
  const r = await api('/v1/swap', { inputs: plain, outputs: outs.map(wire) });
  finish(xpub, id, r.signatures, []);
  return `${sats(keep)} arrived unlocked and is now locked to signer key ${outs[0].keep} (mint fee ${sats(total - keep)}).`;
}

async function receiveToken() {
  const xpub = st.xpub;
  await ks();
  const tok = getDecodedToken($('token-in').value.trim(), keyset.ids);
  if (tok.mint.replace(/\/$/, '') !== MINT) throw new Refusal(`that token is from ${tok.mint}; this page uses ${MINT}`);
  const proofs = cleanProofs(tok.proofs);
  const locked = proofs.filter((p) => p.secret.startsWith('['));
  const said = locked.length ? await keepLockedTip(xpub, proofs) : await swapBearer(xpub, proofs);
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
// request by: the tip comes to a Nostr key this page makes and keeps. That
// key only reads deliveries, and only tips locked to the signer are taken
// that way, so a copy of it, or of this page's storage, can see tips arrive
// and spend none of them.
const RELAYS = ['wss://relay.primal.net', 'wss://relay.damus.io', 'wss://relay.snort.social'];
function jarKeys() {
  if (!st.jar || !st.jar.nsk) {
    const xpub = st.xpub;
    const counter = st.jar ? st.jar.counter : reserveCounter(xpub);
    const nsk = hex(generateSecretKey());
    commit(xpub, (s) => { if (!s.jar || !s.jar.nsk) s.jar = { counter, nsk, seen: [] }; });
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
// refused for good; one that met a mint or network failure is tried again
// the next time the page opens. The listener belongs to one vault: forget
// it and the listener closes.
let jarPool = null, jarQueue = Promise.resolve();
function listenJar() {
  if (jarPool || !st || !st.jar || !st.jar.nsk) return;
  const xpub = st.xpub;
  const { sk, pk } = jarKeys();
  jarPool = new SimplePool();
  jarPool.subscribe(RELAYS, { kinds: [1059], '#p': [pk] }, {
    onevent: (ev) => { jarQueue = jarQueue.then(() => onWrap(xpub, ev, sk)); },
  });
  msg('jar-msg', 'Listening for tips over Nostr while this page is open.');
}
function stopJar() {
  if (jarPool) { try { jarPool.close(RELAYS); } catch { /* already closed */ } }
  jarPool = null;
}
const markSeen = (xpub, id) => commit(xpub, (s) => { s.jar.seen = [...(s.jar.seen || []).filter((x) => x !== id), id].slice(-500); });
async function onWrap(xpub, ev, sk) {
  if (!st || st.xpub !== xpub || (st.jar.seen || []).includes(ev.id)) return;
  let payload;
  try { payload = JSON.parse(unwrapEvent(ev, sk).content); } catch { return; }   // not for us, or not a tip
  try {
    await ks();
    if (!payload || typeof payload !== 'object') throw new Refusal('a Nostr message that is not a tip');
    if (String(payload.mint || '').replace(/\/$/, '') !== MINT) throw new Refusal(`a tip from another mint (${payload.mint})`);
    const proofs = cleanProofs(payload.proofs);
    if (!proofs.every((p) => jarLocked(st.jar, xpub, p.secret)))
      throw new Refusal('a tip came over Nostr without the lock the tip jar asks for, and was left; ask the sender to pay the tip jar again');
    if (proofs.every((p) => st.proofs.some((q) => q.secret === p.secret))) { markSeen(xpub, ev.id); return; }
    const said = await keepLockedTip(xpub, proofs);
    markSeen(xpub, ev.id);
    msg('jar-msg', `Tip over Nostr: ${said}`, 'ok');
  } catch (e) {
    if (e instanceof Refusal) { try { markSeen(xpub, ev.id); } catch { /* vault gone */ } }
    msg('jar-msg', `A tip arrived over Nostr but was not kept: ${e.message || e}`, 'bad');
  }
}

// A lock the signer will sign for, under the signer's own parsing rules:
// exactly ["P2PK",{nonce,data,tags}] with data the jar key and tags
// [["sigflag","SIG_ALL"]], no spaces or escapes, and a nonce of at most 131
// printable characters. A tip the signer refuses is a tip nobody can spend,
// and it would also block every other tip on the jar key from a spend.
function jarLocked(jar, xpub, secret) {
  if (!jar || secret.length > 1024 || secret.includes('\\')) return false;
  let v;
  try { v = JSON.parse(secret); } catch { return false; }
  if (!Array.isArray(v) || v.length !== 2 || v[0] !== 'P2PK' || JSON.stringify(v) !== secret) return false;
  const o = v[1];
  return o && typeof o === 'object' && !Array.isArray(o) && Object.keys(o).sort().join() === 'data,nonce,tags' &&
    typeof o.nonce === 'string' && /^[\x20-\x7e]{1,131}$/.test(o.nonce) && o.data === pubAt(xpub, jar.counter) &&
    JSON.stringify(o.tags) === JSON.stringify(SIGALL);
}

// A tip that came locked is kept as it is, with no swap: the payer cannot
// take it back, since only the signer can spend it. Checked first, because
// the page cannot spend it to find out later: the lock (above), the mint's
// own proof that it signed each one (NUT-12 DLEQ, current keyset), and the
// mint's word that each one, matched by its Y, is unspent (NUT-07). C is
// stored in the one spelling the signer reads: compressed, lowercase.
async function keepLockedTip(xpub, proofs) {
  if (!proofs.every((p) => jarLocked(st.jar, xpub, p.secret)))
    throw new Refusal('that ecash is locked to something other than your tip jar, so this page cannot take it');
  const kept = proofs.map((p) => {
    if (p.id !== keyset.id || !(String(p.amount) in keyset.keys) || !p.dleq || !hasValidDleq(p, { id: keyset.id, keys: keyset.keys }))
      throw new Refusal('that tip carries no proof the mint signed it; ask the sender to send it again');
    return { amount: p.amount, id: p.id, secret: p.secret, C: pointFromHex(p.C).toHex(true) };
  });
  if (kept.some((p) => st.proofs.some((q) => q.secret === p.secret))) throw new Refusal('that tip is already in the vault');
  const Ys = kept.map((p) => hashToCurve(enc(p.secret)).toHex(true));
  const states = (await api('/v1/checkstate', { Ys })).states || [];
  for (const y of Ys) {
    const row = states.filter((x) => x && x.Y === y);
    if (row.length !== 1) throw new Error('the mint did not answer for every proof; try again');
    if (row[0].state !== 'UNSPENT') throw new Refusal('the mint says part of that tip is already spent');
  }
  commit(xpub, (s) => {
    if (kept.some((p) => s.proofs.some((q) => q.secret === p.secret))) throw new Refusal('that tip is already in the vault');
    s.proofs.push(...kept.map((p) => ({ ...p, counter: s.jar.counter })));
  });
  const total = kept.reduce((s, p) => s + p.amount, 0);
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

// SIG_ALL spends proofs that share one key. From each key's proofs, take
// the largest first until the payment and its fee are covered, which keeps
// the input count, and the request, as small as it can be.
function chooseInputs(amount) {
  const tries = [];
  for (const [counter, all] of groupBy().entries()) {
    const sorted = [...all].sort((a, b) => b.amount - a.amount);
    const take = [];
    let total = 0;
    for (const p of sorted) {
      if (take.length >= MAX_IN) break;
      if (total >= amount + fee(take)) break;
      take.push(p);
      total += p.amount;
    }
    if (total >= amount + fee(take)) tries.push({ counter, proofs: take, total });
  }
  return tries.sort((a, b) => a.proofs.length - b.proofs.length || a.total - b.total);
}

async function makeRequest() {
  const xpub = st.xpub;
  await ks();
  const amount = Number($('send-amt').value);
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('enter an amount');
  const tries = chooseInputs(amount);
  if (!tries.length) throw new Refusal('no single signer key holds that much yet; send a smaller amount');
  let made = null;
  for (const pick of tries) {
    const change = pick.total - amount - fee(pick.proofs);
    const pay = split(amount), back = change > 0 ? split(change) : [];
    if (pay.length + back.length > MAX_OUT) continue;
    const outs = pay.map((a) => ({ ...blind(a, rand32()), keep: null }));
    let cc = null;
    for (const a of back) {
      if (cc === null) cc = reserveCounter(xpub);
      const nonce = rand32();
      outs.push({ ...blind(a, changeSecret(pubAt(xpub, cc), nonce)), keep: cc, nonce });
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
  openReq = { xpub, inputs: pick.proofs, outs, hash: sigallHash(pick.proofs, outs) };
  $('req-text').value = req;
  showFrames(req);
  $('req-box').classList.remove('hidden');
  $('token-box').classList.add('hidden');
  msg('send-msg', `${sats(amount)} to send, ${sats(change)} back to the vault, mint fee ${sats(fee(pick.proofs))}. Your signer will show the same.`);
}

async function submitReply(text) {
  if (!openReq) throw new Refusal('no spend request open');
  let rep;
  try { rep = JSON.parse(text.trim()); } catch { throw new Refusal('that is not the signer\'s answer'); }
  if (rep.v !== 1 || rep.hash !== openReq.hash || !/^[0-9a-f]{128}$/.test(rep.sig || ''))
    throw new Refusal('that answer is for a different request');
  const { xpub, outs } = openReq;
  const inputs = openReq.inputs.map((p, i) => {
    const q = { amount: p.amount, id: p.id, secret: p.secret, C: p.C };
    if (i === 0) q.witness = JSON.stringify({ signatures: [rep.sig] });
    return q;
  });
  const spends = inputs.map((p) => p.secret);
  const id = startPending(xpub, { outs, spends });
  const r = await api('/v1/swap', { inputs, outputs: outs.map(wire) });
  const done = finish(xpub, id, r.signatures, spends);
  openReq = null;
  stopFrames();
  $('req-box').classList.add('hidden');
  showToken(getEncodedToken({ mint: MINT, unit: 'sat', proofs: done ? done.pay : [] }));
  msg('send-msg', '');
}

function showToken(token) {
  $('token-box').classList.remove('hidden');
  const out = $('token-out');
  out.value = token;
  // Tall enough for the whole token: a scrolled box hid its first lines.
  out.style.height = 'auto';
  out.style.height = out.scrollHeight + 2 + 'px';
  drawQR($('token-qr'), token);
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
function pair(text) {
  const t = text.trim();
  const m = /^\[([0-9a-f]{8})\/129373h\/10h\/0h\/0h\](xpub[1-9A-HJ-NP-Za-km-z]{100,112})$/.exec(t);
  if (!m) throw new Error('that is not a KISS Signer ecash key');
  HDKey.fromExtendedKey(m[2]);   // throws on a bad checksum
  if (st && st.xpub === t) return;
  if (st && st.proofs.length) throw new Refusal('forget the current signer first');
  const fresh = { xpub: t, fp: m[1], next: 0, proofs: [], pending: [] };
  try { localStorage.setItem(STORE, JSON.stringify(fresh)); } catch (e) {
    throw new Error(`could not save the vault in this browser (${e.message || e})`);
  }
  st = fresh;
  render();
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
  const paired = !!st;
  for (const id of ['vault-card', 'recv-card', 'send-card', 'jar-card']) $(id).classList.toggle('hidden', !paired);
  $('pair-card').classList.toggle('hidden', paired);
  if (!paired) return;
  const bal = st.proofs.reduce((s, p) => s + p.amount, 0);
  $('bal').textContent = bal;
  $('bal-unit').textContent = bal === 1 ? 'sat' : 'sats';
  $('mint-url').textContent = MINT;
  $('fp').textContent = st.fp;
  const g = [...groupBy().entries()].map(([c, ps]) => `key ${c}: ${ps.reduce((s, p) => s + p.amount, 0)}`);
  $('groups').textContent = g.length ? g.join(', ') : 'nothing yet';
}

const guard = (id, f) => async (...a) => {
  try { await f(...a); } catch (e) { msg(id, e.message || String(e), 'bad'); }
};
$('pair-btn').onclick = guard('pair-msg', () => pair($('xpub-in').value));
$('pair-scan').onclick = guard('pair-msg', () => scan(guard('pair-msg', pair)));
$('unpair').onclick = () => {
  if ((st.proofs.length || (st.pending || []).length) && !confirm('This vault still holds ecash. Forget it anyway? Your signer can still spend it, but this page will no longer know where it is.')) return;
  stopJar();
  openReq = null;
  st = null;
  try { localStorage.removeItem(STORE); } catch { /* private window */ }
  render();
};
$('mint-btn').onclick = guard('recv-msg', mintTest);
$('token-btn').onclick = guard('recv-msg', receiveToken);
$('send-btn').onclick = guard('send-msg', makeRequest);
$('reply-btn').onclick = guard('send-msg', () => submitReply($('reply-in').value));
$('reply-scan').onclick = guard('send-msg', () => scan(guard('send-msg', submitReply)));
$('req-cancel').onclick = () => { openReq = null; stopFrames(); stopCam(); $('req-box').classList.add('hidden'); msg('send-msg', ''); };
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
    'st-token': on('token-box') && on('send-card') && !cam,
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
listenJar();
recoverPending().catch((e) => msg('recv-msg', `An interrupted swap is still waiting for the mint: ${e.message || e}`, 'bad'));
