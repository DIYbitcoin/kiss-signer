// KISS ecash vault: a coordinator for docs/specs/ecash-spend-request.md.
//
// Everything it stores is locked to the signer's keys (NUT-11 P2PK, SIG_ALL,
// keys from the signer's NUT-13 export), so a copy of this page's storage
// cannot spend anything. The only bearer ecash it ever holds is a payment on
// its way out, which the signer has already shown the owner.
import { blindMessage, unblindSignature, pointFromHex, createP2PKsecret,
  getEncodedToken, getDecodedToken, hashToCurve, hasValidDleq,
  PaymentRequest } from './vendor/cashu-cashu-ts/lib/cashu-ts.es.js';
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
const rand32 = () => hex(crypto.getRandomValues(new Uint8Array(32)));

// ---- state ----
let st = load();
function load() {
  try { return JSON.parse(localStorage.getItem(STORE)) || null; } catch { return null; }
}
function save() {
  try { localStorage.setItem(STORE, JSON.stringify(st)); } catch { /* private window */ }
  render();
}

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
  keyset = { id: k.id, ppk: k.input_fee_ppk || 0, keys, ids: all.map((x) => x.id) };
  return keyset;
}
const fee = (n) => Math.ceil((keyset.ppk * n) / 1000);
function split(amount) {
  const out = [];
  const denoms = Object.keys(keyset.keys).map(Number).sort((a, b) => b - a);
  for (const d of denoms) while (amount >= d) { out.push(d); amount -= d; }
  if (amount) throw new Error('amount cannot be split into this mint\'s denominations');
  return out;
}

// ---- keys ----
function pubAt(i) {
  const xpub = st.xpub.slice(st.xpub.indexOf(']') + 1);
  return hex(HDKey.fromExtendedKey(xpub).deriveChild(i).publicKey);
}
function nextCounter() { return st.next++; }
// The change lock exactly as the signer rebuilds it (spec: no spaces).
const changeSecret = (pub, nonce) =>
  `["P2PK",{"nonce":"${nonce}","data":"${pub}","tags":[["sigflag","SIG_ALL"]]}]`;

function blind(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, secret, r: bm.r, B_: bm.B_.toHex(true) };
}
function unblind(o, sig) {
  const C = unblindSignature(pointFromHex(sig.C_), o.r, pointFromHex(keyset.keys[o.amount]));
  return { amount: o.amount, id: keyset.id, secret: o.secret, C: C.toHex(true) };
}
const wire = (o) => ({ amount: o.amount, id: keyset.id, B_: o.B_ });

// Fresh outputs locked to one new key of the signer.
function lockedOutputs(amount) {
  const counter = nextCounter();
  const pub = pubAt(counter);
  return { counter, outs: split(amount).map((a) => blind(a, createP2PKsecret(pub, SIGALL))) };
}

// ---- receive ----
async function mintTest() {
  const amount = Number($('mint-amt').value);
  await ks();
  msg('recv-msg', 'Asking the test mint...');
  const q = await api('/v1/mint/quote/bolt11', { amount, unit: 'sat' });
  for (let i = 0; i < 30; i++) {
    if ((await api(`/v1/mint/quote/bolt11/${q.quote}`)).state === 'PAID') break;
    await new Promise((r) => setTimeout(r, 500));
  }
  const { counter, outs } = lockedOutputs(amount);
  const m = await api('/v1/mint/bolt11', { quote: q.quote, outputs: outs.map(wire) });
  st.proofs.push(...outs.map((o, i) => ({ ...unblind(o, m.signatures[i]), counter })));
  save();
  msg('recv-msg', `${amount} sat locked to signer key ${counter}.`, 'ok');
}

// ---- tip jar ----
// One printable NUT-18 payment request whose nut10 field asks the payer's
// wallet to lock the tip to one key of the signer, SIG_ALL, nothing else.
// Every tip shares that key, which is what lets one spend take them all
// (NUT-11 wants every SIG_ALL input on the same key); the cost is that the
// mint can link the tips to each other.
function jarRequest() {
  if (!st.jar) { st.jar = { counter: nextCounter() }; save(); }
  const pr = new PaymentRequest(undefined, `kiss${st.fp}`, undefined, 'sat', [MINT],
    'tips to a KISS Signer vault', false,
    { kind: 'P2PK', data: pubAt(st.jar.counter), tags: SIGALL });
  return pr.toEncodedRequest();
}
function showJar() {
  const creq = jarRequest();
  const t = $('jar-text');
  t.value = creq;
  drawQR($('jar-qr'), creq);
  $('jar-box').classList.remove('hidden');
  t.style.height = 'auto';
  t.style.height = t.scrollHeight + 2 + 'px';
}

// A lock the signer will sign for: exactly ["P2PK",{nonce,data,tags}] with
// data the jar key and tags [["sigflag","SIG_ALL"]], written without spaces
// or escapes, since the signer refuses anything else and a tip it refuses
// is a tip nobody can spend.
function jarLocked(secret) {
  if (!st.jar || secret.includes('\\')) return false;
  let v;
  try { v = JSON.parse(secret); } catch { return false; }
  if (!Array.isArray(v) || v.length !== 2 || v[0] !== 'P2PK' || JSON.stringify(v) !== secret) return false;
  const o = v[1];
  return o && typeof o === 'object' && Object.keys(o).sort().join() === 'data,nonce,tags' &&
    typeof o.nonce === 'string' && o.nonce.length > 0 && o.data === pubAt(st.jar.counter) &&
    JSON.stringify(o.tags) === JSON.stringify(SIGALL);
}

// A tip that came locked is kept as it is, with no swap: the payer cannot
// take it back, since only the signer can spend it. Two things are checked
// first, because the page cannot spend it to find out later: the mint's own
// proof that it signed each one (NUT-12 DLEQ), and that the mint has not
// seen any of them spent (NUT-07).
async function keepLockedTip(proofs) {
  for (const p of proofs) {
    if (st.proofs.some((q) => q.secret === p.secret)) throw new Error('that tip is already in the vault');
    if (p.id !== keyset.id || !p.dleq || !hasValidDleq(p, { id: keyset.id, keys: keyset.keys }))
      throw new Error('that tip carries no proof the mint signed it; ask the sender to send it again');
  }
  const Ys = proofs.map((p) => hashToCurve(enc(p.secret)).toHex(true));
  const states = (await api('/v1/checkstate', { Ys })).states;
  if (states.length !== Ys.length || states.some((x) => x.state !== 'UNSPENT'))
    throw new Error('the mint says part of that tip is already spent');
  st.proofs.push(...proofs.map((p) => ({ amount: p.amount, id: p.id, secret: p.secret, C: p.C, counter: st.jar.counter })));
  $('token-in').value = '';
  save();
  const total = proofs.reduce((s, p) => s + p.amount, 0);
  msg('recv-msg', `${total} sat tip arrived locked to your signer; kept as it is, no fee.`, 'ok');
}

async function receiveToken() {
  await ks();
  const tok = getDecodedToken($('token-in').value.trim(), keyset.ids);
  if (tok.mint.replace(/\/$/, '') !== MINT) throw new Error(`that token is from ${tok.mint}; this page uses ${MINT}`);
  // cashu-ts decodes amounts as objects; the mint wants plain integers
  tok.proofs = tok.proofs.map((p) => ({ amount: Number(String(p.amount)), id: p.id, secret: p.secret, C: p.C, dleq: p.dleq }));
  const locked = tok.proofs.filter((p) => p.secret.startsWith('['));
  if (locked.length) {
    if (locked.length !== tok.proofs.length || !locked.every((p) => jarLocked(p.secret)))
      throw new Error('that token is locked to something other than your tip jar, so this page cannot take it');
    return keepLockedTip(tok.proofs);
  }
  // Unlocked: whoever holds the string can spend it, so swap it now into
  // ecash locked to the signer.
  tok.proofs = tok.proofs.map(({ dleq, ...p }) => p);
  const total = tok.proofs.reduce((s, p) => s + p.amount, 0);
  const keep = total - fee(tok.proofs.length);
  if (keep <= 0) throw new Error('that token is smaller than the mint fee');
  const { counter, outs } = lockedOutputs(keep);
  const r = await api('/v1/swap', { inputs: tok.proofs, outputs: outs.map(wire) });
  st.proofs.push(...outs.map((o, i) => ({ ...unblind(o, r.signatures[i]), counter })));
  $('token-in').value = '';
  save();
  msg('recv-msg', `${keep} sat arrived unlocked and is now locked to signer key ${counter} (mint fee ${total - keep}).`, 'ok');
}

// ---- send ----
let pending = null;

// NUT-11 SIG_ALL: the hash the signer's reply must carry.
function sigallHash(inputs, outs) {
  let m = '';
  for (const p of inputs) m += p.secret + p.C;
  for (const o of outs) m += String(o.amount) + o.B_;
  return hex(sha256(enc(m)));
}

async function makeRequest() {
  await ks();
  const amount = Number($('send-amt').value);
  if (!(amount > 0)) throw new Error('enter an amount');
  // SIG_ALL spends proofs that share one key, so pick the smallest key group
  // that covers the payment and its fee.
  const groups = groupBy();
  const pick = [...groups.entries()]
    .map(([counter, proofs]) => ({ counter, proofs, total: proofs.reduce((s, p) => s + p.amount, 0) }))
    .filter((g) => g.total >= amount + fee(g.proofs.length))
    .sort((a, b) => a.total - b.total)[0];
  if (!pick) throw new Error('no single signer key holds that much yet; send a smaller amount');
  const change = pick.total - amount - fee(pick.proofs.length);
  const outs = split(amount).map((a) => ({ ...blind(a, rand32()), change: null }));
  if (change > 0) {
    const cc = nextCounter();
    const pub = pubAt(cc);
    for (const a of split(change)) {
      const nonce = rand32();
      const o = blind(a, changeSecret(pub, nonce));
      outs.push({ ...o, change: { counter: cc, nonce, r: o.r.toString(16).padStart(64, '0') } });
    }
  }
  const req = JSON.stringify({ v: 1, mint: MINT, unit: 'sat', counter: pick.counter,
    inputs: pick.proofs.map((p) => ({ secret: p.secret, C: p.C })),
    outputs: outs.map((o) => (o.change ? { amount: o.amount, B_: o.B_, change: o.change }
                                        : { amount: o.amount, B_: o.B_ })) });
  if (req.length > REQ_MAX) throw new Error(`that request is ${req.length} bytes and the signer takes ${REQ_MAX}; send a smaller amount`);
  pending = { inputs: pick.proofs, outs, hash: sigallHash(pick.proofs, outs), amount, change };
  save();   // the counter moved
  $('req-text').value = req;
  showFrames(req);
  $('req-box').classList.remove('hidden');
  $('token-box').classList.add('hidden');
  msg('send-msg', `${amount} sat to send, ${change} sat back to the vault, mint fee ${fee(pick.proofs.length)}. Your signer will show the same.`);
}

async function submitReply(text) {
  if (!pending) throw new Error('no spend request open');
  let rep;
  try { rep = JSON.parse(text.trim()); } catch { throw new Error('that is not the signer\'s answer'); }
  if (rep.v !== 1 || rep.hash !== pending.hash || !/^[0-9a-f]{128}$/.test(rep.sig || ''))
    throw new Error('that answer is for a different request');
  const inputs = pending.inputs.map((p, i) => {
    const q = { amount: p.amount, id: p.id, secret: p.secret, C: p.C };
    if (i === 0) q.witness = JSON.stringify({ signatures: [rep.sig] });
    return q;
  });
  const r = await api('/v1/swap', { inputs, outputs: pending.outs.map(wire) });
  const got = pending.outs.map((o, i) => ({ o, p: unblind(o, r.signatures[i]) }));
  const spent = new Set(pending.inputs.map((p) => p.secret));
  st.proofs = st.proofs.filter((p) => !spent.has(p.secret));
  for (const g of got) if (g.o.change) st.proofs.push({ ...g.p, counter: g.o.change.counter });
  const pay = got.filter((g) => !g.o.change).map((g) => g.p);
  const token = getEncodedToken({ mint: MINT, unit: 'sat', proofs: pay });
  pending = null;
  stopFrames();
  save();
  $('req-box').classList.add('hidden');
  $('token-box').classList.remove('hidden');
  const out = $('token-out');
  out.value = token;
  // Tall enough for the whole token: a scrolled box hid its first lines.
  out.style.height = 'auto';
  out.style.height = out.scrollHeight + 2 + 'px';
  drawQR($('token-qr'), token);
  msg('send-msg', '');
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
  if (st && st.proofs.length) throw new Error('forget the current signer first');
  st = { xpub: t, fp: m[1], next: 0, proofs: [] };
  save();
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
  $('bal').textContent = st.proofs.reduce((s, p) => s + p.amount, 0);
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
  if (st.proofs.length && !confirm('This vault still holds ecash. Forget it anyway? Your signer can still spend it, but this page will no longer know where it is.')) return;
  st = null;
  try { localStorage.removeItem(STORE); } catch { /* private window */ }
  render();
};
$('mint-btn').onclick = guard('recv-msg', mintTest);
$('token-btn').onclick = guard('recv-msg', receiveToken);
$('send-btn').onclick = guard('send-msg', makeRequest);
$('reply-btn').onclick = guard('send-msg', () => submitReply($('reply-in').value));
$('reply-scan').onclick = guard('send-msg', () => scan(guard('send-msg', submitReply)));
$('req-cancel').onclick = () => { pending = null; stopFrames(); stopCam(); $('req-box').classList.add('hidden'); msg('send-msg', ''); };
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
  if (opened && narrow.matches) $(opened).scrollIntoView({ behavior: 'smooth', block: 'start' });
}
const narrow = matchMedia('(max-width: 899px)');
const stageWatch = new MutationObserver(syncStage);
for (const id of ['pair-card', 'send-card', 'req-box', 'token-box', 'cam-card', 'jar-card', 'jar-box'])
  stageWatch.observe($(id), { attributes: true, attributeFilter: ['class'] });
render();
syncStage();
