// KISS ecash vault: a coordinator for docs/specs/ecash-spend-request.md.
//
// What it keeps is locked to the signer's keys (NUT-11 P2PK, SIG_ALL, keys
// from the signer's NUT-13 export), so a copy of this page's storage cannot
// spend it. Bearer ecash passes through only on its way somewhere: a pasted
// token until its swap into locked ecash finishes, and a payment the signer
// approved until the owner has handed it over.
//
// How it is built (see each section): one saved record per vault, shaped
// so nothing in it can exist twice (state); one routine that carries every
// operation sent to the mint and one that finishes it (operations); one
// way ecash is credited (credit). Four rules hold everywhere:
//  1. Nothing is deleted that might still be worth something: an operation
//     leaves only when every output of one of its sets has a checked
//     signature and the proofs are saved in the same write; copies of a
//     forgotten vault are never written over.
//  2. Nothing is credited that the mint has not provably signed: a DLEQ
//     proof (NUT-12) under keys whose keyset id is rebuilt from the keys
//     themselves, on the page's own output or on a tip's own proof.
//  3. Nothing is credited twice: coins live under their own secret, and a
//     secret this vault spent is never credited again.
//  4. One writer: one tab holds the vault (Web Locks), and every write is
//     tied to the pairing it was started for.
import { blindMessage, unblindSignature, pointFromHex, createP2PKsecret,
  getEncodedToken, getDecodedToken, hashToCurve, hasValidDleq, deriveKeysetId,
  PaymentRequest } from './vendor/cashu-cashu-ts/lib/cashu-ts.es.js';
import { SimplePool } from './vendor/nostr-tools/pool.js';
import { unwrapEvent } from './vendor/nostr-tools/nip59.js';
import { generateSecretKey, getPublicKey } from './vendor/nostr-tools/pure.js';
import { nprofileEncode } from './vendor/nostr-tools/nip19.js';
import { HDKey } from '@scure/bip32';
import { sha256 } from '@noble/hashes/sha2.js';
import { schnorr } from '@noble/curves/secp256k1.js';

const MINT = 'https://testnut.cashu.space';
const STORE = 'kiss-ecash-v3';
const FORGOTTEN = `${STORE}-forgotten:`;   // + xpub + ':' + time: a vault as it was when forgotten
const REQ_MAX = 4096;          // the signer's QR transport limit
const FRAME_CHARS = 200;       // base64 characters per animated frame
const SIGALL = [['sigflag', 'SIG_ALL']];
const TIMEOUT = 20000;         // a mint request with no answer by then is given up

const $ = (id) => document.getElementById(id);
const enc = (s) => new TextEncoder().encode(s);
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const sats = (n) => `${n} ${n === 1 ? 'sat' : 'sats'}`;
const unhex = (h) => Uint8Array.from(h.match(/../g) || [], (b) => parseInt(b, 16));
const rand32 = () => hex(crypto.getRandomValues(new Uint8Array(32)));
const tag = (s) => hex(sha256(enc(s))).slice(0, 32);
const yOf = (secret) => hashToCurve(enc(secret)).toHex(true);
const listTag = (secrets) => tag(JSON.stringify([...secrets].sort()));

// A refusal is final: the input itself is wrong, and retrying cannot help.
// Anything else (the mint or the network failing) may work the next time.
class Refusal extends Error {}
// The vault is in storage but cannot be read: nothing may replace or remove
// it, and the page offers its raw text instead.
class Unreadable extends Error {}

// ---- state ----
// The vault is one record in this browser's storage (STORE), shaped so that
// nothing in it can exist twice:
//   coins { secret: coin }  ecash the vault holds, under its own secret;
//                           coin.apart when the mint calls it spent (kept,
//                           never deleted, and back when it says unspent)
//   ops   { id: op }        every operation sent to the mint, under an id
//                           made from what it is (rid), so one operation
//                           can never have two records; a finished one
//                           shrinks to { kind, rid, done } and its id stays
//                           taken for good
//   pays  { key: pay }      payments, under the set of their proofs;
//                           waiting until marked sent, then kept
//   spent { tag: 1 }        secrets this vault has spent
// plus the signer's xpub and fingerprint, the pairing id (gen), the next
// key counter, the tip jar and the keyset pins. check() states that whole
// shape: load() refuses a record that fails it (its raw text is shown, and
// nothing is written over it), and save() refuses to write one.
const V = 3;
const XPUB = /^\[([0-9a-f]{8})\/129373h\/10h\/0h\/0h\](xpub[1-9A-HJ-NP-Za-km-z]{100,112})$/;
let st = null;
let owner = false;
function load() {
  let raw;
  try { raw = localStorage.getItem(STORE); } catch (e) {
    throw new Unreadable(`this browser would not let the page read its storage (${e.message || e})`);
  }
  if (raw === null) return null;
  let v = null;
  try { v = JSON.parse(raw); } catch { /* below */ }
  if (check(v)) return v;
  throw Object.assign(new Unreadable('the vault in this browser\'s storage cannot be read'), { raw });
}
const isObj = (x) => !!x && typeof x === 'object' && !Array.isArray(x);
const isCounter = (c) => Number.isSafeInteger(c) && c >= 0 && c < 2 ** 31;   // a non hardened BIP 32 index
const isAmount = (a) => Number.isSafeInteger(a) && a > 0;
const isProof = (p) => isObj(p) && typeof p.secret === 'string' && typeof p.C === 'string' && typeof p.id === 'string' && isAmount(p.amount);
const isOut = (o) => isObj(o) && typeof o.B_ === 'string' && typeof o.secret === 'string' && typeof o.r === 'string' &&
  /^[0-9a-f]{1,64}$/.test(o.r) && isAmount(o.amount) && typeof o.id === 'string' && (o.keep === null || isCounter(o.keep));
const distinct = (xs) => new Set(xs).size === xs.length;
function check(v) {
  if (!isObj(v) || v.v !== V || typeof v.xpub !== 'string' || !XPUB.test(v.xpub)) return false;
  try { HDKey.fromExtendedKey(v.xpub.slice(v.xpub.indexOf(']') + 1)); } catch { return false; }   // checksum
  if (typeof v.fp !== 'string' || typeof v.gen !== 'string' || !Number.isSafeInteger(v.next) || v.next < 0 || v.next > 2 ** 31) return false;
  if (!isObj(v.kpin) || Object.values(v.kpin).some((x) => typeof x !== 'string')) return false;
  if (v.jar !== null && !(isObj(v.jar) && isCounter(v.jar.counter) && (v.jar.nsk === undefined || /^[0-9a-f]{64}$/.test(v.jar.nsk)) &&
    Array.isArray(v.jar.seen) && v.jar.seen.every((x) => typeof x === 'string'))) return false;
  if (!isObj(v.coins) || !Object.entries(v.coins).every(([k, c]) => isProof(c) && c.secret === k && isCounter(c.counter) &&
    (c.apart === undefined || c.apart === true))) return false;
  if (!isObj(v.spent) || !Object.values(v.spent).every((x) => x === 1)) return false;
  if (!isObj(v.pays) || !Object.values(v.pays).every((p) => isObj(p) && typeof p.token === 'string' && isAmount(p.amount) &&
    Number.isSafeInteger(p.at) && (p.sent === undefined || Number.isSafeInteger(p.sent)) &&
    (p.redeemed === undefined || Number.isSafeInteger(p.redeemed)) &&
    (p.proofs === undefined || (Array.isArray(p.proofs) && p.proofs.every(isProof))))) return false;
  if (!isObj(v.ops)) return false;
  for (const [id, o] of Object.entries(v.ops)) {
    if (!isObj(o) || !['mint', 'bearer', 'spend'].includes(o.kind) || typeof o.rid !== 'string' || id !== tag(o.rid)) return false;
    if (o.done === true) continue;
    if (!['open', 'aside'].includes(o.status) || !Number.isSafeInteger(o.at) || (o.note !== undefined && typeof o.note !== 'string')) return false;
    if (!Array.isArray(o.sets) || !o.sets.length || !o.sets.every((set) => Array.isArray(set) && set.length && set.every(isOut) &&
      distinct(set.map((x) => x.B_)) && distinct(set.map((x) => x.secret)))) return false;
    if (!isObj(o.got) || !Object.values(o.got).every((p) => isProof(p) && isObj(p.dleq))) return false;
    if (o.kind === 'mint' && typeof o.quote !== 'string') return false;
    if (o.kind !== 'mint' && !(Array.isArray(o.inputs) && o.inputs.length && o.inputs.every(isProof) && distinct(o.inputs.map((p) => p.secret)))) return false;
    if (o.kind === 'spend' && o.sets.length !== 1) return false;   // the signer signed these outputs; they are never remade
  }
  return true;
}
function save(v) {
  if (!check(v)) throw new Error('this page was about to save a vault in a shape it could not read back, so nothing was saved; reload the page');
  try { localStorage.setItem(STORE, JSON.stringify(v)); } catch (e) {
    throw new Error(`could not save the vault in this browser (${e.message || e}); reload the page before doing anything else`);
  }
}
// Every change: reread, refuse if the vault was forgotten or paired again
// since the change began (each pairing has its own gen), apply, check, write.
function commit(gen, fn) {
  const cur = load();
  if (!cur || cur.gen !== gen) throw new Refusal('this vault was forgotten or replaced; reload the page');
  const out = fn(cur);
  save(cur);
  st = cur;
  try { render(); } catch (e) { console.error(e); }
  return out;
}
window.addEventListener('storage', (e) => {
  if (e.key !== STORE || !owner) return;
  const was = st && st.gen;
  try { st = load(); } catch { st = null; }
  if (!st || st.gen !== was) { stopJar(); clearSignerViews(); }
  render();
});
const fresh = (xpub, fp) => ({ v: V, xpub, fp, gen: rand32(), next: 0, kpin: {}, jar: null, coins: {}, ops: {}, pays: {}, spent: {} });
const openOps = (s) => Object.entries(s.ops).filter(([, o]) => !o.done).sort(([, a], [, b]) => a.at - b.at);
// Whether a vault still holds anything worth keeping before it is replaced.
const holds = (v) => !!v && (Object.keys(v.coins).length > 0 || openOps(v).length > 0 || Object.values(v.pays).some((p) => !p.sent));
const sum = (ps) => ps.reduce((t, p) => t + p.amount, 0);

// ---- mint ----
// Mint errors carry the mint's code (0 when it gave none). An error with no
// code at all means no answer came back: the request may or may not have
// reached the mint. A request with no answer within TIMEOUT is given up the
// same way, so nothing waits on a stalled mint for ever; any record it
// belongs to stays and is tried again.
async function api(path, body, wait = TIMEOUT) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), Math.max(1000, wait));
  let r, t;
  try {
    r = await fetch(MINT + path, { signal: ctl.signal, ...(body ? { method: 'POST',
      headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) } : {}) });
    t = await r.text();
  } catch (e) {
    throw new Error(ctl.signal.aborted ? `the mint did not answer in time` : (e.message || String(e)));
  } finally { clearTimeout(timer); }
  let j = null;
  try { j = JSON.parse(t); } catch { /* some mints answer errors in plain text */ }
  if (!r.ok) {
    const said = j && (j.detail || j.error);
    const e = new Error(`mint said: ${(said && (typeof said === 'string' ? said : JSON.stringify(said)).slice(0, 200)) || t.slice(0, 200) || r.status}`);
    e.code = j && Number.isInteger(j.code) ? j.code : 0;
    throw e;
  }
  if (!j || typeof j !== 'object') throw new Error('the mint gave an answer that is not JSON');
  return j;
}
// The mint's sat keysets and its active one. Refreshed every ten minutes, so
// a page left open does not keep making outputs for a retired keyset, and
// when a proof names a keyset the list does not have (at most every half
// minute).
let keyset = null, keysetAt = 0;
async function ks(refresh) {
  const age = Date.now() - keysetAt;
  if (keyset && age < 600000 && !(refresh && age > 30000)) return keyset;
  if (!keyset) {
    // The mint must say plainly that it supports what this page relies on:
    // spent checks (NUT-07), restore (NUT-09), signer locks (NUT-11) and
    // proofs of its signatures (NUT-12). Anything less is retried later.
    const info = await api('/v1/info');
    const has = (n) => info.nuts && info.nuts[n] && info.nuts[n].supported === true;
    if (!['7', '9', '11', '12'].every(has))
      throw new Error('the mint does not say it supports spent checks, restore, signer locks and signature proofs (NUT-07, 09, 11, 12); this page needs all four and tries again later');
  }
  const all = (await api('/v1/keysets')).keysets;
  if (!Array.isArray(all)) throw new Error('the mint gave a malformed keyset list');
  // Only rows with a well formed id are used at all; a malformed row is the
  // mint's problem and never blamed on a token or a tip.
  const okId = (id) => typeof id === 'string' && /^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id);
  const rows = all.filter((x) => x && typeof x === 'object' && okId(x.id) && typeof x.unit === 'string');
  const list = {};
  for (const x of rows) if (x.unit === 'sat') list[x.id] = x;
  const act = Object.values(list).find((x) => x.active === true);
  if (!act) throw new Error('the mint lists no active sat keyset');
  const k = await keysFor(act.id, list);
  keyset = { id: act.id, keys: k.keys, list, ids: rows.map((x) => x.id) };
  keysetAt = Date.now();
  return keyset;
}
// The keys of one keyset, active or not, kept only once its id has been
// rebuilt from them (with the unit, fee and expiry it commits to): a mint
// cannot hand over other keys under a known id. The id must be on the
// mint's sat list, the amounts must be whole numbers written plainly, and
// no key may serve two amounts. A version 00 id commits to the keys in
// amount order but not to the amounts, so for 00 the amounts must be
// exactly 1, 2, 4 and so on, the one labelling that order allows. The
// vault also remembers each keyset's keys the first time it sees them and
// refuses a later different set under the same id.
const verified = new Map();
const amountOrder = ([a], [b]) => (BigInt(a) < BigInt(b) ? -1 : BigInt(a) > BigInt(b) ? 1 : 0);
async function keysFor(id, list = keyset && keyset.list) {
  if (verified.has(id)) return pinned(id, verified.get(id), list && list[id]);
  if (typeof id !== 'string' || !/^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id)) throw new Error(`keyset ${id} is not one this page can check`);
  if (!list || !list[id]) throw new Error(`keyset ${id} is not on the mint's list of sat keysets`);
  const k = ((await api(`/v1/keys/${id}`)).keysets || []).find((x) => x && x.id === id);
  if (!k || k.unit !== 'sat' || !k.keys || typeof k.keys !== 'object') throw new Error(`the mint has no sat keys for keyset ${id}`);
  // Amounts are read as decimal strings (up to 2^64): the page only limits
  // the amounts it actually uses, not the keyset as a whole.
  const amts = Object.keys(k.keys);
  if (!amts.length || amts.some((a) => !/^[1-9][0-9]{0,19}$/.test(a) || BigInt(a) > 2n ** 64n))
    throw new Error(`the mint's keyset ${id} has amounts this page cannot read`);
  let pts;
  try { pts = amts.map((a) => pointFromHex(k.keys[a]).toHex(true)); } catch { throw new Error(`the mint's keyset ${id} holds a key that is not a curve point`); }
  if (new Set(pts).size !== amts.length) throw new Error(`the mint's keyset ${id} uses one key for two amounts`);
  if (id.startsWith('00')) {
    const sorted = amts.map(BigInt).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    if (sorted.some((a, i) => a !== 2n ** BigInt(i))) throw new Error(`the mint's keyset ${id} has amounts its id cannot vouch for`);
  }
  const meta = list[id];
  const fees = [...new Set([k.input_fee_ppk, meta.input_fee_ppk, 0])].filter((x) => x !== undefined);
  const exps = [...new Set([k.final_expiry, meta.final_expiry, undefined])];
  let found = null;
  for (const ppk of fees) for (const expiry of exps) {
    if (found) break;
    try {
      if (deriveKeysetId(k.keys, { unit: 'sat', input_fee_ppk: ppk || undefined, expiry: expiry || undefined, versionByte: parseInt(id.slice(0, 2), 16) }) === id)
        found = { ppk: Number(ppk) || 0, expiry: Number(expiry) || 0 };
    } catch { /* try the next */ }
  }
  if (!found) throw new Error(`the mint's keys for keyset ${id} do not match its id`);
  const pin = tag(JSON.stringify(amts.map((a, i) => [a, pts[i]]).sort(amountOrder)));
  const e = { keys: k.keys, pin, ...found };
  verified.set(id, e);
  return pinned(id, e, meta);
}
// Every use of a keyset's keys, cached or not: they must match the keys
// this vault first saw under that id. A 00 keyset's fee and expiry (which
// its id does not commit to) follow the mint's current list.
function pinned(id, e, meta) {
  if (st && st.kpin && st.kpin[id] && st.kpin[id] !== e.pin) throw new Error(`the mint's keys for keyset ${id} changed since this vault first saw them`);
  if (st && owner && st.kpin && !st.kpin[id]) { try { commit(st.gen, (s) => { s.kpin[id] = s.kpin[id] || e.pin; }); } catch { /* remembered next time */ } }
  if (id.startsWith('00') && meta) {
    e.ppk = Number(meta.input_fee_ppk) || 0;
    e.expiry = Number(meta.final_expiry) || 0;
  }
  return e;
}
const expired = (e) => e.expiry > 0 && e.expiry * 1000 <= Date.now();
// A keyset id as a proof names it, full or short (the first 16 hex of a 01
// id, as wallets write it in tokens), as the full id the mint lists. A
// short one not on the list refreshes it once; still missing is a retry.
async function fullId(id) {
  if (typeof id === 'string' && /^01[0-9a-f]{14}$/.test(id)) {
    await ks();
    let full = keyset.ids.filter((i) => i.startsWith(id));
    if (full.length !== 1) { await ks(true); full = keyset.ids.filter((i) => i.startsWith(id)); }
    if (full.length !== 1) throw new Error(`keyset ${id} is not on the mint's list yet; it is looked up again shortly`);
    return full[0];
  }
  return id;
}
// Keys for a proof's keyset. One not on the mint's list refreshes the list;
// it is refused only when a refresh really happened and it is still not
// there (a refresh held back by the half minute limit is a retry, not a no).
async function keysOf(id) {
  // A proof naming a keyset id no mint could have is the proof's own fault.
  if (typeof id !== 'string' || !/^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id)) throw new Refusal(`that ecash names a keyset id (${String(id).slice(0, 20)}) no mint uses`);
  await ks();
  if (!keyset.list[id]) {
    const before = keysetAt;
    await ks(true);
    if (!keyset.list[id]) {
      if (keysetAt === before) throw new Error(`keyset ${id} is not on the mint's list yet; it is looked up again shortly`);
      throw new Error(`ecash from keyset ${id}, which this mint does not list for sat`);
    }
  }
  return keysFor(id);
}
// The keysets of everything in the vault, checked before a spend.
async function vaultKeys() {
  for (const id of new Set(Object.values(st.coins).map((p) => p.id))) { try { await keysOf(id); } catch { /* left out of spends */ } }
}
// The mint's input fee: each input's own keyset rate, summed, rounded up once.
function fee(proofs) {
  let ppk = 0;
  for (const p of proofs) {
    const e = verified.get(p.id);
    if (!e) throw new Refusal(`ecash from keyset ${p.id}, whose keys this page has not checked`);
    ppk += e.ppk;
  }
  return Math.ceil(ppk / 1000);
}
const MAX_PROOFS = 64;          // per receive: more than any real token
function split(amount) {
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('that amount is not a whole number of sats');
  const out = [];
  const denoms = Object.keys(keyset.keys).map(Number).filter((d) => Number.isSafeInteger(d) && d > 0).sort((a, b) => b - a);
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
    const m = rows.filter((x) => x && typeof x.Y === 'string' && x.Y.toLowerCase() === y);
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

// A lock the signer will sign for, read by the same rules as the signer's
// own parser (main/kiss_ecash.c parse_lock_in): ["P2PK",{...}] with the
// keys nonce, data and tags each exactly once in any order, whitespace
// allowed between tokens, no escapes or control bytes inside strings, a
// nonce of 1 to 131 bytes, data this key, and tags exactly
// [["sigflag","SIG_ALL"]]; at most 1024 bytes in all. The secret's own
// bytes are kept as they are. Ecash the signer refuses is ecash nobody can
// spend, and in a spend it would block every other proof beside it.
function signerLock(pub, secret) {
  if (typeof secret !== 'string') return false;
  const b = enc(secret);
  if (b.length === 0 || b.length > 1024) return false;
  let i = 0;
  const ws = () => { while (i < b.length && (b[i] === 32 || b[i] === 9 || b[i] === 10 || b[i] === 13)) i++; };
  const eat = (c) => { ws(); if (b[i] === c) { i++; return true; } return false; };
  const str = (cap) => {   // cap: the most bytes the signer's buffer holds
    if (!eat(34)) return null;
    const s0 = i;
    while (i < b.length && b[i] !== 34) {
      if (b[i] < 0x20 || b[i] === 0x7f || b[i] === 92) return null;
      i++;
    }
    if (i >= b.length || i - s0 > cap) return null;
    const v = new TextDecoder().decode(b.slice(s0, i));
    i++;
    return v;
  };
  if (!eat(91) || str(7) !== 'P2PK' || !eat(44) || !eat(123)) return false;
  const seen = {};
  do {
    const key = str(7);
    if (key === null || seen[key] || !eat(58)) return false;
    seen[key] = true;
    if (key === 'nonce') { const n = str(131); if (!n) return false; }
    else if (key === 'data') { if (str(66) !== pub) return false; }
    else if (key === 'tags') {
      if (!eat(91) || !eat(91) || str(15) !== 'sigflag' || !eat(44) || str(15) !== 'SIG_ALL' || !eat(93) || !eat(93)) return false;
    } else return false;
  } while (eat(44));
  if (!eat(125) || !eat(93)) return false;
  ws();
  return i === b.length && seen.nonce && seen.data && seen.tags;
}

// Outputs are kept with their blinding factor as hex and the keyset they
// are for, so a record written before a swap can rebuild every proof
// after it, even once the mint has moved to a new keyset.
function blind(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, id: keyset.id, secret, r: bm.r.toString(16).padStart(64, '0'), B_: bm.B_.toHex(true) };
}
const wire = (o) => ({ amount: o.amount, id: o.id, B_: o.B_ });

// Fresh outputs locked to one new key of the signer.
function lockedOutputs(gen, amount) {
  const pieces = split(amount);
  const counter = reserveCounter(gen);
  const pub = pubAt(st.xpub, counter);
  return pieces.map((a) => ({ ...blind(a, createP2PKsecret(pub, SIGALL)), keep: counter }));
}

// The mint's signatures, checked before anything is saved: one per output,
// in order, each for the amount (and keyset) asked, each with the mint's
// DLEQ proof that it signed this output's own B_ with the checked key of
// that keyset for that amount. An answer that fails any of this saves
// nothing. checkOne checks one signature against the one output it is for.
async function checkOne(o, s) {
  if (!s || typeof s.C_ !== 'string' || typeof s.id !== 'string' || Number(String(s.amount)) !== o.amount ||
      s.id !== o.id || !s.dleq || typeof s.dleq.e !== 'string' || typeof s.dleq.s !== 'string')
    throw new Error('the mint\'s answer does not match the outputs it was given');
  const { keys } = await keysOf(s.id);
  if (!(String(o.amount) in keys)) throw new Error(`keyset ${s.id} has no key for ${o.amount}`);
  let p;
  try {
    const C = unblindSignature(pointFromHex(s.C_), BigInt('0x' + o.r), pointFromHex(keys[o.amount]));
    p = { amount: o.amount, id: s.id, secret: o.secret, C: C.toHex(true) };
  } catch { throw new Error('the mint\'s answer holds a point that is not on the curve'); }
  p.dleq = { e: s.dleq.e, s: s.dleq.s, r: o.r };   // kept with a proof held for later, to check it again
  if (!hasValidDleq(p, { id: s.id, keys })) throw new Error('the mint\'s signature on an output did not check out');
  return p;
}
async function checkPromises(outs, sigs) {
  if (!Array.isArray(sigs) || sigs.length !== outs.length) throw new Error('the mint answered for a different number of outputs');
  const proofs = [];
  for (let i = 0; i < outs.length; i++) proofs.push(await checkOne(outs[i], sigs[i]));
  return proofs;
}

// ---- operations sent to the mint ----
// Every operation that makes the mint sign outputs (a test mint, a pasted
// token swap, a spend) is saved before anything is sent: what it is (kind,
// rid), what it takes to send it again (quote or inputs) and its output
// sets (secrets, blinding factors, keyset, which signer key each output
// belongs to). One routine, settle(), carries every operation:
//   1. ask the mint what it already signed of each output set, newest
//      first (NUT-09 restore); each signature is checked on its own and
//      kept with the operation, so answers that each cover part add up;
//   2. only for an open operation, and only when the mint gave an answer
//      about the newest set, send that set again: the same request;
//   3. a refusal that means the outputs no longer fit (keyset retired, fee
//      changed) gives a test mint or pasted token swap a new output set;
//      the old sets are still asked about.
// An operation leaves only through finish(), with one checked proof for
// every output of one set, in one write. Nothing else removes one. The
// owner may set an operation aside (no more sending, still asked about)
// and resume it.
const inFlight = new Set();
function openOp(gen, op) {
  const id = tag(op.rid);
  commit(gen, (s) => {
    const o = s.ops[id];
    if (o && o.done) throw new Refusal('this page already did that; nothing more to do');
    if (o && o.status === 'aside') throw new Refusal('that is set aside in the vault; tap Resume there to send it again');
    if (o) throw new Refusal('that is already listed under unfinished in the vault');
    // An input of another open operation is never taken by a second one: it
    // is reserved in the same write that saves this one. (A set aside one
    // lets its inputs go; its outputs are still looked for.)
    const mine = new Set((op.inputs || []).map((p) => p.secret));
    if (mine.size && openOps(s).some(([, x]) => x.status === 'open' && (x.inputs || []).some((p) => mine.has(p.secret))))
      throw new Refusal('part of that is already being received or spent by an unfinished swap; it is tried again with Try again');
    s.ops[id] = { ...op, status: 'open', at: Date.now(), got: {} };
  });
  inFlight.add(id);   // being sent from here on, by the caller
  return id;
}
// The one way ecash joins the vault: under its own secret, once, and never
// if this vault already holds it or spent it.
function credit(s, p, counter, apart) {
  if (s.coins[p.secret] || s.spent[tag(p.secret)]) return false;
  s.coins[p.secret] = { amount: p.amount, id: p.id, secret: p.secret, C: p.C, counter, ...(apart ? { apart: true } : {}) };
  return true;
}
// The one way an operation finishes, in one write: one distinct checked
// proof for every output of one of its sets; a spend's inputs become spent;
// outputs that come back are credited; a payment is added once; the
// operation shrinks to its done marker.
let lastPay = null;
// A payment's proofs: kept with it, or read from its token when a record
// has none.
function payProofs(pay) {
  if (Array.isArray(pay.proofs)) return pay.proofs;
  if (!pay.token) return [];
  try { return decodeLoose(pay.token).proofs.map((p) => ({ ...p, amount: Number(String(p.amount)) })); } catch { return []; }
}
// A token decoded even when it names a short keyset id the page has not
// loaded (yet): such an id is kept as it is.
function decodeLoose(text) {
  const ids = keyset ? [...keyset.ids] : [];
  for (let tries = 0; tries < 8; tries++) {
    try { return getDecodedToken(text, ids); } catch (e) {
      const short = /short keyset id ([0-9a-f]+)/i.exec(e.message || '');
      if (!short || ids.includes(short[1])) throw e;
      ids.push(short[1]);
    }
  }
  throw new Error('that token names too many keysets this page does not know');
}
function finish(gen, id, set, proofs) {
  if (!set.length || proofs.length !== set.length || !distinct(proofs.map((p) => p.secret)) ||
      proofs.some((p, i) => p.secret !== set[i].secret || p.amount !== set[i].amount || p.id !== set[i].id))
    throw new Error('the proofs for this swap do not match its outputs; it is kept to try again');
  return commit(gen, (s) => {
    const op = s.ops[id];
    if (!op || op.done) return null;
    if (op.kind === 'spend') for (const p of op.inputs) { delete s.coins[p.secret]; s.spent[tag(p.secret)] = 1; }
    // A pasted token that holds every proof of one of this vault's waiting
    // payments redeems it, in this same write. Anything less is left to the
    // balance check, which marks any payment the mint calls spent.
    if (op.kind === 'bearer') {
      const ins = new Set(op.inputs.map((p) => p.secret));
      for (const pay of Object.values(s.pays)) {
        const pp = payProofs(pay);
        if (!pay.redeemed && pp.length && pp.every((p) => ins.has(p.secret))) { pay.redeemed = Date.now(); pay.token = ''; delete pay.proofs; }
      }
    }
    const pay = [];
    set.forEach((o, i) => { if (o.keep === null) pay.push(proofs[i]); else credit(s, proofs[i], o.keep); });
    if (pay.length) {
      const key = listTag(pay.map((p) => p.secret));
      if (!s.pays[key]) {
        s.pays[key] = { token: getEncodedToken({ mint: MINT, unit: 'sat', proofs: pay }), amount: sum(pay), at: Date.now(), proofs: pay };
        lastPay = key;
      }
    }
    s.ops[id] = { kind: op.kind, rid: op.rid, done: true };
    return { pay };
  });
}
// Why an operation is still open, kept with it for the owner to see.
function note(gen, id, why) {
  try { commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) o.note = why; }); } catch { /* vault gone */ }
}
// What the mint has signed of one output set (NUT-09 restore). A row is
// used only if it names one of these outputs exactly once; each signature
// is checked against its own output and kept with the operation. A proof
// kept from an earlier answer counts only for the output it belongs to.
// Returns the set's proofs once every output is covered, null until then.
const fits = (o, p) => isProof(p) && p.secret === o.secret && p.amount === o.amount && p.id === o.id;
async function gather(gen, id, set) {
  const r = await api('/v1/restore', { outputs: set.map(wire) });
  const rows = r.outputs, sigs = r.signatures || r.promises;
  if (!Array.isArray(rows) || !Array.isArray(sigs) || rows.length !== sigs.length) throw new Error('the mint gave a malformed restore answer');
  const byB = new Map(set.map((o) => [o.B_, o])), count = new Map();
  for (const row of rows) { const B = row && row.B_; count.set(B, (count.get(B) || 0) + 1); }
  const op = st && st.ops[id];
  const got = {}, fresh = {};
  let bad = null;
  // Fresh signatures first; a proof kept from an earlier answer is used only
  // for an output this answer did not cover, and only after its DLEQ proof
  // is checked again.
  for (let i = 0; i < rows.length; i++) {
    const B = rows[i] && rows[i].B_;
    if (!byB.has(B) || count.get(B) !== 1) { bad = bad || new Error('the mint\'s restore answer names outputs it was not asked about'); continue; }
    try { got[B] = fresh[B] = await checkOne(byB.get(B), sigs[i]); } catch (e) { bad = bad || e; }
  }
  for (const o of set) {
    const c = op && op.got && op.got[o.B_];
    if (got[o.B_] || !fits(o, c) || !c.dleq) continue;
    try { if (hasValidDleq(c, { id: c.id, keys: (await keysOf(c.id)).keys })) got[o.B_] = c; } catch { /* asked again next time */ }
  }
  if (Object.keys(fresh).length) {
    try { commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) Object.assign(o.got, fresh); }); } catch { /* asked again next time */ }
  }
  if (set.every((o) => got[o.B_])) return set.map((o) => got[o.B_]);
  if (bad) throw bad;
  return null;
}
// A send answer, checked signature by signature: every one that checks
// out is kept with the operation at once (so a later restore that covers
// only the rest can still finish it), and the set's proofs come back only
// when all of them do.
async function checkSent(gen, id, set, sigs) {
  if (!Array.isArray(sigs)) throw new Error('the mint gave a malformed answer');
  // Each signature is matched to the output it checks out for (its DLEQ
  // binds it to that output's B_), not by its position in the answer.
  const good = {};
  let bad = sigs.length > set.length ? new Error('the mint answered for more outputs than it was given') : null;
  for (const sig of sigs) {
    let hit = false;
    for (const o of set) {
      if (good[o.B_]) continue;
      try { if (Number(String(sig && sig.amount)) !== o.amount) continue; good[o.B_] = await checkOne(o, sig); hit = true; break; } catch { /* not this output */ }
    }
    if (!hit) bad = bad || new Error('the mint\'s answer holds a signature that fits none of the outputs');
  }
  const proofs = set.map((o) => good[o.B_]);
  if (proofs.some((p) => !p)) bad = bad || new Error('the mint\'s answer does not cover every output');
  if (Object.keys(good).length) {
    try { commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) Object.assign(o.got, good); }); } catch { /* asked again next time */ }
  }
  if (bad) throw bad;
  return proofs;
}
async function send(op, set) {
  const outputs = set.map(wire);
  if (op.kind === 'mint') return (await api('/v1/mint/bolt11', { quote: op.quote, outputs })).signatures;
  return (await api('/v1/swap', { inputs: op.inputs, outputs })).signatures;
}
// mode: 'fresh' (just saved: send first), 'again' (ask, then send), or
// 'aside' (ask only).
async function settle(gen, id, mode) {
  if (inFlight.has(id) && mode !== 'fresh') throw new Error('already being sent');
  inFlight.add(id);
  try {
    await ks();
    let op = st && st.ops[id];
    if (!op || op.done) return null;
    if (mode !== 'fresh') {
      let newestErr = null;
      const sets = [...op.sets].reverse();
      for (let k = 0; k < sets.length; k++) {
        let proofs = null;
        try { proofs = await gather(gen, id, sets[k]); } catch (e) { if (k === 0) newestErr = e; continue; }
        if (proofs) return finish(gen, id, sets[k], proofs);
      }
      if (mode === 'aside') return null;
      if (newestErr) throw newestErr;   // no answer about the newest set: do not send blind
    }
    const cur = load();
    op = cur && cur.gen === gen ? cur.ops[id] : null;
    if (!op || op.done || op.status !== 'open') throw new Refusal('the vault changed, so this was not sent');
    const set = op.sets[op.sets.length - 1];
    try { return finish(gen, id, set, await checkSent(gen, id, set, await send(op, set))); } catch (e) {
      if (e.code === undefined) throw e;
      const proofs = await gather(gen, id, set);
      if (proofs) return finish(gen, id, set, proofs);
      if ([11002, 12001, 12002].includes(e.code)) await newSet(gen, id, e);
      throw e;
    }
  } catch (e) {
    if (mode !== 'aside') note(gen, id, e.message || String(e));
    throw e;
  } finally {
    inFlight.delete(id);
    try { render(); } catch { /* shown next time */ }
  }
}
// The mint no longer takes an operation's newest outputs (keyset retired,
// fee changed). A test mint or pasted token swap gets a new set for what
// it should now carry; the old sets stay and are still asked about. A
// spend cannot: the signer signed its outputs, so it is left for the
// owner to set aside and make again.
async function newSet(gen, id, e) {
  const op = st.ops[id];
  if (!op || op.done) return;
  if (op.kind === 'spend') { e.message += '; this spend\'s outputs no longer fit what the mint takes: set it aside and make the spend again'; return; }
  await ks(true);
  let total = sum(op.sets[op.sets.length - 1]);
  if (op.kind === 'bearer') {
    for (const pid of new Set(op.inputs.map((p) => p.id))) await keysOf(pid);
    total = sum(op.inputs) - fee(op.inputs);
    if (total <= 0) { e.message += '; at the mint\'s current fee that ecash is worth nothing to swap'; return; }
  }
  const last = op.sets[op.sets.length - 1];
  if (last.every((o) => o.id === keyset.id) && sum(last) === total) return;
  const outs = lockedOutputs(gen, total);
  commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) o.sets.push(outs); });
  e.message += '; the mint\'s keyset or fee changed, so new outputs were made and are sent on the next try';
}
const why = (e) => String((e && e.message) || e).replace(/\.+$/, '');
const KEPT = 'It is listed under unfinished in the vault, with everything needed to finish it; nothing was lost.';
async function runFresh(gen, id) {
  try { return await settle(gen, id, 'fresh'); } catch (e) {
    throw new Error(`Not finished: ${why(e)}. ${KEPT}`);
  }
}
// Every load and every Try again: each open operation not being sent right
// now, then each set aside one (ask only), then the balance check.
async function recoverAll(gen) {
  let done = 0, open = 0, first = null;
  for (const [id, op] of openOps(st)) {
    if (!st || st.gen !== gen) return;
    if (op.status !== 'open' || inFlight.has(id)) continue;
    try { if (await settle(gen, id, 'again')) done++; } catch (e) { open++; first = first || why(e); }
  }
  if (done || open) {
    const parts = [];
    if (done) parts.push(`Finished ${done === 1 ? 'a swap that was' : `${done} swaps that were`} interrupted.`);
    if (open) parts.push(`${open === 1 ? 'One unfinished swap is' : `${open} unfinished swaps are`} still not done (${first}). ${open === 1 ? 'It is' : 'They are'} listed under unfinished in the vault, with everything needed to finish ${open === 1 ? 'it' : 'them'}; nothing was lost.`);
    msg('recv-msg', parts.join(' '), open ? 'bad' : 'ok');
  }
  for (const [id, op] of openOps(st)) {
    if (!st || st.gen !== gen) return;
    if (op.status !== 'aside' || inFlight.has(id)) continue;
    try { if (await settle(gen, id, 'aside')) msg('recv-msg', 'A swap that was set aside turned out finished; its ecash is in the vault.', 'ok'); } catch { /* still set aside */ }
  }
  await checkBalance(gen);
}
// The inputs of open spends: not offered to another spend, and not moved
// by the balance check while the mint decides.
const busy = (s) => new Set(openOps(s).filter(([, o]) => o.kind === 'spend' && o.status === 'open').flatMap(([, o]) => o.inputs.map((p) => p.secret)));
// The balance check, one at a time: every coin not in an open spend is
// asked about by its own secret (each batch the mint answers counts, even
// if a later one fails). A coin the mint calls spent is held apart, kept;
// one held apart that it calls unspent comes back. A coin it calls pending
// is not offered or counted until it says otherwise.
const mintPending = new Set();
let balanceRun = Promise.resolve(), pendingTimer = null, balanceFailed = null;
function checkBalance(gen) {
  balanceRun = balanceRun.then(() => checkBalanceNow(gen), () => checkBalanceNow(gen));
  return balanceRun;
}
async function checkBalanceNow(gen) {
  if (!st || st.gen !== gen) return;
  const held = busy(st);
  // Payments are asked about too, waiting or sent, until the mint says
  // every proof of one is spent: whoever redeemed it, it is then marked
  // redeemed and no longer offered to hand over.
  const livePays = Object.values(st.pays).filter((p) => !isRedeemed(p));
  const ask = [...new Set([...Object.keys(st.coins).filter((x) => !held.has(x)), ...livePays.flatMap((p) => payProofs(p).map((q) => q.secret))])];
  const state = new Map();
  balanceFailed = null;
  for (let i = 0; i < ask.length; i += 100) {
    const part = ask.slice(i, i + 100);
    let res;
    try { res = await stateOf(part.map(yOf)); } catch (e) { balanceFailed = why(e); break; /* the rest is checked next time */ }
    part.forEach((x, j) => state.set(x, res[j]));
  }
  for (const [x, v] of state) { if (v === 'PENDING') mintPending.add(x); else if (v === 'UNSPENT' || v === 'SPENT') mintPending.delete(x); }
  if (mintPending.size && !pendingTimer) pendingTimer = setTimeout(() => { pendingTimer = null; if (st && st.gen === gen) checkBalance(gen); }, 60000);
  const flips = (s) => {
    const h = busy(s);
    return Object.values(s.coins).filter((c) => (state.get(c.secret) === 'SPENT' && !c.apart && !h.has(c.secret)) ||
      (state.get(c.secret) === 'UNSPENT' && c.apart));
  };
  // Per proof: a payment all of whose proofs the mint calls spent is
  // redeemed (its token, now worth nothing, is dropped; amount and dates
  // stay); a waiting one partly spent keeps waiting with a token of its
  // unspent proofs only.
  const payChanges = (s) => Object.entries(s.pays).filter(([, p]) => !p.redeemed && payProofs(p).length &&
    payProofs(p).some((q) => state.get(q.secret) === 'SPENT') && (!p.sent || payProofs(p).every((q) => state.get(q.secret) === 'SPENT'))).map(([k]) => k);
  if (!flips(st).length && !payChanges(st).length) { try { render(); } catch { /* shown next time */ } return; }
  commit(gen, (s) => {
    for (const c of flips(s)) { if (c.apart) delete c.apart; else c.apart = true; }
    for (const k of payChanges(s)) {
      const p = s.pays[k], left = payProofs(p).filter((q) => state.get(q.secret) !== 'SPENT');
      if (!left.length) { p.redeemed = Date.now(); p.token = ''; delete p.proofs; }
      else { p.proofs = left; p.token = getEncodedToken({ mint: MINT, unit: 'sat', proofs: left }); p.amount = sum(left); }
    }
  });
}
// The owner's calls on the oldest unfinished operation: set it aside (its
// inputs the mint calls spent are held apart, kept), or resume the oldest
// set aside one exactly as it was.
async function setAside() {
  const gen = st.gen;
  const entry = openOps(st).find(([id, o]) => o.status === 'open' && !inFlight.has(id));
  if (!entry) return;
  const [id, op] = entry;
  if (!confirm(`Set this unfinished swap aside?\n\n${op.note || 'It has not finished yet.'}\n\nThe page stops sending it but keeps its record, and still picks it up if the mint shows it finished.`)) return;
  let spentIn = [];
  if (op.kind === 'spend') {
    try {
      const states = await stateOf(op.inputs.map((p) => yOf(p.secret)));
      spentIn = op.inputs.filter((p, i) => states[i] === 'SPENT').map((p) => p.secret);
    } catch { /* the inputs stay as they are */ }
  }
  commit(gen, (s) => {
    const o = s.ops[id];
    if (!o || o.done || o.status !== 'open') return;
    o.status = 'aside';
    for (const x of spentIn) if (s.coins[x]) s.coins[x].apart = true;
  });
}
// Resume sends again the oldest set aside swap that can still go through:
// a test mint always can; a pasted token swap or a spend only if none of
// its inputs is spent or pending at the mint, and no open swap is using
// them. One that cannot is left as it is (still looked for), with the
// reason, and the next one is tried. (A pasted token whose proofs were
// partly received elsewhere is received again by pasting it.)
async function resume() {
  const gen = st.gen;
  let reason = null;
  for (const [id, op] of openOps(st).filter(([x, o]) => o.status === 'aside' && !inFlight.has(x))) {
    let why2 = null;
    if (op.kind !== 'mint') {
      let states;
      try { states = await stateOf(op.inputs.map((p) => yOf(p.secret))); } catch (e) { reason = reason || why(e); continue; }
      const spentIn = op.inputs.filter((p, i) => states[i] === 'SPENT'), rest = op.inputs.filter((p, i) => states[i] === 'UNSPENT');
      if (spentIn.length && op.kind === 'spend') why2 = 'part of its ecash is already spent; make the spend again';
      else if (!states.every((x) => x === 'UNSPENT' || x === 'SPENT')) why2 = 'the mint has not settled all of its ecash yet';
      else if (spentIn.length && !rest.length) why2 = 'all of its token was received elsewhere; kept only as a record';
      else if (spentIn.length) {
        // Part was received elsewhere: the unspent rest of the saved inputs
        // is received on its own (with the usual fee check), and this swap
        // stays as a record.
        const r = await swapBearer(gen, rest);
        try { commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) o.note = 'part of its token was received elsewhere and the rest on its own; kept only as a record'; }); } catch { /* shown as before */ }
        msg('recv-msg', `Part of that token was received elsewhere, so the rest was received on its own: ${r.said}`, 'ok');
        return;
      }
    }
    if (!why2) {
      const ok = commit(gen, (s) => {
        const o = s.ops[id];
        if (!o || o.done || o.status !== 'aside') return false;
        const mine = new Set((o.inputs || []).map((p) => p.secret));
        if (openOps(s).some(([x, other]) => x !== id && other.status === 'open' && (other.inputs || []).some((p) => mine.has(p.secret)))) return false;
        o.status = 'open';
        return true;
      });
      if (!ok) why2 = 'another unfinished swap is using part of its ecash';
      else {
        msg('recv-msg', 'Sending the set aside swap again...');
        if (await settle(gen, id, 'again')) msg('recv-msg', 'Finished the swap that was set aside.', 'ok');
        return;
      }
    }
    try { commit(gen, (s) => { const o = s.ops[id]; if (o && !o.done) o.note = why2; }); } catch { /* shown as before */ }
    reason = reason || why2;
  }
  if (reason) throw new Refusal(`no set aside swap can go through right now (${reason}); they stay set aside and are still looked for`);
}

// ---- receive ----
// The quote and its outputs are saved before waiting for payment, so
// closing the page while it waits loses nothing.
async function mintTest() {
  const amount = Number($('mint-amt').value);
  const gen = st.gen;
  await ks();
  if (!st || st.gen !== gen) throw new Refusal('the vault changed; nothing was asked of the mint');
  if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 100000) throw new Refusal('ask for 1 to 100000 test sats');
  msg('recv-msg', 'Asking the test mint...');
  const q = await api('/v1/mint/quote/bolt11', { amount, unit: 'sat' });
  if (typeof q.quote !== 'string' || !q.quote) throw new Error('the test mint gave no quote');
  const outs = lockedOutputs(gen, amount);
  const id = openOp(gen, { kind: 'mint', rid: `q:${q.quote}`, quote: q.quote, sets: [outs] });
  // Waiting for payment; any failure here is left to the mint request
  // below, which records it on the record.
  try {
    const until = Date.now() + 20000;
    for (let i = 0; i < 30 && Date.now() < until; i++) {
      const s = await api(`/v1/mint/quote/bolt11/${encodeURIComponent(q.quote)}`, null, until - Date.now());
      if (s.state === 'PAID' || s.state === 'ISSUED') break;
      await new Promise((r) => setTimeout(r, 500));
    }
  } catch { /* below */ }
  await runFresh(gen, id);
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
  // Exact repeats are dropped; copies of one secret that differ are all
  // judged, and the first that checks out is the one kept.
  const seen = new Set();
  const once = out.filter((p) => { const k = JSON.stringify([p.amount, p.id, p.secret, p.C, p.dleq || null]); return !seen.has(k) && seen.add(k); });
  return once;
}
// Each proof's keyset must be one of the mint's sat keysets, checked, not
// past its final expiry, with a key for the proof's amount.
// Each proof of a pasted token on its own (checklist rule 2): one whose
// keyset the mint does not list yet, or calls expired, waits for the next
// try; one with an amount its keyset does not issue, a C that is not a
// curve point, or a DLEQ proof that does not hold, is left out and said;
// the rest go on.
// Whether receiving these unspent proofs now and the rest later costs more
// in rounded fees than receiving them together. Proofs whose keyset is not
// checked yet count as costing more, unless the part now rounds exactly.
const ppkOf = (ps) => ps.reduce((t, p) => t + ((verified.get(p.id) || {}).ppk || 0), 0);
function feeSplitCostsMore(now, waiting, later) {
  if (!waiting.length && !later.length) return false;
  const a = ppkOf(now);
  if (a % 1000 === 0) return false;
  if (later.length) return true;
  const b = ppkOf(waiting);
  return Math.ceil(a / 1000) + Math.ceil(b / 1000) > Math.ceil((a + b) / 1000);
}
async function sortPasted(proofs) {
  const looked = new Map();   // one lookup per keyset
  const ok = [], left = [], laterProofs = [];
  let later = null;
  for (const p of proofs) {
    if (!looked.has(p.id)) looked.set(p.id, fullId(p.id).then((full) => keysOf(full).then((e) => ({ full, e }))));
    let e;
    try { const r = await looked.get(p.id); p.id = r.full; e = r.e; } catch (x) {
      if (x instanceof Refusal) { left.push({ secret: p.secret, why: 'a proof naming a keyset this mint does not use' }); continue; }
      later = later || x;
      laterProofs.push(p);
      continue;
    }
    if (expired(e)) { later = later || new Error('the mint says that keyset no longer redeems; try again later'); laterProofs.push(p); continue; }
    if (!(String(p.amount) in e.keys)) { left.push({ secret: p.secret, why: 'a proof with an amount its keyset does not issue' }); continue; }
    try { p.C = pointFromHex(p.C).toHex(true); } catch { left.push({ secret: p.secret, why: 'a proof whose signature is not a curve point' }); continue; }
    if (p.dleq && !hasValidDleq(p, { id: p.id, keys: e.keys })) { left.push({ secret: p.secret, why: 'a proof whose proof of the mint\'s signature does not check out' }); continue; }
    if (ok.some((q) => q.secret === p.secret)) continue;   // another copy of it was already taken
    ok.push(p);
  }
  // A secret kept from another copy is not reported as left out.
  const taken = new Set(ok.map((p) => p.secret));
  return { ok, left: left.filter((x) => !taken.has(x.secret)).map((x) => x.why), later, laterProofs };
}
async function swapBearer(gen, proofs) {
  const sorted = await sortPasted(proofs);
  if (!sorted.ok.length) {
    if (sorted.later) throw sorted.later;
    throw new Refusal(`nothing in that token could be received. Left out: ${[...new Set(sorted.left)].join('; ')}; ask the sender for it again.`);
  }
  proofs = sorted.ok;
  // Each proof on its own: the ones the mint calls unspent are swapped, the
  // spent ones are left (and said), and if the mint has not answered for
  // some, those can be received by pasting the token again later.
  // A proof that an unfinished swap of this vault already takes in is not
  // swapped a second time: that swap is retried instead.
  const taking = new Map();
  for (const [oid, o] of openOps(st)) if (o.kind === 'bearer' && o.status === 'open') for (const p of o.inputs) taking.set(p.secret, oid);
  const already = new Set(proofs.filter((p) => taking.has(p.secret)).map((p) => taking.get(p.secret)));
  for (const oid of already) { if (!inFlight.has(oid) && st.ops[oid].status === 'open') { try { await settle(gen, oid, 'again'); } catch { /* listed as unfinished */ } } }
  const all = proofs.filter((p) => !taking.has(p.secret)).map(({ dleq, ...p }) => p);
  const busySaid = already.size ? ' Part of that token was already being received; that swap was tried again and is listed under unfinished if it is not done.' : '';
  if (!all.length) return { said: busySaid.trim(), skipped: true };
  const states = await stateOf(all.map((p) => yOf(p.secret)));
  const plain = all.filter((p, i) => states[i] === 'UNSPENT');
  const spentSum = all.filter((p, i) => states[i] === 'SPENT').reduce((t, p) => t + p.amount, 0);
  const waitSum = all.filter((p, i) => states[i] !== 'UNSPENT' && states[i] !== 'SPENT').reduce((t, p) => t + p.amount, 0);
  if (!plain.length) {
    // Part not yet answered for, or not yet checkable, is never called spent.
    if (waitSum || sorted.later) throw new Error(`the mint has not settled ${waitSum ? sats(waitSum) : 'part'} of that token yet; receive it again in a moment`);
    throw new Refusal('that token was already spent, so the mint will not take it');
  }
  // Swapping part now and the rest later costs a second rounded fee; when
  // that would cost more than waiting, wait.
  if (feeSplitCostsMore(plain, all.filter((p, i) => states[i] !== 'UNSPENT' && states[i] !== 'SPENT'), sorted.laterProofs))
    throw new Error('part of that token is not settled yet, and receiving it in two parts would cost an extra mint fee; receive it again in a moment');
  const leftSaid = sorted.left.length ? ` Left out: ${[...new Set(sorted.left)].join('; ')}.` : '';
  const laterSaid = sorted.later ? ` Part of it could not be checked yet (${why(sorted.later)}); receive the same token again later for that part.` : '';
  const extra = `${spentSum ? ` ${sats(spentSum)} of it was already spent and was left.` : ''}${waitSum ? ` The mint has not answered for ${sats(waitSum)} of it yet; receive the same token again in a moment for that part.` : ''}${leftSaid}${laterSaid}`;
  const total = plain.reduce((s, p) => s + p.amount, 0);
  const keep = total - fee(plain);
  if (keep <= 0) throw new Refusal('that ecash is smaller than the mint fee');
  const outs = lockedOutputs(gen, keep);
  await runFresh(gen, openOp(gen, { kind: 'bearer', rid: 'in:' + listTag(plain.map((p) => JSON.stringify([p.amount, p.id, p.secret, p.C]))), inputs: plain, sets: [outs] }));
  return { said: `${sats(keep)} arrived unlocked and is now locked to signer key ${outs[0].keep} (mint fee ${sats(total - keep)}).${extra}${busySaid}`, skipped: !!(spentSum || waitSum || sorted.left.length || sorted.later || already.size) };
}

async function receiveToken() {
  const gen = st.gen;
  // The text is taken once, before anything waits: the box may be edited
  // meanwhile, and it is cleared only if it still holds this text.
  const text = $('token-in').value.trim();
  if (!text) throw new Refusal('paste a token first');
  await ks();
  // A token from a keyset added since the page loaded names it by a short
  // id only the mint's current list resolves.
  // A short keyset id the mint does not list (yet) is kept as it is, so the
  // token still decodes and each of its proofs is judged on its own.
  let tok;
  try { tok = decodeLoose(text); } catch { throw new Refusal('that is not a cashu token this page can read'); }
  if (String(tok.mint || '').replace(/\/$/, '') !== MINT) throw new Refusal(`that token is from ${String(tok.mint || 'no mint')}; this page uses ${MINT}`);
  const proofs = cleanProofs(tok.proofs);
  // Locked means a NUT-10 secret: a JSON array naming its kind. Anything
  // else is bearer ecash.
  const isLocked = (p) => { try { const v = JSON.parse(p.secret); return Array.isArray(v) && typeof v[0] === 'string'; } catch { return false; } };
  const locked = proofs.filter(isLocked);
  if (locked.length && locked.length !== proofs.length)
    throw new Refusal('that token mixes locked and unlocked ecash; ask the sender to send them as two tokens');
  let said, whole = true;
  if (locked.length) {
    if (!st.jar) throw new Refusal('that ecash is locked to something other than your tip jar, so this page cannot take it');
    const r = await keepLockedTip(gen, st.jar.counter, proofs, (t) => msg('recv-msg', t, 'ok'));
    said = (r && r.said) || 'that tip is already in the vault.';
    whole = !r || !r.skipped;
  } else {
    const r = await swapBearer(gen, proofs);
    said = r.said;
    whole = !r.skipped;
  }
  // The box is cleared only when all of the token was handled, and only if
  // it still holds the text that was received.
  if (whole && $('token-in').value.trim() === text) $('token-in').value = '';
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
// page was closed. A wrap is marked handled only once nothing in it is
// left to try; one that met a mint or network failure is tried again a
// little later, and the next time the page opens. The listener belongs to
// one pairing and one jar key: forget the signer and it closes, and work it
// started is refused if either changed meanwhile.
let jarPool = null, jarQueue = Promise.resolve();
function listenJar() {
  if (jarPool || !owner || !st || !st.jar || !st.jar.nsk) return;
  const { sk, pk } = jarKeys();
  const w = { gen: st.gen, counter: st.jar.counter, sk };
  const pool = jarPool = new SimplePool();
  pool.subscribe(RELAYS, { kinds: [1059], '#p': [pk] }, {
    onevent: (ev) => { jarQueue = jarQueue.then(() => onWrap(w, ev, 0)); },
    // Every relay closed the subscription: not listening any more. It says
    // so and connects again in a minute, for the pairing still open.
    onclose: () => {
      if (jarPool !== pool) return;
      try { pool.close(RELAYS); } catch { /* already closed */ }
      jarPool = null;
      msg('jar-msg', 'Not connected to the tip relays; trying again in a minute.', 'bad');
      setTimeout(() => { if (live(w)) listenJar(); }, 60000);
    },
  });
  msg('jar-msg', 'Listening for tips over Nostr while this page is open.');
}
function stopJar() {
  if (jarPool) { try { jarPool.close(RELAYS); } catch { /* already closed */ } }
  jarPool = null;
}
const markSeen = (w, id) => commit(w.gen, (s) => {
  if (!s.jar || s.jar.counter !== w.counter) return;
  s.jar.seen = [...(s.jar.seen || []).filter((x) => x !== id), id].slice(-2000);
});
const live = (w) => st && st.gen === w.gen && st.jar && st.jar.counter === w.counter;
async function onWrap(w, ev, attempt) {
  if (!live(w) || (st.jar.seen || []).includes(ev.id)) return;
  let payload;
  try { payload = JSON.parse(unwrapEvent(ev, w.sk).content); } catch { return; }   // not for us, or not a tip
  const say = (t) => msg('jar-msg', `Tip over Nostr: ${t}`, 'ok');
  try {
    await ks();
    if (!payload || typeof payload !== 'object') throw new Refusal('a Nostr message that is not a tip');
    if (String(payload.mint || '').replace(/\/$/, '') !== MINT) throw new Refusal(`a tip from another mint (${payload.mint})`);
    const proofs = cleanProofs(payload.proofs);
    const pub = pubAt(st.xpub, w.counter);
    if (!proofs.some((p) => signerLock(pub, p.secret)))
      throw new Refusal('a tip came over Nostr without the lock the tip jar asks for, and was left; ask the sender to pay the tip jar again');
    const r = await keepLockedTip(w.gen, w.counter, proofs, say);
    markSeen(w, ev.id);
    if (r) say(r.said);
  } catch (e) {
    if (!live(w)) return;
    if (e instanceof Refusal) {
      try { markSeen(w, ev.id); } catch { /* vault gone */ }
      msg('jar-msg', `A tip arrived over Nostr but was not kept: ${e.message || e}`, 'bad');
    } else {
      if (attempt < 10) {
        msg('jar-msg', `A tip arrived over Nostr and is waiting for the mint (${e.message || e}); it is tried again.`, 'bad');
        setTimeout(() => { jarQueue = jarQueue.then(() => onWrap(w, ev, attempt + 1)); }, 30000);
      } else {
        msg('jar-msg', `A tip over Nostr is still waiting for the mint (${e.message || e}); it is tried again the next time this page opens.`, 'bad');
      }
    }
  }
}

// A tip that came locked is kept as it is, with no swap: the payer cannot
// take it back, since only the signer can spend it. Checked first, because
// the page cannot spend it to find out later: the lock (above), the keyset
// (checked keys, not expired, a key for the amount), the mint's own proof
// that it signed each one (DLEQ), and the mint's word, matched by Y, that
// each one is unspent. Proofs already known to the vault are skipped, so a
// tip delivered twice is kept once. Proofs the mint calls spent are kept
// apart (see below); the rest join the vault. If the mint has not answered
// for some, those are tried again (an error after the others are kept).
// Returns what was kept, or null when nothing in it was new.
async function keepLockedTip(gen, counter, proofs, progress) {
  // Each proof is judged on its own: a bad one is left out, one the mint
  // cannot settle yet waits for the next try, and the good ones beside them
  // are still kept. A wallet may name a keyset by its short id (the first
  // 16 hex of a 01 id): it is read as the full id the mint lists.
  await ks();
  const pub = pubAt(st.xpub, counter);
  const isKnown = (x) => !!st.coins[x] || !!st.spent[tag(x)];
  const kept = [], left = [], taken = new Set();
  let later = null;
  const looked = new Map();   // one lookup per keyset for this receive, its result or its error
  const look = (id) => { if (!looked.has(id)) looked.set(id, fullId(id).then((full) => keysOf(full).then((e) => ({ full, e })))); return looked.get(id); };
  for (const p of proofs) {
    if (!signerLock(pub, p.secret)) { left.push({ why: 'a proof locked to something other than your tip jar', secret: p.secret }); continue; }
    if (isKnown(p.secret) || taken.has(p.secret)) continue;
    let e;
    try {
      const r = await look(p.id);
      p.id = r.full;
      e = r.e;
    } catch (x) {   // a mint side problem waits for the next try, never final
      if (x instanceof Refusal) { left.push({ why: 'a proof naming a keyset this mint does not use', secret: p.secret }); continue; }
      later = later || x;
      continue;
    }
    if (!(String(p.amount) in e.keys)) { left.push({ why: 'a proof with an amount its keyset does not issue', secret: p.secret }); continue; }
    if (expired(e)) { later = later || new Error('the mint says that keyset no longer redeems; it is looked at again later'); continue; }
    let C;
    try { C = pointFromHex(p.C).toHex(true); } catch { left.push({ why: 'a proof whose signature is not a curve point', secret: p.secret }); continue; }
    if (!p.dleq || !hasValidDleq(p, { id: p.id, keys: e.keys })) { left.push({ why: 'a proof without the mint\'s proof that it signed it', secret: p.secret }); continue; }
    kept.push({ amount: p.amount, id: p.id, secret: p.secret, C });
    taken.add(p.secret);
  }
  // A secret kept from another copy is not reported as left out.
  const leftOut = left.filter((x) => !taken.has(x.secret) && !isKnown(x.secret));
  const leftSaid = leftOut.length ? ` Left out: ${[...new Set(leftOut.map((x) => x.why))].join('; ')}.` : '';
  if (!kept.length) {
    if (later) throw later;
    if (leftOut.length) throw new Refusal(`nothing in that could be kept.${leftSaid}`);
    return null;
  }
  const skipped = leftOut.length > 0;
  // Proofs the mint calls unspent join the vault. Ones it calls spent are
  // genuine (their DLEQ held) and locked to the signer, so only the signer
  // could have spent them: they are held apart, kept, and come back if the
  // mint later calls them unspent. Any other answer is asked again.
  const states = await stateOf(kept.map((p) => yOf(p.secret)));
  const good = kept.filter((p, i) => states[i] === 'UNSPENT');
  const held = kept.filter((p, i) => states[i] === 'SPENT');
  const waiting = states.filter((x) => x !== 'UNSPENT' && x !== 'SPENT').length;
  const [added, apart] = good.length || held.length ? commit(gen, (s) => {
    if (!s.jar || s.jar.counter !== counter) throw new Refusal('the tip jar changed while this tip was checked');
    return [good.filter((p) => credit(s, p, counter)), held.filter((p) => credit(s, p, counter, true))];
  }) : [[], []];
  const total = added.reduce((t, p) => t + p.amount, 0), apartSum = apart.reduce((t, p) => t + p.amount, 0);
  let said = total ? `${sats(total)} arrived locked to your signer; kept as it is, no fee.${leftSaid}` : leftSaid.trim();
  if (apartSum) said += `${said ? ' ' : ''}${sats(apartSum)} of it the mint calls already spent; it is kept apart and comes back if the mint says otherwise.`;
  if (waiting || later) {
    if (said) progress(said);
    throw later || new Error('the mint has not confirmed part of the tip yet');
  }
  return said ? { said, skipped } : null;
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
// first (least change), the one smallest proof that covers it alone, and
// largest first among proofs no bigger than each amount the key holds
// (which leaves a huge proof out when smaller ones suffice). makeRequest
// takes the first that fits the signer's limits. Only spendable proofs
// (below) whose keyset's keys are checked, and that no pending spend
// already uses, are considered.
//
// Ecash a spend may use, and the headline counts: not refused by the
// signer, its keyset not expired (when its keys have been read), and not
// called pending by the mint at the last check.
const spendable = (p) => !p.apart && !mintPending.has(p.secret) && !(verified.has(p.id) && expired(verified.get(p.id)));
function chooseInputs(amount) {
  const inUse = busy(st);
  const usable = (p) => spendable(p) && !inUse.has(p.secret) && verified.has(p.id);
  const tries = [], seen = new Set();
  const add = (counter, take) => {
    const total = take.reduce((s, p) => s + p.amount, 0);
    const key = take.map((p) => p.secret).sort().join('\n');
    if (!take.length || take.length > MAX_IN || total < amount + fee(take) || seen.has(key)) return;
    seen.add(key);
    tries.push({ counter, proofs: take, total });
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
    const ok = all.filter(usable);
    const desc = [...ok].sort((a, b) => b.amount - a.amount || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0)), asc = [...desc].reverse();
    add(counter, greedy(desc));
    add(counter, greedy(asc));
    const one = asc.find((p) => p.amount >= amount + fee([p]));
    if (one) add(counter, [one]);
    for (const cap of new Set(ok.map((p) => p.amount))) add(counter, greedy(desc.filter((p) => p.amount <= cap)));
    for (const take of search(amount, desc)) add(counter, take);
  }
  return tries.sort((a, b) => a.proofs.length - b.proofs.length || a.total - b.total);
}
// A bounded search over one key's proofs (largest first, at most 20,000
// steps) for the sets that cover the payment with the fewest inputs plus
// change pieces, which is what decides the request's size.
function search(amount, desc) {
  const denoms = Object.keys(keyset.keys).map(Number).filter(Number.isSafeInteger).sort((a, b) => b - a);
  const pieces = (n) => { let c = 0; for (const d of denoms) { c += Math.floor(n / d); n %= d; } return c; };
  const suffix = [];
  for (let i = desc.length - 1, t = 0; i >= 0; i--) suffix[i] = t += desc[i].amount;
  const best = [], take = [];
  let steps = 0;
  const visit = (i, total) => {
    if (++steps > 20000) return;
    const need = amount + fee(take);
    if (total >= need) {
      best.push({ score: take.length + pieces(total - need), take: [...take] });
      best.sort((a, b) => a.score - b.score);
      if (best.length > 5) best.pop();
      return;
    }
    if (i >= desc.length || take.length >= MAX_IN || total + suffix[i] < need) return;
    take.push(desc[i]);
    visit(i + 1, total + desc[i].amount);
    take.pop();
    let j = i + 1;
    while (j < desc.length && desc[j].amount === desc[i].amount && desc[j].id === desc[i].id) j++;   // leaving one out leaves out all of that amount and keyset here
    visit(j, total);
  };
  visit(0, 0);
  return best.map((b) => b.take);
}

async function makeRequest() {
  const gen = st.gen;
  await ks();
  await vaultKeys();
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
  openReq = { gen, counter: pick.counter, inputs: pick.proofs, outs, hash: sigallHash(pick.proofs, outs) };
  $('req-text').value = req;
  showFrames(req);
  $('req-box').classList.remove('hidden');
  msg('send-msg', `${sats(amount)} to send, ${sats(change)} back to the vault, mint fee ${sats(fee(pick.proofs))}. Your signer will show the same.`);
}

async function submitReply(text) {
  if (!openReq) throw new Refusal('no spend request open');
  let rep;
  try { rep = JSON.parse(text.trim()); } catch { throw new Refusal('that is not the signer\'s answer'); }
  holdNext = false;
  if (!rep || rep.v !== 1 || rep.hash !== openReq.hash || !/^[0-9a-f]{128}$/.test(rep.sig || ''))
    throw new Refusal('that answer is for a different request');
  // The signature itself is checked here, against the signer's key for these
  // inputs and the hash this page computed, before anything is saved: a
  // mistyped answer leaves the request open for the right one.
  let good = false;
  try { good = schnorr.verify(unhex(rep.sig), unhex(openReq.hash), unhex(pubAt(st.xpub, openReq.counter).slice(2))); } catch { /* below */ }
  if (!good) throw new Refusal('that answer\'s signature does not check out; scan or paste the signer\'s answer again');
  const { gen, outs, hash } = openReq;
  const inputs = openReq.inputs.map((p, i) => {
    const q = { amount: p.amount, id: p.id, secret: p.secret, C: p.C };
    if (i === 0) q.witness = JSON.stringify({ signatures: [rep.sig] });
    return q;
  });
  const id = openOp(gen, { kind: 'spend', rid: `sig:${hash}`, inputs, sets: [outs] });
  openReq = null;
  stopFrames();
  $('req-box').classList.add('hidden');
  $('reply-in').value = '';
  msg('send-msg', 'Sending to the mint...');
  await runFresh(gen, id);
  msg('send-msg', '');
}

// ---- payments to hand over ----
// A payment waits, across reloads, until the owner says it was sent or the
// mint says it was redeemed; every payment is kept and listed, token and
// all. The token on
// screen and its QR are always the same payment: the QR is cleared before
// a new one is drawn, and a token too long for one QR shows as text only.
// The payment on screen stays until it is marked sent; one that finishes
// meanwhile only adds to the waiting count. "I've sent it" acts on the
// payment it was shown with, and after it no payment opens by itself until
// the owner asks for the next one or starts a new spend, so a second tap
// never lands on a payment that just appeared.
let shownKey = null, shownToken = null, holdNext = false;
// Redeemed: the mint said every proof is spent (gone is how an earlier
// version wrote it).
const isRedeemed = (p) => !!p.redeemed || p.gone === true;
const waitingPays = () => Object.entries((st && st.pays) || {}).filter(([, p]) => !p.sent && !isRedeemed(p)).sort(([, a], [, b]) => a.at - b.at);
function renderOutbox() {
  const ob = waitingPays();
  const has = (k) => ob.some(([key]) => key === k);
  if (lastPay && shownKey === null && !holdNext && has(lastPay)) shownKey = lastPay;
  lastPay = null;
  if (shownKey !== null && !has(shownKey)) shownKey = null;
  const cur = shownKey && st.pays[shownKey];
  if (cur) {
    if (shownToken !== cur.token) {
      shownToken = cur.token;
      const out = $('token-out');
      out.value = cur.token;
      out.style.height = 'auto';
      out.style.height = out.scrollHeight + 2 + 'px';
      const fitsQR = drawQR($('token-qr'), cur.token);
      $('token-qr').classList.toggle('hidden', !fitsQR);
      $('token-qr-note').classList.toggle('hidden', fitsQR);
    }
    $('token-done').dataset.key = shownKey;
    $('token-box').classList.remove('hidden');
  } else {
    shownToken = null;
    $('token-done').dataset.key = '';
    $('token-box').classList.add('hidden');
  }
  const waiting = ob.filter(([key]) => key !== shownKey).length;
  $('next-text').textContent = waiting ? `${waiting} payment${waiting > 1 ? 's' : ''} waiting to be handed over.` : '';
  $('next-box').classList.toggle('hidden', !waiting);
  $('token-next').classList.toggle('hidden', !!cur);
  const when = (p) => Math.max(p.sent || 0, p.redeemed || 0);
  const sent = Object.values((st && st.pays) || {}).filter((p) => p.sent || isRedeemed(p)).sort((a, b) => when(b) - when(a));
  $('sent-box').classList.toggle('hidden', !sent.length);
  $('sent-count').textContent = sent.length;
  $('sent-text').value = sent.map((x) => `${sats(x.amount)}, ${isRedeemed(x) ? `redeemed${x.redeemed ? ` ${new Date(x.redeemed).toLocaleString()}` : ''}` : `marked sent ${new Date(x.sent).toLocaleString()}, not redeemed yet`}${x.token && !isRedeemed(x) ? `\n${x.token}` : ''}`).join('\n\n');
}
function sentIt() {
  const key = $('token-done').dataset.key;
  if (!key || key !== shownKey) return;
  commit(st.gen, (s) => {
    const x = s.pays[key];
    if (x && !x.sent) x.sent = Date.now();
  });
  shownKey = null;   // only once the save went through
  holdNext = true;
  renderOutbox();
}
function showNext() {
  holdNext = false;
  const ob = waitingPays();
  if (ob.length) shownKey = ob[0][0];
  renderOutbox();
}

// ---- QR out ----
let frameTimer = null;
// Draws text as one QR code, or clears the canvas and returns false when
// the text is too long for one: an older code is never left on screen.
function drawQR(canvas, text) {
  canvas.width = canvas.height = 0;
  let q;
  try {
    q = qrcode(0, 'L');
    q.addData(text, 'Byte');
    q.make();
  } catch { return false; }
  const n = q.getModuleCount(), pad = 4, px = 6;
  canvas.width = canvas.height = (n + 2 * pad) * px;
  const g = canvas.getContext('2d');
  g.fillStyle = '#fff'; g.fillRect(0, 0, canvas.width, canvas.height);
  g.fillStyle = '#000';
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
    if (q.isDark(y, x)) g.fillRect((x + pad) * px, (y + pad) * px, px, px);
  return true;
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
// A new pairing never replaces a vault that still holds ecash: that takes
// Forget first, which asks. Forget, and pairing a different signer over a
// vault with no ecash, keep the vault as it was under a new key of its own
// (one per Forget, never written over). Pairing a signer again brings back
// the newest readable copy for it, with everything in it, under a new
// pairing id, so nothing started before it was forgotten can write to it.
// A copy that cannot be read is left where it is, untouched, and never
// copied or replaced; a storage read that fails stops the pairing.
// Each copy carries a revision one above every copy of that signer before
// it (not a clock time, which can go backward) and a fingerprint of its
// contents; a copy identical to the newest one is not saved again.
// A copy is the vault plus its revision, fingerprint and time.
const stripCopy = (v) => { const { rev, snap, forgottenAt, ...rest } = v; return rest; };
const snapOf = (v) => { const { gen, ...rest } = stripCopy(v); return tag(JSON.stringify(rest)); };
function keepCopy(v) {
  const { found, unread } = copiesOf(v.xpub);
  const snap = snapOf(v);
  const unreadRev = Math.max(0, ...unread.map((k) => Number(k.split(':').pop()) || 0));
  // The newest readable copy is enough when its contents (fingerprint
  // computed again, not trusted from the copy) equal this vault and it is
  // newer than every copy that cannot be read.
  if (found[0] && snapOf(found[0]) === snap && (Number(found[0].rev) || 0) > unreadRev) return;
  const rev = Math.max(unreadRev, ...found.map((x) => Number(x.rev) || 0)) + 1;
  localStorage.setItem(`${FORGOTTEN}${v.xpub}:${rand32().slice(0, 8)}:${rev}`, JSON.stringify({ ...v, rev, snap, forgottenAt: Date.now() }));
}
function copiesOf(xpub) {
  const found = [], unread = [];
  const keys = [];
  try {
    for (let i = 0; i < localStorage.length; i++) keys.push(localStorage.key(i));
  } catch (e) { throw new Unreadable(`this browser would not let the page read its storage (${e.message || e})`); }
  for (const k of keys) {
    if (!k || (k !== FORGOTTEN + xpub && !k.startsWith(`${FORGOTTEN}${xpub}:`))) continue;
    let raw;
    try { raw = localStorage.getItem(k); } catch (e) { throw new Unreadable(`this browser would not let the page read its storage (${e.message || e})`); }
    let v = null;
    try { v = JSON.parse(raw); } catch { /* below */ }
    if (isObj(v) && check(stripCopy(v)) && v.xpub === xpub) found.push(v); else unread.push(k);
  }
  return { found: found.sort((a, b) => (Number(b.rev) || 0) - (Number(a.rev) || 0) || (Number(b.forgottenAt) || 0) - (Number(a.forgottenAt) || 0)), unread };
}
function pair(text) {
  const t = text.trim();
  const m = /^\[([0-9a-f]{8})\/129373h\/10h\/0h\/0h\](xpub[1-9A-HJ-NP-Za-km-z]{100,112})$/.exec(t);
  if (!m) throw new Error('that is not a KISS Signer ecash key');
  HDKey.fromExtendedKey(m[2]);   // throws on a bad checksum
  const cur = load();
  if (cur && cur.xpub === t) { render(); return; }
  if (holds(cur)) throw new Refusal('this page still holds another signer\'s vault; forget it first');
  const { found, unread } = copiesOf(t);
  // A copy that cannot be read might be the newest one: the owner decides
  // before an older readable copy is brought back.
  if (unread.length && found.length && !confirm(`This browser holds ${unread.length === 1 ? 'a copy' : `${unread.length} copies`} of this signer's forgotten vault that cannot be read, and might be newer. Bring back the newest copy that can be read anyway? (The unreadable one is kept, untouched.)`))
    throw new Refusal('pairing stopped; nothing was changed');
  // Another signer's vault with no ecash can still hold a tip jar key or
  // payment history: it is kept, as Forget would keep it.
  if (cur) keepCopy(cur);
  stopJar();
  clearSignerViews();
  let back = found[0] || null;
  if (back) {
    back = { ...stripCopy(back), gen: rand32() };
  }
  save(back || fresh(t, m[1]));
  st = load();
  render();
  const notes = [];
  if (back) notes.push('This signer\'s vault, as it was when you last forgot it, is back.');
  if (unread.length) notes.push(`${unread.length === 1 ? 'A copy' : `${unread.length} copies`} of this signer's forgotten vault could not be read; ${unread.length === 1 ? 'it is' : 'they are'} kept in this browser, untouched.`);
  if (notes.length) msg('recv-msg', notes.join(' '), unread.length ? 'bad' : 'ok');
  listenJar();
  recoverAll(st.gen).catch(() => { /* listed as unfinished */ });
}
// Forget keeps a copy of the vault (see above) and refuses to touch a vault
// it cannot read.
function forget() {
  let cur;
  try { cur = load(); } catch (e) {
    alert(`${e.message}. Nothing was removed.`);
    return;
  }
  if (holds(cur) && !confirm('This vault still holds ecash or unfinished swaps. Forget it anyway? Your signer can still spend its ecash, and pairing this signer again in this browser brings the vault back.')) return;
  try {
    if (cur) keepCopy(cur);
    localStorage.removeItem(STORE);
  } catch (e) {
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
  for (const id of ['req-box', 'jar-box', 'token-box', 'next-box']) $(id).classList.add('hidden');
  for (const id of ['jar-qr', 'jar-qr-inline', 'req-qr', 'token-qr']) $(id).width = 0;
  for (const id of ['jar-text', 'req-text', 'token-out', 'reply-in']) $(id).value = '';
  shownKey = shownToken = null;
}

// ---- view ----
// Coins by the signer key they are locked to (held apart ones left out).
function groupBy() {
  const g = new Map();
  for (const p of Object.values(st.coins)) {
    if (p.apart) continue;
    if (!g.has(p.counter)) g.set(p.counter, []);
    g.get(p.counter).push(p);
  }
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
  // The headline is what a spend can use: the same rule chooseInputs
  // follows. Ecash from an expired keyset, or that the mint last called
  // pending, is shown apart, beside its key.
  const coins = Object.values(st.coins);
  const bal = sum(coins.filter(spendable));
  $('bal').textContent = bal;
  $('bal-unit').textContent = bal === 1 ? 'sat' : 'sats';
  $('mint-url').textContent = MINT;
  $('fp').textContent = st.fp;
  const g = [...groupBy().entries()].map(([c, ps]) => {
    const exp = sum(ps.filter((p) => verified.has(p.id) && expired(verified.get(p.id))));
    const wait = sum(ps.filter((p) => mintPending.has(p.secret)));
    const notes = [exp && `${exp} from an expired keyset`, wait && `${wait} the mint calls pending`].filter(Boolean);
    return `key ${c}: ${sum(ps)}${notes.length ? ` (${notes.join('; ')})` : ''}`;
  });
  $('groups').textContent = g.length ? g.join(', ') : 'nothing yet';
  // Every unfinished operation shows here from the moment it is saved,
  // except one this tab is sending right now.
  const ops = openOps(st);
  const un = ops.filter(([id, o]) => o.status === 'open' && !inFlight.has(id));
  const asideOps = ops.filter(([, o]) => o.status === 'aside');
  const apart = coins.filter((c) => c.apart);
  const parts = [];
  if (un.length) parts.push(`${un.length} unfinished swap${un.length > 1 ? 's' : ''}, kept and tried again on every load. Oldest: ${un[0][1].note || 'not finished yet'}`);
  if (asideOps.length) parts.push(`${asideOps.length} set aside, still kept and looked for at the mint${asideOps[0][1].note ? ` (oldest: ${asideOps[0][1].note})` : ''}.`);
  if (apart.length) parts.push(`${sats(sum(apart))} held apart because the mint calls it spent; kept, and it comes back if the mint calls it unspent.`);
  if (balanceFailed) parts.push(`The balance could not be checked with the mint (${balanceFailed}); tap Try again.`);
  const pend = coins.filter((c) => !c.apart && mintPending.has(c.secret));
  if (pend.length) parts.push(`${sats(sum(pend))} the mint calls pending, so it is not counted yet; it is checked again every minute.`);
  $('stuck-text').textContent = parts.join(' ');
  $('stuck-box').classList.toggle('hidden', !parts.length);
  $('stuck-aside').classList.toggle('hidden', !un.length);
  $('stuck-resume').classList.toggle('hidden', !asideOps.length);
  $('stuck-retry').classList.toggle('hidden', !parts.length);
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
$('token-done').onclick = guard('send-msg', sentIt);
$('token-next').onclick = guard('send-msg', showNext);
$('stuck-retry').onclick = guard('recv-msg', () => recoverAll(st.gen));
$('stuck-aside').onclick = guard('recv-msg', setAside);
$('stuck-resume').onclick = guard('recv-msg', resume);
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
// reload can briefly overlap the closing page, hence the few retries. If
// the page cannot start, it lets the lock go and says why.
function start() {
  owner = true;
  $('tab-card').classList.add('hidden');
  try { st = load(); } catch (e) {
    if (!(e instanceof Unreadable)) throw e;
    // Kept as it is: nothing replaces or removes it, and its text is shown
    // so it can be copied out.
    blocked(`${e.message[0].toUpperCase()}${e.message.slice(1)}. Nothing was changed: the page will not replace or remove it. Its text is below, to copy somewhere safe.`);
    $('tab-raw').value = e.raw || '';
    $('tab-raw').classList.toggle('hidden', !e.raw);
    return false;
  }
  if (st && waitingPays().length) shownKey = waitingPays()[0][0];
  render();
  if (!st) return;
  listenJar();
  recoverAll(st.gen).catch((e) => msg('recv-msg', `An unfinished swap is still not done (${why(e)}). ${KEPT}`, 'bad'));
}
function blocked(text) {
  owner = false;
  $('tab-msg').textContent = text;
  $('tab-card').classList.remove('hidden');
  try { render(); } catch { /* the note is what matters */ }
}
function takeLock(tries) {
  let release = null;
  const held = new Promise((r) => { release = r; });
  navigator.locks.request('kiss-ecash-vault', { ifAvailable: true }, (lock) => {
    if (!lock) {
      if (tries > 0) setTimeout(() => takeLock(tries - 1), 700);
      else blocked('This vault is already open in another tab or window. Use that one, or close it and reload this page.');
      return undefined;
    }
    try { if (start() === false) release(); } catch (e) {
      release();
      blocked(`The vault could not start in this browser (${e.message || e}). Reload the page to try again.`);
    }
    return held;   // kept until the tab closes
  }).catch((e) => blocked(`This browser would not let the vault take its tab lock (${e.message || e}). Open the page in Brave or Chrome.`));
}
if (navigator.locks && navigator.locks.request) takeLock(4);
else blocked('This browser cannot keep two tabs of the vault apart. Open the page in Brave or Chrome.');
