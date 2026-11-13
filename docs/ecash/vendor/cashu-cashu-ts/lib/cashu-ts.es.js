import { sha256 as e, sha512 as t } from "@noble/hashes/sha2.js";
import { bytesToHex as n, concatBytes as r, hexToBytes as i, randomBytes as a, utf8ToBytes as o } from "@noble/hashes/utils.js";
import { base64 as s, base64nopad as c, base64url as l, base64urlnopad as u, bech32 as d, bech32m as f } from "@scure/base";
import { pbkdf2 as p } from "@noble/hashes/pbkdf2.js";
import { schnorr as m, secp256k1 as h } from "@noble/curves/secp256k1.js";
import { bytesToNumberBE as g, equalBytes as _, numberToBytesBE as v } from "@noble/curves/utils.js";
import { HARDENED_OFFSET as y, HDKey as b } from "@scure/bip32";
import { hmac as x } from "@noble/hashes/hmac.js";
//#region src/model/Errors.ts
var S = class e extends Error {
	constructor(t, n) {
		super(t), this.name = "CTSError", n?.cause !== void 0 && Object.defineProperty(this, "cause", {
			configurable: !0,
			enumerable: !1,
			value: n.cause,
			writable: !0
		}), Object.setPrototypeOf(this, e.prototype);
	}
}, C = class e extends S {
	constructor(t, n, r) {
		super(t, r), this.status = n, this.name = "HttpResponseError", Object.setPrototypeOf(this, e.prototype);
	}
}, w = class e extends S {
	constructor(t, n) {
		super(t, n), this.name = "NetworkError", Object.setPrototypeOf(this, e.prototype);
	}
}, T = class e extends w {
	constructor(t) {
		super(t), this.name = "CallerAbortError", Object.setPrototypeOf(this, e.prototype);
	}
}, E = class e extends w {
	constructor(t, n) {
		super(t, n), this.name = "UncancellableReadError", Object.setPrototypeOf(this, e.prototype);
	}
}, D = class e extends S {
	constructor(t) {
		super(t), this.name = "AmountError", Object.setPrototypeOf(this, e.prototype);
	}
}, ee = class e extends S {
	constructor(t) {
		super(t), this.name = "AmountWithUnitError", Object.setPrototypeOf(this, e.prototype);
	}
}, te = class e extends S {
	constructor(t, n) {
		let r = n?.refreshed ?? !1, i = n?.cause === void 0 ? r ? `Keyset '${t}' is not a keyset of this mint` : `Keyset '${t}' is not in the wallet snapshot and no refresh was attempted; call loadMint(true), or retry once the repair cooldown clears` : `Could not resolve unknown keyset '${t}': mint refresh failed`;
		super(i, n), this.keysetId = t, this.refreshed = r, this.name = "UnknownKeysetError", Object.setPrototypeOf(this, e.prototype);
	}
}, ne = class e extends S {
	constructor(t, n, r) {
		super("Melt completed but its change could not be reconstructed; recover the proofs with this error's outputData and createMeltChangeProofs()", r), this.outputData = t, this.quote = n, this.name = "MeltChangeError", Object.setPrototypeOf(this, e.prototype);
	}
}, re = class e extends S {
	constructor(t, n) {
		super(t ? "Mint rejected a stale keyset; the snapshot has been refreshed, re-prepare the operation" : "Mint rejected a stale keyset; refresh the snapshot (loadMint(true)) and re-prepare", n), this.repaired = t, this.name = "StaleKeysetError", Object.setPrototypeOf(this, e.prototype);
	}
}, ie = class e extends C {
	constructor(t, n) {
		super(t, 429), this.retryAfterMs = n, this.name = "RateLimitError", Object.setPrototypeOf(this, e.prototype);
	}
}, ae = class e extends C {
	constructor(t, n) {
		super(n || "Unknown mint operation error", 400), this.code = t, this.name = "MintOperationError", Object.setPrototypeOf(this, e.prototype);
	}
};
function oe(e) {
	return e instanceof ae || e instanceof Error && e.name === "MintOperationError" && "code" in e;
}
//#endregion
//#region src/logger/NullLogger.ts
var O = {
	error() {},
	warn() {},
	info() {},
	debug() {},
	trace() {},
	log() {}
};
//#endregion
//#region src/logger/helpers.ts
function se(e, t = O, n) {
	throw t.error(e, n), new S(e);
}
function k(e, t, n = O, r) {
	e && se(t, n, r);
}
function ce(e, t, n = O, r) {
	e ?? se(t, n, r);
}
function A(e, t, n = O, r) {
	if (e) try {
		let i = e(t);
		i && typeof i.then == "function" && i.catch((t) => {
			try {
				n.warn("callback failed", {
					...r ?? {},
					error: t,
					cb: e.name ?? ""
				});
			} catch {}
		});
	} catch (t) {
		try {
			n.warn("callback failed", {
				...r ?? {},
				error: t,
				cb: e.name ?? ""
			});
		} catch {}
	}
}
//#endregion
//#region src/utils/limits.ts
var le = 8192, ue = 1e4, de = 1024, fe = 1024, pe = 16384, me = 2147483647, he = 2n ** 64n - 1n, ge = 1048576, _e = {
	error: 0,
	warn: 1,
	info: 2,
	debug: 3,
	trace: 4
}, ve = {
	"\n": "\\n",
	"\r": "\\r",
	"	": "\\t",
	"\u2028": "\\u2028",
	"\u2029": "\\u2029"
}, ye = /[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g;
function be(e) {
	return String(e).replace(ye, (e) => ve[e] ?? "\\x" + e.charCodeAt(0).toString(16).padStart(2, "0"));
}
function xe(e, t) {
	if (typeof e == "string") return be(e);
	if (e instanceof Error) return {
		message: be(e.message),
		stack: e.stack === void 0 ? void 0 : be(e.stack)
	};
	if (t >= 8 || typeof e != "object" || !e) return e;
	if (Array.isArray(e)) return e.map((e) => xe(e, t + 1));
	let n = Object.getPrototypeOf(e);
	if (n !== Object.prototype && n !== null) return e;
	let r = {};
	for (let [n, i] of Object.entries(e)) Object.defineProperty(r, n, {
		value: xe(i, t + 1),
		writable: !0,
		enumerable: !0,
		configurable: !0
	});
	return r;
}
var Se = class {
	constructor(e = "info") {
		this.minLevel = e;
	}
	should(e) {
		return _e[e] <= _e[this.minLevel];
	}
	method(e) {
		switch (e) {
			case "error": return console.error;
			case "warn": return console.warn;
			case "info": return console.info;
			case "debug": return console.debug;
			case "trace": return console.trace;
			default: return console.log;
		}
	}
	header(e, t) {
		return `[${e.toUpperCase()}] ${be(t)}`;
	}
	flattenContext(e) {
		return e ? xe(e, 0) : void 0;
	}
	emit(e, t, n) {
		if (!this.should(e)) return;
		let r = this.header(e, t), i = this.flattenContext(n), a = this.method(e);
		i && Object.keys(i).length ? a(r, i) : a(r);
	}
	error(e, t) {
		this.emit("error", e, t);
	}
	warn(e, t) {
		this.emit("warn", e, t);
	}
	info(e, t) {
		this.emit("info", e, t);
	}
	debug(e, t) {
		this.emit("debug", e, t);
	}
	trace(e, t) {
		this.emit("trace", e, t);
	}
	log(e, t, n) {
		this.emit(e, t, n);
	}
};
function Ce() {
	let e = Date.now();
	return { elapsed: () => Date.now() - e };
}
//#endregion
//#region src/utils/bytes.ts
var we = new TextDecoder("utf-8", {
	ignoreBOM: !0,
	fatal: !0
});
function Te(e) {
	try {
		return we.decode(e);
	} catch (e) {
		throw new S("Malformed UTF-8 sequence", { cause: e });
	}
}
var Ee = new TextDecoder("utf-8", { fatal: !0 });
function De(e) {
	let t;
	try {
		t = Ee.decode(e);
	} catch (e) {
		throw new S("Malformed UTF-8 sequence", { cause: e });
	}
	return t.charCodeAt(0) === 65279 ? t.slice(1) : t;
}
var Oe = /[\uD800-\uDFFF]/u;
function ke(e) {
	return Oe.test(e);
}
//#endregion
//#region \0@oxc-project+runtime@0.127.0/helpers/checkPrivateRedeclaration.js
function Ae(e, t) {
	if (t.has(e)) throw TypeError("Cannot initialize the same private elements twice on an object");
}
//#endregion
//#region \0@oxc-project+runtime@0.127.0/helpers/classPrivateFieldInitSpec.js
function je(e, t, n) {
	Ae(e, t), t.set(e, n);
}
//#endregion
//#region \0@oxc-project+runtime@0.127.0/helpers/typeof.js
function Me(e) {
	"@babel/helpers - typeof";
	return Me = typeof Symbol == "function" && typeof Symbol.iterator == "symbol" ? function(e) {
		return typeof e;
	} : function(e) {
		return e && typeof Symbol == "function" && e.constructor === Symbol && e !== Symbol.prototype ? "symbol" : typeof e;
	}, Me(e);
}
//#endregion
//#region \0@oxc-project+runtime@0.127.0/helpers/checkInRHS.js
function Ne(e) {
	if (Object(e) !== e) throw TypeError("right-hand side of 'in' should be an object, got " + (e === null ? "null" : Me(e)));
	return e;
}
//#endregion
//#region src/model/Amount.ts
var Pe, Fe = !1, Ie = /* @__PURE__ */ new WeakMap();
Pe = Symbol.toPrimitive;
var j = class e {
	constructor(e) {
		if (je(this, Ie, !0), e < 0n) throw new D(`Amount must be >= 0, got ${e}`);
		if (e > he) throw new D(`Amount exceeds u64 max, got ${e}`);
		this.value = e, Object.freeze(this);
	}
	static from(t) {
		if (t instanceof e) {
			if (!Ie.has(Ne(t))) throw new D("Invalid Amount instance");
			return t;
		}
		if (typeof t == "bigint") {
			if (t < 0n) throw new D(`Amount must be >= 0, got ${t}`);
			return new e(t);
		}
		if (typeof t == "number") {
			if (!Number.isFinite(t) || !Number.isInteger(t)) throw new D(`Invalid number amount: ${t}`);
			if (t < 0) throw new D(`Amount must be >= 0, got ${t}`);
			if (!Number.isSafeInteger(t)) throw new D(`Unsafe integer amount: ${t}. Use bigint or decimal string.`);
			return new e(BigInt(t));
		}
		if (typeof t == "string") {
			if (t.length > 20) throw new D(`Amount exceeds u64 max: "${t.slice(0, 20)}..."`);
			if (!/^(0|[1-9]\d*)$/.test(t)) throw new D(`Invalid amount string "${t}". Expected non-negative decimal integer.`);
			return new e(BigInt(t));
		}
		if (typeof t == "object" && t && typeof t.value == "bigint") return new e(t.value);
		throw new D("Unsupported amount input type");
	}
	static zero() {
		return new e(0n);
	}
	static one() {
		return new e(1n);
	}
	toBigInt() {
		return this.value;
	}
	toNumber() {
		if (!this.isSafeNumber()) throw new D(`Amount ${this.value} exceeds Number.MAX_SAFE_INTEGER; use toBigInt/toString/toJSON.`);
		return Number(this.value);
	}
	toNumberUnsafe() {
		return Number(this.value);
	}
	toString() {
		return this.value.toString(10);
	}
	toJSON() {
		return this.toString();
	}
	[Pe](e) {
		return e === "string" ? this.toString() : (Fe || (Fe = !0, console.warn("Implicit numeric coercion of Amount is deprecated and throws in cashu-ts v5; use .add()/.subtract()/.compareTo(), .toBigInt() or .toNumber(), or .toString() for display.")), this.toNumberUnsafe());
	}
	add(t) {
		let n = e.from(t);
		return new e(this.value + n.value);
	}
	subtract(t) {
		let n = e.from(t), r = this.value - n.value;
		if (r < 0n) throw new D(`Amount underflow: ${this.value} - ${n.value} would be negative`);
		return new e(r);
	}
	multiplyBy(t) {
		let n = e.from(t).value;
		return new e(this.value * n);
	}
	divideBy(t) {
		let n = e.from(t).value;
		if (n <= 0n) throw new D(`Divisor must be > 0, got ${n}`);
		return new e(this.value / n);
	}
	modulo(t) {
		let n = e.from(t).value;
		if (n <= 0n) throw new D(`Divisor must be > 0, got ${n}`);
		return new e(this.value % n);
	}
	ceilPercent(t, n = 100) {
		if (!Number.isSafeInteger(t) || t < 0) throw new D(`ceilPercent: numerator must be a non-negative integer, got ${t}`);
		if (!Number.isSafeInteger(n) || n <= 0) throw new D(`ceilPercent: denominator must be a positive integer, got ${n}`);
		let r = BigInt(t), i = BigInt(n);
		return new e((this.value * r + (i - 1n)) / i);
	}
	floorPercent(t, n = 100) {
		if (!Number.isSafeInteger(t) || t < 0) throw new D(`floorPercent: numerator must be a non-negative integer, got ${t}`);
		if (!Number.isSafeInteger(n) || n <= 0) throw new D(`floorPercent: denominator must be a positive integer, got ${n}`);
		let r = BigInt(t), i = BigInt(n);
		return new e(this.value * r / i);
	}
	inRange(t, n) {
		let r = e.from(t), i = e.from(n);
		if (r.greaterThan(i)) throw new D(`inRange: min (${r.toString()}) must be <= max (${i.toString()})`);
		return this.greaterThanOrEqual(r) && this.lessThanOrEqual(i);
	}
	clamp(t, n) {
		let r = e.from(t), i = e.from(n);
		if (r.greaterThan(i)) throw new D(`clamp: min (${r.toString()}) must be <= max (${i.toString()})`);
		return e.max(r, e.min(i, this));
	}
	scaledBy(t, n) {
		let r = e.from(t).value, i = e.from(n).value;
		if (i === 0n) throw new D("scaledBy: denominator must be > 0");
		return r === 0n ? e.zero() : new e((2n * this.value * r + i) / (2n * i));
	}
	isSafeNumber() {
		let e = BigInt(2 ** 53 - 1);
		return this.value <= e;
	}
	isZero() {
		return this.value === 0n;
	}
	equals(t) {
		return this.value === e.from(t).value;
	}
	compareTo(t) {
		let n = e.from(t).value;
		return this.value < n ? -1 : +(this.value > n);
	}
	lessThan(e) {
		return this.compareTo(e) < 0;
	}
	lessThanOrEqual(e) {
		return this.compareTo(e) <= 0;
	}
	greaterThan(e) {
		return this.compareTo(e) > 0;
	}
	greaterThanOrEqual(e) {
		return this.compareTo(e) >= 0;
	}
	static min(t, n) {
		let r = e.from(t), i = e.from(n);
		return r.compareTo(i) <= 0 ? r : i;
	}
	static max(t, n) {
		let r = e.from(t), i = e.from(n);
		return r.compareTo(i) >= 0 ? r : i;
	}
	static sum(t) {
		let n = 0n;
		for (let r of t) n += e.from(r).value;
		return new e(n);
	}
	withUnit(e) {
		return new Le(this, e);
	}
}, Le = class e {
	constructor(e, t) {
		if (typeof t != "string" || t.length === 0) throw new ee("unit required");
		this._amount = e, this.unit = t, Object.freeze(this);
	}
	static from(t, n) {
		return new e(j.from(t), n);
	}
	static zero(t) {
		return new e(j.zero(), t);
	}
	static one(t) {
		return new e(j.one(), t);
	}
	toAmount() {
		return this._amount;
	}
	toBigInt() {
		return this._amount.toBigInt();
	}
	toNumber() {
		return this._amount.toNumber();
	}
	toString() {
		return `[${this.unit}]: ${this._amount.toString()}`;
	}
	toJSON() {
		return {
			amount: this._amount.toString(),
			unit: this.unit
		};
	}
	[Symbol.toPrimitive](e) {
		if (e === "string") return this.toString();
		throw new ee(`Implicit ${e === "number" ? "numeric" : "default"} coercion of AmountWithUnit is unsafe; use .toAmount() then explicit arithmetic, or .toString() for display.`);
	}
	isZero() {
		return this._amount.isZero();
	}
	isSafeNumber() {
		return this._amount.isSafeNumber();
	}
	requireSameUnit(e) {
		if (this.unit !== e.unit) throw new ee(`unit mismatch: ${this.unit} vs ${e.unit}`);
	}
	add(t) {
		return this.requireSameUnit(t), new e(this._amount.add(t._amount), this.unit);
	}
	subtract(t) {
		return this.requireSameUnit(t), new e(this._amount.subtract(t._amount), this.unit);
	}
	equals(e) {
		return this.requireSameUnit(e), this._amount.equals(e._amount);
	}
	compareTo(e) {
		return this.requireSameUnit(e), this._amount.compareTo(e._amount);
	}
	lessThan(e) {
		return this.compareTo(e) < 0;
	}
	lessThanOrEqual(e) {
		return this.compareTo(e) <= 0;
	}
	greaterThan(e) {
		return this.compareTo(e) > 0;
	}
	greaterThanOrEqual(e) {
		return this.compareTo(e) >= 0;
	}
	inRange(e, t) {
		return this.requireSameUnit(e), this.requireSameUnit(t), this._amount.inRange(e._amount, t._amount);
	}
	clamp(t, n) {
		return this.requireSameUnit(t), this.requireSameUnit(n), new e(this._amount.clamp(t._amount, n._amount), this.unit);
	}
	multiplyBy(t) {
		return new e(this._amount.multiplyBy(t), this.unit);
	}
	divideBy(t) {
		return new e(this._amount.divideBy(t), this.unit);
	}
	modulo(t) {
		return new e(this._amount.modulo(t), this.unit);
	}
	ceilPercent(t, n) {
		return new e(this._amount.ceilPercent(t, n), this.unit);
	}
	floorPercent(t, n) {
		return new e(this._amount.floorPercent(t, n), this.unit);
	}
	scaledBy(t, n) {
		return new e(this._amount.scaledBy(t, n), this.unit);
	}
	static min(e, t) {
		return e.requireSameUnit(t), e.compareTo(t) <= 0 ? e : t;
	}
	static max(e, t) {
		return e.requireSameUnit(t), e.compareTo(t) >= 0 ? e : t;
	}
	static sum(t, n) {
		let r = n, i = 0n, a = !1;
		for (let e of t) {
			if (r === void 0) r = e.unit;
			else if (e.unit !== r) throw new ee(`unit mismatch: ${r} vs ${e.unit}`);
			i += e._amount.toBigInt(), a = !0;
		}
		if (r === void 0) throw new ee("cannot infer unit from empty sum");
		return new e(a ? j.from(i) : j.zero(), r);
	}
}, M = Object.freeze({
	parse: Ke,
	stringify: Xe
}), Re = 64, ze;
function Be(e) {
	return typeof e == "object" && !!e && !Array.isArray(e);
}
function Ve() {
	let e = globalThis.BigInt;
	return typeof e == "function" ? e : void 0;
}
function He(e) {
	if (!ze) {
		let t = e(String(2 ** 53 - 1));
		ze = {
			max: t,
			min: -t
		};
	}
	return ze;
}
var Ue = class {
	constructor(e, t, n, r) {
		this.src = e, this.strict = t, this.fallbackTo = n, this.bigIntCtor = r, this.i = 0;
	}
	parse() {
		let e = this.parseValue(0);
		if (this.skipWhitespace(), !this.isEnd()) throw this.syntaxError("Unexpected trailing input");
		return e;
	}
	parseValue(e) {
		if (e > Re) throw this.syntaxError("JSON nesting exceeds the maximum depth");
		this.skipWhitespace();
		let t = this.peek();
		if (t === "{") return this.parseObject(e);
		if (t === "[") return this.parseArray(e);
		if (t === "\"") return this.parseString();
		if (t === "-" || this.isDigit(t)) return this.parseNumber();
		if (t === "t") return this.parseLiteral("true", !0);
		if (t === "f") return this.parseLiteral("false", !1);
		if (t === "n") return this.parseLiteral("null", null);
		throw this.syntaxError(`Unexpected token '${t || "EOF"}'`);
	}
	parseObject(e) {
		this.expect("{"), this.skipWhitespace();
		let t = {}, n = /* @__PURE__ */ new Set();
		if (this.peek() === "}") return this.expect("}"), t;
		for (; !this.isEnd();) {
			let r = this.parseString();
			if (this.strict && n.has(r)) throw this.syntaxError(`Duplicate key "${r}"`);
			if (n.add(r), this.skipWhitespace(), this.expect(":"), Object.defineProperty(t, r, {
				value: this.parseValue(e + 1),
				writable: !0,
				enumerable: !0,
				configurable: !0
			}), this.skipWhitespace(), this.peek() === "}") return this.expect("}"), t;
			this.expect(","), this.skipWhitespace();
		}
		throw this.syntaxError("Unterminated object");
	}
	parseArray(e) {
		this.expect("["), this.skipWhitespace();
		let t = [];
		if (this.peek() === "]") return this.expect("]"), t;
		for (; !this.isEnd();) {
			if (t.push(this.parseValue(e + 1)), this.skipWhitespace(), this.peek() === "]") return this.expect("]"), t;
			this.expect(","), this.skipWhitespace();
		}
		throw this.syntaxError("Unterminated array");
	}
	parseString() {
		this.expect("\"");
		let e = "";
		for (; !this.isEnd();) {
			let t = this.next();
			if (t === "\"") return e;
			if (t === "\\") {
				let t = this.next();
				switch (t) {
					case "\"":
					case "\\":
					case "/":
						e += t;
						break;
					case "b":
						e += "\b";
						break;
					case "f":
						e += "\f";
						break;
					case "n":
						e += "\n";
						break;
					case "r":
						e += "\r";
						break;
					case "t":
						e += "	";
						break;
					case "u": {
						let t = this.src.slice(this.i, this.i + 4);
						if (!/^[0-9a-fA-F]{4}$/.test(t)) throw this.syntaxError("Invalid unicode escape");
						this.i += 4, e += String.fromCharCode(parseInt(t, 16));
						break;
					}
					default: throw this.syntaxError(`Invalid escape '\\${t}'`);
				}
				continue;
			}
			if (t < " ") throw this.syntaxError("Invalid control character in string");
			e += t;
		}
		throw this.syntaxError("Unterminated string");
	}
	parseNumber() {
		let e = this.i;
		this.peek() === "-" && (this.i += 1), this.peek() === "0" ? this.i += 1 : this.readDigits(), this.peek() === "." && (this.i += 1, this.readDigits());
		let t = this.peek();
		if (t === "e" || t === "E") {
			this.i += 1;
			let e = this.peek();
			(e === "+" || e === "-") && (this.i += 1), this.readDigits();
		}
		let n = this.src.slice(e, this.i);
		if (!(n.indexOf(".") === -1 && n.indexOf("e") === -1 && n.indexOf("E") === -1)) {
			let e = Number(n);
			if (!Number.isFinite(e)) throw this.syntaxError("Bad number");
			return e;
		}
		if (!this.bigIntCtor) switch (this.fallbackTo) {
			case "number": {
				let e = Number(n);
				if (!Number.isFinite(e)) throw this.syntaxError("Bad number");
				return e;
			}
			case "string": return n;
			case "error": throw new S("BigInt is not available in this runtime");
		}
		let r = this.bigIntCtor(n), { max: i, min: a } = He(this.bigIntCtor);
		return r > i || r < a ? r : Number(n);
	}
	parseLiteral(e, t) {
		if (this.src.slice(this.i, this.i + e.length) !== e) throw this.syntaxError(`Unexpected token near '${this.src.slice(this.i, this.i + 8)}'`);
		return this.i += e.length, t;
	}
	readDigits() {
		let e = this.i;
		for (; this.isDigit(this.peek());) this.i += 1;
		if (this.i === e) throw this.syntaxError("Bad number");
	}
	skipWhitespace() {
		for (; !this.isEnd();) {
			let e = this.peek();
			if (e === " " || e === "\n" || e === "\r" || e === "	") {
				this.i += 1;
				continue;
			}
			break;
		}
	}
	expect(e) {
		if (this.next() !== e) throw this.syntaxError(`Expected '${e}'`);
	}
	peek() {
		return this.src.charAt(this.i);
	}
	next() {
		let e = this.src.charAt(this.i);
		return this.i += 1, e;
	}
	isDigit(e) {
		return e >= "0" && e <= "9";
	}
	isEnd() {
		return this.i >= this.src.length;
	}
	syntaxError(e) {
		return /* @__PURE__ */ SyntaxError(`${e} at position ${this.i}`);
	}
};
function We(e, t, n) {
	Reflect.defineProperty(e, t, {
		value: n,
		writable: !0,
		enumerable: !0,
		configurable: !0
	});
}
function Ge(e, t, n) {
	let r = Array.isArray(e) ? e[Number(t)] : e[t];
	if (Array.isArray(r)) for (let e = 0; e < r.length; e += 1) {
		let t = Ge(r, String(e), n);
		t === void 0 ? Reflect.deleteProperty(r, e) : We(r, String(e), t);
	}
	else if (Be(r)) for (let e of Object.keys(r)) {
		let t = Ge(r, e, n);
		t === void 0 ? delete r[e] : We(r, e, t);
	}
	return n.call(e, t, r);
}
function Ke(e, t, n) {
	let r = n?.strict === !0, i = n?.fallbackTo ?? "number";
	if (i !== "number" && i !== "string" && i !== "error") throw new S(`Incorrect value for fallbackTo option, must be "number", "string", "error" or undefined but passed ${String(n?.fallbackTo)}`);
	let a = new Ue(String(e), r, i, Ve()).parse();
	return typeof t == "function" ? Ge({ "": a }, "", t) : a;
}
function qe(e) {
	let t = JSON.stringify(e);
	if (typeof t != "string") throw new S("Failed to stringify string value");
	return t;
}
function Je(e) {
	return typeof e == "object" && !!e && "toJSON" in e && typeof e.toJSON == "function";
}
function Ye(e) {
	return e instanceof Number || e instanceof String || e instanceof Boolean ? e.valueOf() : e;
}
function Xe(e, t, n) {
	let r = "", i = "", a = /* @__PURE__ */ new WeakSet();
	if (typeof n == "number" ? i = " ".repeat(Math.min(10, Math.max(0, Math.floor(n)))) : typeof n == "string" && (i = n.slice(0, 10)), t && typeof t != "function" && !Array.isArray(t)) throw new S("stringify: replacer must be a function or array");
	let o = Array.isArray(t) ? Array.from(new Set(t.filter((e) => typeof e == "string" || typeof e == "number").map((e) => String(e)))) : void 0, s = (e, n) => {
		let c = e[n];
		switch (c instanceof j ? c = c.toBigInt() : Je(c) && (c = c.toJSON(n)), typeof t == "function" && (c = t.call(e, n, c)), c = Ye(c), typeof c) {
			case "string": return qe(c);
			case "number": return Number.isFinite(c) ? String(c) : "null";
			case "boolean": return c ? "true" : "false";
			case "bigint": return String(c);
			case "undefined": return;
			case "object": {
				if (c === null) return "null";
				if (a.has(c)) throw TypeError("Converting circular structure to JSON");
				a.add(c);
				let e = r;
				r += i;
				try {
					if (Array.isArray(c)) {
						let t = [], n = c;
						for (let e = 0; e < c.length; e += 1) {
							let r = s(n, String(e));
							t.push(r ?? "null");
						}
						let i = t.length === 0 ? "[]" : r ? `[\n${r}${t.join(`,\n${r}`)}\n${e}]` : `[${t.join(",")}]`;
						return r = e, i;
					}
					let t = c, n = o ?? Object.keys(t), i = [];
					for (let e of n) {
						let n = s(t, e);
						n !== void 0 && i.push(`${qe(e)}${r ? ": " : ":"}${n}`);
					}
					let a = i.length === 0 ? "{}" : r ? `{\n${r}${i.join(`,\n${r}`)}\n${e}}` : `{${i.join(",")}}`;
					return r = e, a;
				} finally {
					a.delete(c);
				}
			}
			default: return;
		}
	};
	return s({ "": e }, "");
}
//#endregion
//#region src/utils/base64.ts
function Ze(e) {
	return e.trim().replace(/[\t\n\f\r ]+/g, "").replace(/={1,2}$/, "");
}
function Qe(e) {
	return s.encode(e);
}
function $e(e) {
	return u.encode(e);
}
function et(e) {
	return l.encode(e);
}
function tt(e) {
	try {
		return u.decode(Ze(e));
	} catch (e) {
		throw new S("Invalid base64url string", { cause: e });
	}
}
function nt(e) {
	try {
		return c.decode(Ze(e));
	} catch (e) {
		throw new S("Invalid base64 string", { cause: e });
	}
}
function rt(e) {
	try {
		return tt(e);
	} catch (t) {
		try {
			return nt(e);
		} catch {
			throw t;
		}
	}
}
function it(e) {
	let t = De(rt(e));
	return M.parse(t, void 0, { strict: !0 });
}
function at(e) {
	if (typeof e != "string" || e.length === 0) return !1;
	let t = Ze(e);
	if (t.length === 0) return !1;
	let n = /[-_]/.test(t) ? u : c;
	try {
		return n.encode(n.decode(t)) === t;
	} catch {
		return !1;
	}
}
//#endregion
//#region src/utils/bech32m.ts
var ot = 1023;
function st(e) {
	let t = e.lastIndexOf("1");
	if (t < 1 || t === e.length - 1) throw new S("Invalid bech32m string: missing or misplaced separator");
}
function ct(e, t, n = ot) {
	let r = f.toWords(t);
	return f.encode(e, r, n);
}
function lt(e, t = ot) {
	st(e);
	let { prefix: n, words: r } = f.decode(e, t);
	return {
		hrp: n,
		data: f.fromWords(r)
	};
}
function ut(e, t = ot) {
	return lt(e, t).data;
}
//#endregion
//#region src/utils/cbor.ts
function dt(e) {
	return typeof e == "number" || typeof e == "bigint" || typeof e == "string";
}
function ft(e) {
	let t = [];
	return pt(e, t), new Uint8Array(t);
}
function pt(e, t) {
	if (e === null) t.push(246);
	else if (e === void 0) t.push(247);
	else if (typeof e == "boolean") t.push(e ? 245 : 244);
	else if (typeof e == "number") yt(e, t);
	else if (typeof e == "bigint") ht(e, t);
	else if (typeof e == "string") xt(e, t);
	else if (Array.isArray(e)) St(e, t);
	else if (e instanceof Uint8Array) bt(e, t);
	else if (typeof e == "object" && e && !Array.isArray(e)) Ct(e, t);
	else throw new S("Unsupported type");
}
function mt(e, t) {
	e < 24 ? t.push(e) : e < 256 ? t.push(24, e) : e < 65536 ? t.push(25, e >>> 8 & 255, e & 255) : e < 4294967296 ? t.push(26, e >>> 24 & 255, e >>> 16 & 255, e >>> 8 & 255, e & 255) : ht(BigInt(e), t);
}
function ht(e, t) {
	e >= 0n ? gt(0, e, t) : gt(1, -1n - e, t);
}
function gt(e, t, n) {
	let r = e << 5;
	if (t < 24n) n.push(r | Number(t));
	else if (t < 256n) n.push(r | 24, Number(t));
	else if (t < 65536n) {
		let e = Number(t);
		n.push(r | 25, e >>> 8 & 255, e & 255);
	} else if (t < 4294967296n) {
		let e = Number(t);
		n.push(r | 26, e >>> 24 & 255, e >>> 16 & 255, e >>> 8 & 255, e & 255);
	} else if (t < 18446744073709551616n) {
		let e = Number(t >> 32n), i = Number(t & 4294967295n);
		n.push(r | 27, e >>> 24 & 255, e >>> 16 & 255, e >>> 8 & 255, e & 255, i >>> 24 & 255, i >>> 16 & 255, i >>> 8 & 255, i & 255);
	} else throw new S("BigInt value out of uint64 range");
}
function _t(e, t) {
	let n = -1 - e;
	n < 24 ? t.push(32 | n) : n < 256 ? t.push(56, n & 255) : n < 65536 ? t.push(57, n >>> 8 & 255, n & 255) : n < 4294967296 ? t.push(58, n >>> 24 & 255, n >>> 16 & 255, n >>> 8 & 255, n & 255) : ht(BigInt(e), t);
}
function vt(e, t) {
	let n = /* @__PURE__ */ new DataView(/* @__PURE__ */ new ArrayBuffer(8));
	n.setFloat64(0, e, !1), t.push(251);
	for (let e = 0; e < 8; e++) t.push(n.getUint8(e));
}
function yt(e, t) {
	Number.isInteger(e) ? e >= 0 ? mt(e, t) : _t(e, t) : vt(e, t);
}
function bt(e, t) {
	let n = e.length;
	if (n < 24) t.push(64 + n);
	else if (n < 256) t.push(88, n);
	else if (n < 65536) t.push(89, n >> 8 & 255, n & 255);
	else if (n < 4294967296) t.push(90, n >>> 24 & 255, n >>> 16 & 255, n >>> 8 & 255, n & 255);
	else throw new S("Byte string too long to encode");
	for (let n = 0; n < e.length; n++) t.push(e[n]);
}
function xt(e, t) {
	if (ke(e)) throw new S("CBOR text must be well-formed UTF-16");
	let n = new TextEncoder().encode(e), r = n.length;
	if (r < 24) t.push(96 + r);
	else if (r < 256) t.push(120, r);
	else if (r < 65536) t.push(121, r >>> 8 & 255, r & 255);
	else if (r < 4294967296) t.push(122, r >>> 24 & 255, r >>> 16 & 255, r >>> 8 & 255, r & 255);
	else throw new S("String too long to encode");
	for (let e = 0; e < n.length; e++) t.push(n[e]);
}
function St(e, t) {
	let n = e.length;
	if (n < 24) t.push(128 | n);
	else if (n < 256) t.push(152, n);
	else if (n < 65536) t.push(153, n >>> 8 & 255, n & 255);
	else throw new S("Unsupported array length");
	for (let n of e) pt(n, t);
}
function Ct(e, t) {
	let n = Object.keys(e), r = n.length;
	if (r >= 4294967296) throw new S("Object has too many keys to encode");
	r < 24 ? t.push(160 | r) : r < 256 ? t.push(184, r) : r < 65536 ? t.push(185, r >> 8 & 255, r & 255) : t.push(186, r >> 24 & 255, r >> 16 & 255, r >> 8 & 255, r & 255);
	for (let r of n) xt(r, t), pt(e[r], t);
}
function wt(e) {
	return Et(new DataView(e.buffer, e.byteOffset, e.byteLength), 0).value;
}
var Tt = 64;
function Et(e, t, n = 0, r = { nodes: 0 }) {
	if (n > Tt) throw new S("CBOR nesting exceeds the maximum depth");
	if (t >= e.byteLength) throw new S("Unexpected end of data");
	let i = e.getUint8(t++), a = i >> 5, o = i & 31;
	switch (a) {
		case 0: return Ot(e, t, o);
		case 1: return kt(e, t, o);
		case 2: return At(e, t, o);
		case 3: return jt(e, t, o);
		case 4: return Nt(e, t, o, n, r);
		case 5: return Pt(e, t, o, n, r);
		case 7: return It(e, t, o);
		default: throw new S(`Unsupported major type: ${a}`);
	}
}
function N(e, t, n) {
	if (t + n > e.byteLength) throw new S("Unexpected end of data");
}
function Dt(e, t, n) {
	if (n < 24) return {
		value: n,
		offset: t
	};
	if (n === 24) return N(e, t, 1), {
		value: e.getUint8(t++),
		offset: t
	};
	if (n === 25) {
		N(e, t, 2);
		let n = e.getUint16(t, !1);
		return t += 2, {
			value: n,
			offset: t
		};
	}
	if (n === 26) {
		N(e, t, 4);
		let n = e.getUint32(t, !1);
		return t += 4, {
			value: n,
			offset: t
		};
	}
	if (n === 27) {
		N(e, t, 8);
		let n = e.getUint32(t, !1), r = e.getUint32(t + 4, !1);
		t += 8;
		let i = n * 2 ** 32 + r;
		return i > 2 ** 53 - 1 ? {
			value: BigInt(n) << 32n | BigInt(r),
			offset: t
		} : {
			value: i,
			offset: t
		};
	}
	throw new S(`Unsupported length: ${n}`);
}
function Ot(e, t, n) {
	let { value: r, offset: i } = Dt(e, t, n);
	return {
		value: r,
		offset: i
	};
}
function kt(e, t, n) {
	let { value: r, offset: i } = Dt(e, t, n);
	if (typeof r == "bigint") return {
		value: -1n - r,
		offset: i
	};
	let a = -1 - r;
	return Number.isSafeInteger(a) ? {
		value: a,
		offset: i
	} : {
		value: -1n - BigInt(r),
		offset: i
	};
}
function At(e, t, n) {
	let { value: r, offset: i } = Dt(e, t, n), a = Number(r);
	if (i + a > e.byteLength) throw new S("Byte string length exceeds data length");
	return {
		value: new Uint8Array(e.buffer, e.byteOffset + i, a),
		offset: i + a
	};
}
function jt(e, t, n) {
	let { value: r, offset: i } = Dt(e, t, n), a = Number(r);
	if (i + a > e.byteLength) throw new S("String length exceeds data length");
	return {
		value: Te(new Uint8Array(e.buffer, e.byteOffset + i, a)),
		offset: i + a
	};
}
function Mt(e) {
	if (e.nodes++, e.nodes > 262144) throw new S("CBOR payload exceeds the maximum item/entry budget");
}
function Nt(e, t, n, r, i) {
	let { value: a, offset: o } = Dt(e, t, n), s = Number(a), c = [], l = o;
	for (let t = 0; t < s; t++) {
		Mt(i);
		let t = Et(e, l, r + 1, i);
		c.push(t.value), l = t.offset;
	}
	return {
		value: c,
		offset: l
	};
}
function Pt(e, t, n, r, i) {
	let { value: a, offset: o } = Dt(e, t, n), s = Number(a), c = {}, l = o;
	for (let t = 0; t < s; t++) {
		Mt(i);
		let t = Et(e, l, r + 1, i);
		if (!dt(t.value)) throw new S("Invalid key type");
		let n = String(t.value);
		if (Object.prototype.hasOwnProperty.call(c, n)) throw new S(`Duplicate map key "${n}"`);
		let a = Et(e, t.offset, r + 1, i);
		Object.defineProperty(c, n, {
			value: a.value,
			writable: !0,
			enumerable: !0,
			configurable: !0
		}), l = a.offset;
	}
	return {
		value: c,
		offset: l
	};
}
function Ft(e) {
	let t = (e & 31744) >> 10, n = e & 1023, r = e & 32768 ? -1 : 1;
	return t === 0 ? r * 2 ** -14 * (n / 1024) : t === 31 ? n ? NaN : r * Infinity : r * 2 ** (t - 15) * (1 + n / 1024);
}
function It(e, t, n) {
	if (n < 24) switch (n) {
		case 20: return {
			value: !1,
			offset: t
		};
		case 21: return {
			value: !0,
			offset: t
		};
		case 22: return {
			value: null,
			offset: t
		};
		case 23: return {
			value: void 0,
			offset: t
		};
		default: throw new S(`Unknown simple value: ${n}`);
	}
	if (n === 24) {
		N(e, t, 1);
		let n = e.getUint8(t++);
		if (n < 32) throw new S(`Invalid extended simple value: ${n}`);
		return {
			value: n,
			offset: t
		};
	}
	if (n === 25) {
		N(e, t, 2);
		let n = Ft(e.getUint16(t, !1));
		return t += 2, {
			value: n,
			offset: t
		};
	}
	if (n === 26) {
		N(e, t, 4);
		let n = e.getFloat32(t, !1);
		return t += 4, {
			value: n,
			offset: t
		};
	}
	if (n === 27) {
		N(e, t, 8);
		let n = e.getFloat64(t, !1);
		return t += 8, {
			value: n,
			offset: t
		};
	}
	throw new S(`Unknown simple or float value: ${n}`);
}
//#endregion
//#region src/crypto/bip39.ts
var Lt = [
	12,
	15,
	18,
	21,
	24
];
function Rt(e, n = "") {
	if (typeof e != "string" || typeof n != "string") throw new S("mnemonicToSeedSync: mnemonic and passphrase must be strings");
	let r = e.normalize("NFKD");
	if (!Lt.includes(r.split(" ").length)) throw new S("mnemonicToSeedSync: a mnemonic has 12, 15, 18, 21 or 24 words");
	return p(t, r, ("mnemonic" + n).normalize("NFKD"), {
		c: 2048,
		dkLen: 64
	});
}
//#endregion
//#region src/crypto/core.ts
var zt = o("Secp256k1_HashToCurve_Cashu_");
function Bt(t) {
	let i = e(r(zt, t)), a = 0, o = new Uint8Array(4), s = new DataView(o.buffer), c = 2 ** 16;
	for (let t = 0; t < c; t++) {
		s.setUint32(0, a, !0);
		let t = e(r(i, o));
		try {
			return P(n(r(new Uint8Array([2]), t)));
		} catch {
			a++;
		}
	}
	throw new S("No valid point found");
}
function Vt(t) {
	let n = t.map((e) => e.toHex(!1)).join("");
	return e(new TextEncoder().encode(n));
}
function Ht(e) {
	return h.Point.fromHex(n(e));
}
function P(e) {
	return h.Point.fromHex(e);
}
function Ut(e) {
	try {
		return P(e.toHex(!0));
	} catch (e) {
		throw new S("Invalid point: not a valid secp256k1 point", { cause: e });
	}
}
var Wt = 12;
function Gt(e) {
	return e.length === 12 && at(e);
}
var Kt = (e) => {
	let t, n = (e) => g(rt(e)) % BigInt(2 ** 31 - 1);
	if (e.length === 12 && at(e)) t = n(e);
	else if (U(e)) t = Vi(e) % BigInt(2 ** 31 - 1);
	else if (at(e)) t = n(e);
	else throw new S("Invalid keyset id: neither hex nor base64");
	return t;
};
function qt() {
	return h.utils.randomSecretKey();
}
function Jt(e, t, n) {
	let r = h.Point.Fn.fromBytes(t);
	return {
		C_: Ut(e).multiply(r),
		id: n
	};
}
function Yt() {
	let e = n(a(32));
	return Xt(new TextEncoder().encode(e));
}
function Xt(e, t) {
	let n = Bt(e);
	if (t === void 0) t = h.Point.Fn.fromBytes(qt());
	else if (t === 0n) throw new S("Blinding factor r must be non-zero");
	let r = h.Point.BASE.multiply(t);
	return {
		B_: n.add(r),
		r: t,
		secret: e
	};
}
function Zt(e, t, n) {
	return e.subtract(n.multiply(t));
}
function Qt(e, t, n, r) {
	let i = Zt(e.C_, t, r);
	return {
		id: e.id,
		secret: n,
		C: i
	};
}
function $t(t, r = !1) {
	if (ke(t)) throw new S("Message must be well-formed UTF-16");
	let i = e(new TextEncoder().encode(t));
	return r ? n(i) : i;
}
var en = (e, t) => {
	let r = typeof e == "string" ? i(e) : e, a = typeof t == "string" ? i(t) : t;
	return n(m.sign(r, a));
}, tn = (e, t) => en($t(e), t), nn = (e, t, n, r = !1) => {
	try {
		return rn(e, $t(t), n, r);
	} catch (e) {
		if (r) throw e;
	}
	return !1;
}, rn = (e, t, n, r = !1) => {
	try {
		let r = typeof t == "string" ? i(t) : t;
		return m.verify(i(e), r, i(an(n)));
	} catch (e) {
		if (r) throw e;
	}
	return !1;
};
function an(e) {
	if (typeof e != "string") return e;
	let t = e.toLowerCase();
	return t.length === 66 && (t.startsWith("02") || t.startsWith("03")) ? t.slice(2) : t;
}
function on(e, t) {
	let r = Array.isArray(t) ? t : [t], a = e.toLowerCase();
	for (let e of r) {
		let t = n(h.getPublicKey(i(e), !0));
		if (t === a) return e;
		if (t.slice(2) === a.slice(2)) {
			let t = h.Point.Fn.fromBytes(i(e));
			return n(h.Point.Fn.toBytes(h.Point.Fn.neg(t)));
		}
	}
	throw new S(`No private key matches quote pubkey ${e}`);
}
function sn(e, t, n) {
	let r = /* @__PURE__ */ new Map();
	for (let e of n) {
		let t = an(e);
		r.has(t) || r.set(t, e);
	}
	return Array.from(r.values()).filter((n) => e.some((e) => nn(e, t, n)));
}
var cn = (e, t, n, r = 1) => !Number.isInteger(r) || r < 1 ? !1 : sn(e, t, n).length >= r, ln = "m/0'/0'/0'";
function un(e) {
	let t = {};
	return Object.keys(e).forEach((r) => {
		t[r] = n(e[r]);
	}), t;
}
function dn(e) {
	let t = {};
	return Object.keys(e).forEach((n) => {
		t[n] = i(e[n]);
	}), t;
}
function fn(e) {
	return h.getPublicKey(e, !0);
}
function pn(e, t, n) {
	if (!Number.isInteger(e) || e < 0 || e > 64) throw new S("createNewMintKeys: pow2height must be an integer in [0, 64]");
	let { expiry: r, input_fee_ppk: i, unit: a = "sat", versionByte: o = 1 } = n || {}, s = 0n, c = {}, l = {}, u;
	for (t && (u = b.fromMasterSeed(t)); s < e;) {
		let e = (2n ** s).toString();
		if (u) {
			let t = u.derive(`${ln}/${s}`).privateKey;
			if (t) l[e] = t;
			else throw new S(`Could not derive Private key from: ${ln}/${s}`);
		} else l[e] = qt();
		c[e] = fn(l[e]), s++;
	}
	return {
		pubKeys: c,
		privKeys: l,
		keysetId: oa(un(c), {
			expiry: r,
			input_fee_ppk: i,
			unit: a,
			versionByte: o
		})
	};
}
function mn(e, t) {
	let n = Bt(e.secret), r = h.Point.Fn.fromBytes(t);
	return n.multiply(r).equals(e.C);
}
//#endregion
//#region src/crypto/NUT10.ts
function hn(e, t, r) {
	let i = [e, {
		nonce: n(a(32)),
		data: t,
		tags: r
	}];
	return JSON.stringify(i);
}
function F(e) {
	let t;
	if (typeof e == "string" && ke(e)) throw new S("Invalid NUT-10 secret Unicode");
	try {
		t = typeof e == "string" ? JSON.parse(e) : e;
	} catch (e) {
		throw new S("Can't parse secret", { cause: e });
	}
	if (!Array.isArray(t) || t.length !== 2 || typeof t[0] != "string" || typeof t[1] != "object" || t[0].trim().length === 0 || t[1] === null) throw new S("Invalid NUT-10 secret");
	let [n, r] = t;
	if (typeof r.nonce != "string" || typeof r.data != "string") throw new S("Invalid NUT-10 secret nonce / data");
	if (r.tags != null) {
		if (!Array.isArray(r.tags)) throw new S("Invalid NUT-10 secret tags");
		if (r.tags.some((e) => !Array.isArray(e) || e.length === 0 || e.some((e) => typeof e != "string" || !e.length))) throw new S("Invalid NUT-10 tag(s)");
	}
	return [n, {
		nonce: r.nonce,
		data: r.data,
		tags: r.tags ?? void 0
	}];
}
function gn(e, t) {
	let n = Array.isArray(e) ? e : [e], r = F(t), i = r[0];
	if (!n.includes(i)) throw new S(`Invalid secret kind: ${i} Allowed: ${n.join(", ")}`);
	return r;
}
function _n(e) {
	return F(e)[0];
}
function vn(e) {
	return F(e)[1];
}
function yn(e) {
	let { data: t } = vn(e);
	return t;
}
function bn(e) {
	let { tags: t } = vn(e);
	return t ?? [];
}
function xn(e, t) {
	return bn(e).some((e) => e[0] === t);
}
function Sn(e, t) {
	let n = bn(e).find((e) => e[0] === t);
	if (!(!n || n.length <= 1)) return n.slice(1);
}
function Cn(e, t) {
	let n = Sn(e, t);
	if (n !== void 0) {
		if (n.length !== 1) throw new S(`Invalid NUT-10 tag "${t}": must carry a single value`);
		return n[0];
	}
}
function I(e, t) {
	let n = Cn(e, t);
	if (n === void 0) {
		if (!xn(e, t)) return;
		throw new S(`Invalid NUT-10 tag "${t}": must carry a single value`);
	}
	let r = /^-?\d+$/.test(n) ? Number(n) : NaN;
	if (!Number.isSafeInteger(r)) throw new S(`Invalid NUT-10 tag "${t}": must be an integer`);
	return r;
}
//#endregion
//#region src/crypto/NUT28.ts
var wn = o("Cashu_P2BK_v1"), Tn = wn.slice();
function En(e, t, r = !0) {
	let i = +!r, a = e.length + i;
	if (a > 11) throw new S(`Too many pubkeys, ${a} slots provided, maximum allowed is 11 in total`);
	if (!e.length) return {
		blinded: [],
		Ehex: ""
	};
	t = t ?? h.utils.randomSecretKey();
	let o = h.Point.Fn.fromBytes(t), s = h.getPublicKey(t, !0);
	return {
		blinded: e.map((e, t) => {
			let n = P(e), r = kn(n, o, t + i), a = n.add(h.Point.BASE.multiply(r));
			if (a.equals(h.Point.ZERO)) throw new S("Blinded key at infinity");
			return a.toHex(!0);
		}),
		Ehex: n(s)
	};
}
function Dn(e, t, n, r = !0) {
	let a = +!r, o = Array.isArray(t) ? t : [t], s = Array.isArray(n) ? n : [n], c = /* @__PURE__ */ new Set(), l = h.Point.fromHex(e);
	for (let e of o) {
		let t = h.Point.Fn.fromBytes(i(e)), n = h.getPublicKey(i(e), !0);
		s.forEach((r, o) => {
			let s = On(e, kn(l, t, o + a), i(r), n);
			s && c.add(s);
		});
	}
	return Array.from(c);
}
function On(e, t, r, i) {
	let a = h.Point.CURVE().n, o = typeof e == "string" ? Vi(e) : e, s = typeof t == "string" ? Vi(t) : t;
	if (o <= 0n || o >= a) throw new S("Invalid private key");
	if (s <= 0n || s >= a) throw new S("Invalid scalar r");
	if (i = i ?? h.Point.BASE.multiply(o).toBytes(!0), i.length !== 33) throw new S("naturalPub must be 33 bytes");
	let c = (o + s) % a, l = (a - o + s) % a;
	if (!r) {
		if (c === 0n) throw new S("Derived secret key is zero");
		return Hi(c);
	}
	if (r.length !== 33) throw new S("blindPubkey must be 33 bytes");
	let u = h.Point.fromHex(n(r)), d = h.Point.BASE.multiply(s), f = u.subtract(d);
	if (f.equals(h.Point.ZERO) || !_(f.toBytes(!0).slice(1), i.slice(1))) return null;
	let p = (f.toBytes(!0)[0] & 1) == (i[0] & 1) ? c : l;
	if (p === 0n) throw new S("Derived secret key is zero");
	return Hi(p);
}
function kn(t, n, i) {
	if (!Number.isInteger(i) || i < 0 || i > 255) throw new S("P2BK: slot index must be an integer in [0, 255]");
	let a = t.multiply(n).toBytes(!0).slice(1), o = new Uint8Array([i & 255]), s = g(e(r(wn, a, o)));
	if ((s === 0n || s >= h.Point.CURVE().n) && (s = g(e(r(wn, a, o, new Uint8Array([255])))), s === 0n || s >= h.Point.CURVE().n)) throw new S("P2BK: tweak derivation failed");
	return s;
}
//#endregion
//#region src/crypto/NUT11.ts
var An = {
	SIG_INPUTS: "SIG_INPUTS",
	SIG_ALL: "SIG_ALL"
}, jn = new Set(Object.values(An)), Mn = new Set([
	"locktime",
	"pubkeys",
	"n_sigs",
	"refund",
	"n_sigs_refund",
	"sigflag"
]);
function Nn(e) {
	return Mn.has(e);
}
function Pn(e, t) {
	let n = hn("P2PK", e, t);
	return L(n), n;
}
function L(e) {
	if (typeof e == "string" && e.length > 1024) throw new S(`Secret too long (${e.length} characters), maximum is ${fe}`);
	let t = gn(["P2PK", "HTLC"], e);
	Qn(bn(t));
	let n = Cn(t, "sigflag");
	return n !== void 0 && $n(n), t;
}
function Fn(e) {
	let t = e.toLowerCase();
	if (t.length === 66 && (t.startsWith("02") || t.startsWith("03"))) return t;
	if (t.length === 64) return `02${t}`;
	throw new S(`Invalid pubkey, expected 33 byte compressed or 32 byte x only, got length ${t.length}`);
}
function R(e) {
	let t = /* @__PURE__ */ new Set(), n = [];
	for (let r of e) {
		let e = Fn(r), i = e.slice(-64);
		t.has(i) || (t.add(i), n.push(e));
	}
	return n;
}
function In(e) {
	let t = R(Array.isArray(e.pubkey) ? e.pubkey : [e.pubkey]), n = R(e.refundKeys ?? []), r = typeof e.hashlock == "string" && e.hashlock.length > 0;
	if (t.length === 0 && !r) throw new S("P2PK requires at least one pubkey");
	let i = +!!r + t.length + n.length;
	if (i > 11) throw new S(`Too many pubkeys, ${i} slots provided, maximum allowed is 11 in total`);
	e.sigFlag !== void 0 && $n(e.sigFlag);
	let a = t.length > 0 ? e.requiredSignatures ?? 1 : e.requiredSignatures, o = e.requiredRefundSignatures;
	return tr({
		mainKeyCount: t.length,
		refundKeyCount: n.length,
		nSigs: a,
		nSigsRefund: o,
		hasLocktime: e.locktime !== void 0
	}), {
		pubkey: t.length === 1 ? t[0] : t,
		...e.locktime === void 0 ? {} : { locktime: e.locktime },
		...n.length > 0 ? { refundKeys: n } : {},
		...a !== void 0 && a > 1 ? { requiredSignatures: a } : {},
		...o !== void 0 && o > 1 ? { requiredRefundSignatures: o } : {},
		...e.additionalTags?.length ? { additionalTags: e.additionalTags } : {},
		...e.blindKeys ? { blindKeys: !0 } : {},
		...e.sigFlag === void 0 ? {} : { sigFlag: e.sigFlag },
		...e.hashlock ? { hashlock: e.hashlock } : {}
	};
}
function Ln(e) {
	let t = L(e), n = ar(ir(t)), r = nr(t), i = rr(t);
	return n === "ACTIVE" || n === "PERMANENT" ? r : n === "EXPIRED" && i.length ? Array.from(new Set([...r, ...i])) : [];
}
function Rn(e, t, r = Math.floor(Date.now() / 1e3)) {
	let a = L(e.secret), o = nr(a), s = rr(a), c = ir(a);
	tr({
		mainKeyCount: o.length,
		refundKeyCount: s.length,
		nSigs: I(a, "n_sigs"),
		nSigsRefund: I(a, "n_sigs_refund"),
		hasLocktime: Number.isFinite(c)
	});
	let l = e.p2pk_e === void 0 ? t : Jn(t, e), u = new Set(l.map((e) => n(fn(i(e))).slice(2))), d = (e) => e.filter((e) => u.has(e.slice(2))).length, f = (e, t) => t.length ? Math.max(I(a, e) ?? 1, 1) : 0;
	return _n(a) !== "HTLC" && d(o) >= f("n_sigs", o) ? !0 : Number.isFinite(c) && r > c && d(s) >= f("n_sigs_refund", s);
}
function zn(e) {
	return Cn(L(e), "sigflag") ?? "SIG_INPUTS";
}
function Bn(e) {
	let t = Vn(e)?.signatures ?? [];
	if (t.length > 64) throw new S(`Too many witness signatures: ${t.length}`);
	return t;
}
function Vn(e) {
	if (!e) return;
	if (typeof e == "string" && e.length > 16384) throw new S(`Witness too long (${e.length} characters), maximum is ${pe}`);
	let t;
	try {
		t = typeof e == "string" ? JSON.parse(e) : e;
	} catch {
		return;
	}
	if (!t || typeof t != "object") return;
	let n = { signatures: Array.isArray(t.signatures) ? t.signatures : [] };
	return typeof t.preimage == "string" && t.preimage.length > 0 && (n.preimage = t.preimage), n;
}
function Hn(e, t, r = O, i) {
	let a = (e) => typeof e == "string" ? e : n(e), o = Array.isArray(t) ? t.map(a) : a(t);
	return e.map((e, t) => {
		let n = Jn(o, e), a = e;
		for (let e of n) try {
			a = Wn(a, e, i);
		} catch (e) {
			let n = e instanceof Error ? e.message : "Unknown error";
			r.warn(`Proof #${t + 1}: ${n}`);
		}
		return a;
	});
}
function Un(e, t) {
	let r = typeof t == "string" ? i(t) : t, a = n(m.getPublicKey(r));
	if (!Ln(e).some((e) => e.slice(2) === a)) throw new S(`Signature not required from [02|03]${a}`);
	return a;
}
function Wn(e, t, n) {
	let r = L(e.secret), i = zn(r) === "SIG_ALL";
	if (i && n === void 0) throw new S("Cannot sign a SIG_ALL proof without the message to sign");
	if (!i && n !== void 0) throw new S("A message override is only valid for SIG_ALL proofs");
	n = n ?? e.secret;
	let a = Un(r, t), o = Bn(e.witness);
	if (o.some((e) => nn(e, n, a))) throw new S(`Proof already signed by [02|03]${a}`);
	if (o.length >= 64) throw new S("Cannot sign: witness already at the 64-signature limit");
	let s = tn(n, t), c = Vn(e.witness), l = {
		...c && c.preimage !== void 0 ? { preimage: c.preimage } : {},
		signatures: [...c?.signatures ?? [], s]
	};
	return {
		...e,
		witness: l
	};
}
function Gn(e, t, n) {
	if (!t.witness) return !1;
	if (!Zn([t])) n = t.secret;
	else if (!n) throw new S("Cannot verify a SIG_ALL proof without the message to sign");
	return Bn(t.witness).some((t) => nn(t, n, e));
}
function Kn(e, t = O, n) {
	if (!Zn([e])) n = e.secret;
	else if (!n) throw t.error("Cannot verify a SIG_ALL proof without the message to sign"), new S("Cannot verify a SIG_ALL proof without the message to sign");
	let r = L(e.secret), i = nr(r), a = rr(r);
	tr({
		mainKeyCount: i.length,
		refundKeyCount: a.length,
		nSigs: I(r, "n_sigs"),
		nSigsRefund: I(r, "n_sigs_refund"),
		hasLocktime: Number.isFinite(ir(r))
	});
	let o = Bn(e.witness), s = ir(r), c = ar(s), l = i.length === 0 ? 0 : or(r, c, a), u = sr(r, c, a), d = sn(o, n, i), f = a.length ? sn(o, n, a) : [], p = {
		locktime: s,
		lockState: c,
		main: {
			pubkeys: i,
			requiredSigners: l,
			receivedSigners: d
		},
		refund: {
			pubkeys: a,
			requiredSigners: u,
			receivedSigners: f
		}
	};
	if (i.length && l > 0 && d.length >= l) {
		let e = {
			...p,
			success: !0,
			path: "MAIN"
		};
		return t.debug("Spending condition satisfied via main pubkeys", { result: e }), e;
	}
	if (c !== "EXPIRED") {
		let e = {
			...p,
			success: !1,
			path: "FAILED"
		};
		return t.debug("P2PK lock enabled, but threshold not met by main pubkeys", { result: e }), e;
	}
	if (t.debug("P2PK lock expired. Checking refund path.", { lockState: c }), a.length) {
		if (u > 0 && f.length >= u) {
			let e = {
				...p,
				success: !0,
				path: "REFUND"
			};
			return t.debug("Spending condition satisfied via refund pubkeys", { result: e }), e;
		}
		let e = {
			...p,
			success: !1,
			path: "FAILED"
		};
		return t.debug("Spending threshold not met by either pathway", { result: e }), e;
	}
	let m = {
		...p,
		success: !0,
		path: "UNLOCKED"
	};
	return t.debug("No refund pubkeys, anyone can spend.", { result: m }), m;
}
function qn(e, t = O, n) {
	return Kn(e, t, n).success;
}
function Jn(e, t) {
	let n = Array.isArray(e) ? e : [e], r = t?.p2pk_e;
	if (!r) return Array.from(new Set(n));
	let i = L(t.secret), a = [...nr(i), ...rr(i)], o = _n(i) === "P2PK", s = Dn(r, n, a, o);
	return !o && !s.length ? Dn(r, n, a) : s;
}
function Yn(e) {
	if (e.length === 0) throw new S("No proofs");
	let t = L(e[0].secret);
	if (zn(t) !== "SIG_ALL") throw new S("First proof is not SIG_ALL");
	let n = t[1].data, r = JSON.stringify(t[1].tags ?? []);
	for (let i = 1; i < e.length; i++) {
		let a = L(e[i].secret);
		if (a[0] !== t[0]) throw new S(`Proof #${i + 1} is not ${t[0]}`);
		if (zn(a) !== "SIG_ALL") throw new S(`Proof #${i + 1} is not SIG_ALL`);
		if (a[1].data !== n) throw new S("SIG_ALL inputs must share identical Secret.data");
		if (JSON.stringify(a[1].tags ?? []) !== r) throw new S("SIG_ALL inputs must share identical Secret.tags");
	}
}
function Xn(e, t, n) {
	let r = [];
	for (let t of e) r.push(t.secret, t.C);
	for (let e of t) r.push(String(e.blindedMessage.amount), e.blindedMessage.B_);
	return n && r.push(n), r.join("");
}
function Zn(e) {
	return e.some((e) => {
		try {
			return zn(e.secret) === "SIG_ALL";
		} catch {
			return !1;
		}
	});
}
function Qn(e) {
	let t = /* @__PURE__ */ new Set();
	for (let n of e) {
		let e = n[0];
		if (Nn(e)) {
			if (t.has(e)) throw new S(`Duplicate P2PK tag "${e}"`);
			t.add(e);
		}
	}
}
function $n(e) {
	if (!jn.has(e)) throw new S(`Invalid sigflag "${e}": must be "SIG_INPUTS" or "SIG_ALL"`);
}
function er(e, t) {
	if (!Number.isInteger(e) || e < 1) throw new S(`${t} must be a positive integer, got ${e}`);
	return e;
}
function tr(e) {
	let { mainKeyCount: t, refundKeyCount: n, nSigs: r, nSigsRefund: i, hasLocktime: a } = e;
	if (r !== void 0 && (er(r, "requiredSignatures (n_sigs)"), r > t)) throw new S(`requiredSignatures (n_sigs) (${r}) exceeds available pubkeys (${t})`);
	if (i !== void 0) {
		if (er(i, "requiredRefundSignatures (n_sigs_refund)"), n === 0) throw new S("requiredRefundSignatures (n_sigs_refund) requires refund keys");
		if (i > n) throw new S(`requiredRefundSignatures (n_sigs_refund) (${i}) exceeds available refund keys (${n})`);
	}
	if (n > 0 && !a) throw new S("refund keys require a locktime");
}
function nr(e) {
	let t = Sn(e, "pubkeys") ?? [];
	if (t.length > 16) throw new S(`Too many pubkeys: ${t.length}`);
	let n = (_n(e) === "P2PK" ? [yn(e), ...t] : t).map((e) => Fn(e));
	if (R(n).length !== n.length) throw new S("Duplicate main pubkeys are not allowed");
	return n;
}
function rr(e) {
	let t = Sn(e, "refund") ?? [];
	if (t.length > 16) throw new S(`Too many refund pubkeys: ${t.length}`);
	let n = t.map((e) => Fn(e));
	if (R(n).length !== n.length) throw new S("Duplicate refund pubkeys are not allowed");
	return n;
}
function ir(e) {
	let t = Cn(e, "locktime"), n = t !== void 0 && /^\d+$/.test(t) ? Number(t) : NaN;
	return Number.isSafeInteger(n) ? n : Infinity;
}
function ar(e, t = Math.floor(Date.now() / 1e3)) {
	return Number.isFinite(e) ? t <= e ? "ACTIVE" : "EXPIRED" : "PERMANENT";
}
function or(e, t, n) {
	return !n.length && t === "EXPIRED" ? 0 : Math.max(I(e, "n_sigs") ?? 1, 1);
}
function sr(e, t, n) {
	return n.length && t === "EXPIRED" ? Math.max(I(e, "n_sigs_refund") ?? 1, 1) : 0;
}
//#endregion
//#region src/crypto/NUT12.ts
var cr = o("Cashu_DLEQ_R_v1");
function lr(t, n, i, a) {
	let o = r(cr, n.toBytes(!1), i.toBytes(!1), a.toBytes(!1));
	for (let n = 0; n < 256; n++) {
		let i = g(x(e, t, r(o, new Uint8Array([n]))));
		/* c8 ignore next */
		if (i > 0n && i < h.Point.CURVE().n) return i;
	}
	/* c8 ignore next */
	throw new S("DLEQ nonce derivation failed");
}
var ur = (e, t, n, r) => {
	let i = h.Point.Fn.fromBytes(e.s), a = h.Point.Fn.fromBytes(e.e), o = h.Point.BASE.multiply(i), s = r.multiply(a), c = t.multiply(i), l = n.multiply(a);
	return _(Vt([
		o.subtract(s),
		c.subtract(l),
		r,
		n
	]), e.e);
}, dr = (e, t, n, r) => {
	if (t.r === void 0) throw new S("verifyDLEQProof_reblind: Undefined blinding factor");
	let i = Bt(e), a = n.add(r.multiply(t.r)), o = h.Point.BASE.multiply(t.r);
	return ur(t, i.add(o), a, r);
}, fr = (e, t) => {
	e = Ut(e);
	let n = h.Point.Fn.fromBytes(t), r = h.Point.BASE.multiply(n), i = e.multiply(n), a = lr(t, r, e, i), o = Vt([
		h.Point.BASE.multiply(a),
		e.multiply(a),
		r,
		i
	]), s = h.Point.Fn.fromBytes(o);
	return {
		s: v(h.Point.Fn.add(a, h.Point.Fn.mul(s, n)), 32),
		e: o
	};
}, pr = "m/129372'/0'", mr = BigInt("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141"), hr = /* @__PURE__ */ function(e) {
	return e[e.SECRET = 0] = "SECRET", e[e.BLINDING_FACTOR = 1] = "BLINDING_FACTOR", e;
}(hr || {}), gr = /* @__PURE__ */ function(e) {
	return e[e.DEPRECATED_BIP32 = 0] = "DEPRECATED_BIP32", e[e.HMAC_SHA256 = 1] = "HMAC_SHA256", e;
}(gr || {}), _r = (e, t, n) => yr(e, t, n).secret, vr = (e, t, n) => yr(e, t, n).blindingFactor;
function yr(e, t, n) {
	return br(e, t)(n);
}
function br(e, t) {
	switch (Sr(e), xr(t)) {
		case gr.DEPRECATED_BIP32: {
			let n = Kt(t), r = b.fromMasterSeed(e).derive(`${pr}/${n}'`);
			return (e) => Cr(r, e);
		}
		case gr.HMAC_SHA256: return (n) => wr(e, t, n);
	}
}
function xr(e) {
	if (e.length === 12 && at(e)) return gr.DEPRECATED_BIP32;
	let t = /^[a-fA-F0-9]+$/.test(e), n = e.startsWith("01");
	if (t && n && e.length % 2 != 0) throw new S("Invalid hex string: odd length.");
	if (!t && at(e) || t && e.startsWith("00")) return gr.DEPRECATED_BIP32;
	if (t && n) return gr.HMAC_SHA256;
	throw new S(`Unrecognized keyset ID version ${e.slice(0, 2)}`);
}
function Sr(e) {
	if (!(e instanceof Uint8Array) || e.length < 16 || e.length > 64) throw new S("seed must be a 16 to 64 byte Uint8Array");
}
function Cr(e, t) {
	if (!Number.isInteger(t) || t < 0 || t >= 2147483648) throw new S("Counter must be an integer in the range 0 <= counter < 2^31");
	let n = e.deriveChild(y + t), r = n.deriveChild(0).privateKey, i = n.deriveChild(1).privateKey;
	/* c8 ignore next */
	if (r === null || i === null) throw new S("Could not derive private key");
	return {
		secret: r,
		blindingFactor: i
	};
}
function wr(e, t, n) {
	if (!Number.isSafeInteger(n) || n < 0) throw new S("Counter must be an integer in the range 0 <= counter <= 2^53 - 1");
	return {
		secret: Tr(e, t, n, hr.SECRET),
		blindingFactor: Tr(e, t, n, hr.BLINDING_FACTOR)
	};
}
function Tr(t, n, a, s) {
	let c = r(o("Cashu_KDF_HMAC_SHA256"), i(n), v(a, 8));
	switch (s) {
		case hr.SECRET:
			c = r(c, i("00"));
			break;
		case hr.BLINDING_FACTOR: c = r(c, i("01"));
	}
	let l = x(e, t, c);
	if (s === hr.BLINDING_FACTOR) {
		let e = g(l), t = e >= mr ? e - mr : e;
		/* c8 ignore next */
		if (t === 0n) throw new S("Derived invalid blinding scalar r == 0");
		return v(t, 32);
	}
	return l;
}
//#endregion
//#region src/crypto/NUT14.ts
function Er(e, t) {
	if (!/^[0-9a-f]{64}$/i.test(e)) throw new S("Hashlock must be a 64-character hex string (32 bytes).");
	return hn("HTLC", e.toLowerCase(), t);
}
function Dr(e) {
	return gn("HTLC", L(e));
}
function Or(t) {
	let r = t !== void 0;
	if (r && !/^[0-9a-f]{64}$/i.test(t)) throw new S("Preimage must be a 64 character hexadecimal string (32 bytes).");
	let o = r ? i(t) : a(32);
	return {
		hash: n(e(o)),
		preimage: n(o)
	};
}
function kr(e, t) {
	try {
		return Or(e).hash === t.toLowerCase();
	} catch {
		return !1;
	}
}
function Ar(e, t = O, n) {
	let r, i = L(e.secret), a = _n(i) === "HTLC", o = a ? yn(i) : "", s = Kn(e, t, n);
	if (!a) return s;
	let c = s.main.pubkeys.length === 0;
	if (!c && s.path !== "MAIN" || c && s.success) return s;
	let l = Mr(e.witness);
	if (l && kr(l, o)) return r = {
		...s,
		success: !0,
		path: "MAIN"
	}, t.debug("Spending condition satisfied via hashlock (receiver) pathway", { result: r }), r;
	let { refund: u } = s;
	return u.requiredSigners > 0 && u.receivedSigners.length >= u.requiredSigners ? (r = {
		...s,
		success: !0,
		path: "REFUND"
	}, t.debug("Spending condition satisfied via refund pubkeys", { result: r }), r) : l ? (r = {
		...s,
		success: !1,
		path: "FAILED"
	}, t.debug("Hashlock spend failed, wrong preimage for hash", { result: r }), r) : (r = {
		...s,
		success: !1,
		path: "FAILED"
	}, t.debug("Hashlock spend failed, no preimage found", { result: r }), r);
}
function jr(e, t = O, n) {
	return Ar(e, t, n).success;
}
function Mr(e) {
	return Vn(e)?.preimage;
}
//#endregion
//#region src/crypto/NUT20.ts
var Nr = o("Cashu_MintQuoteSig_v1");
function Pr(e) {
	let t = j.from(e.amount).toBigInt();
	if (t === 0n) return new Uint8Array();
	let n = t.toString(16);
	return i(n.length % 2 == 1 ? "0" + n : n);
}
function Fr(t, n) {
	let r = e.create();
	r.update(Nr);
	let a = o(t);
	r.update(v(a.length, 4)), r.update(a);
	for (let e of n) {
		let t = Pr(e);
		r.update(v(t.length, 4)), r.update(t);
		let n = i(e.B_);
		r.update(v(n.length, 4)), r.update(n);
	}
	return r.digest();
}
function Ir(t, n) {
	let r = t;
	for (let e of n) r += e.B_;
	return e(o(r));
}
function Lr(e) {
	return typeof e == "string" && e.length === 66;
}
function Rr(e, t, n) {
	return en(Ir(t, n), e);
}
function zr(e, t, n, r) {
	if (!Lr(e)) return !1;
	try {
		return rn(r, Ir(t, n), e);
	} catch {
		return !1;
	}
}
function Br(e, t, n) {
	return en(Fr(t, n), e);
}
//#endregion
//#region src/wallet/types/payment-requests.ts
var Vr = /* @__PURE__ */ function(e) {
	return e.POST = "post", e.NOSTR = "nostr", e;
}({}), Hr = 1, Ur = 2, Wr = 3, Gr = 4, Kr = 5, qr = 6, Jr = 7, Yr = 8, Xr = 9, Zr = 10, Qr = 1, $r = 2, ei = 1, ti = 2, ni = 3, ri = 0, ii = 1, ai = 1, oi = 2, si = 3, ci = 0, li = 1;
function ui(e) {
	let t = di(e), n = {};
	for (let e of t) switch (e.tag) {
		case Hr:
			if (n.id !== void 0) throw new S("invalid pr: multiple id fields");
			n.id = z(e.value);
			break;
		case Ur:
			if (n.amount !== void 0) throw new S("invalid pr: multiple amount fields");
			n.amount = pi(e.value);
			break;
		case Wr:
			if (n.unit !== void 0) throw new S("invalid pr: multiple unit fields");
			e.value.length === 1 && e.value[0] === 0 ? n.unit = "sat" : n.unit = z(e.value);
			break;
		case Gr:
			if (n.singleUse !== void 0) throw new S("invalid pr: multiple single_use fields");
			n.singleUse = mi(e.value) === 1;
			break;
		case Kr:
			n.mints || (n.mints = []), n.mints.push(z(e.value));
			break;
		case qr:
			if (n.description !== void 0) throw new S("invalid pr: multiple description fields");
			n.description = z(e.value);
			break;
		case Jr:
			n.transports || (n.transports = []), n.transports.push(_i(e.value));
			break;
		case Yr:
			if (n.nut10) throw new S("invalid pr: multiple nut10 spending conditions");
			n.nut10 = yi(e.value);
			break;
		case Xr:
			if (n.mintsPreferred !== void 0) throw new S("invalid pr: multiple mint_preferred fields");
			n.mintsPreferred = mi(e.value) === 1;
			break;
		case Zr:
			n.supportedMethods || (n.supportedMethods = []), n.supportedMethods.push(vi(e.value));
			break;
		default: break;
	}
	return n;
}
function di(e) {
	let t = [], n = 0;
	for (; n < e.length;) {
		let r = fi(e.subarray(n));
		t.push(r), n += 3 + r.length;
	}
	return t;
}
function fi(e) {
	if (e.length < 3) throw new S("TLV data too short: need at least 3 bytes for tag and length");
	let t = new DataView(e.buffer, e.byteOffset, e.byteLength), n = t.getUint8(0), r = t.getUint16(1, !1);
	if (e.length < 3 + r) throw new S(`TLV data too short: expected ${3 + r} bytes, got ${e.length}`);
	return {
		tag: n,
		length: r,
		value: e.subarray(3, 3 + r)
	};
}
function z(e) {
	return Te(e);
}
function pi(e) {
	if (e.length !== 8) throw new S(`Invalid u64: expected 8 bytes, got ${e.length}`);
	return new DataView(e.buffer, e.byteOffset, e.byteLength).getBigUint64(0, !1);
}
function mi(e) {
	if (e.length !== 1) throw new S(`Invalid u8: expected 1 byte, got ${e.length}`);
	return e[0];
}
function hi(e) {
	switch (e) {
		case ri: return "nostr";
		case ii: return "post";
		default: throw new S(`Unsupported transport kind: ${e}`);
	}
}
function gi(e) {
	switch (e) {
		case ci: return "P2PK";
		case li: return "HTLC";
		default: throw new S(`Unsupported NUT-10 kind: ${e}`);
	}
}
function _i(e) {
	let t = di(e), n, r, i;
	for (let e of t) switch (e.tag) {
		case ei:
			if (n !== void 0) throw new S("invalid pr: multiple transport kind fields");
			n = mi(e.value);
			break;
		case ti:
			if (r !== void 0) throw new S("invalid pr: multiple transport target fields");
			r = e.value;
			break;
		case ni:
			i || (i = []), i.push(bi(e.value));
			break;
	}
	if (n === void 0) throw new S("Transport missing required kind field");
	if (r === void 0) throw new S("Transport missing required target field");
	let a;
	if (n === ri) {
		let e = i?.filter((e) => e[0] === "r").flatMap((e) => e.slice(1)) ?? [];
		a = ji(r, e), i = i?.filter((e) => e[0] !== "r");
	} else a = z(r);
	let o = i && i.length > 0 ? i : void 0;
	return {
		type: hi(n),
		target: a,
		tags: o
	};
}
function vi(e) {
	let t = di(e), n, r;
	for (let e of t) switch (e.tag) {
		case Qr:
			if (n !== void 0) throw new S("invalid pr: multiple supported_method method fields");
			n = z(e.value);
			break;
		case $r:
			if (r !== void 0) throw new S("invalid pr: multiple supported_method fee fields");
			r = pi(e.value);
			break;
	}
	if (n === void 0) throw new S("supported_method missing required method field");
	return r === void 0 ? { method: n } : {
		method: n,
		fee: r
	};
}
function yi(e) {
	let t = di(e), n, r, i;
	for (let e of t) switch (e.tag) {
		case ai:
			if (n !== void 0) throw new S("invalid pr: multiple nut10 kind fields");
			n = mi(e.value);
			break;
		case oi:
			if (r !== void 0) throw new S("invalid pr: multiple nut10 data fields");
			r = z(e.value);
			break;
		case si:
			i || (i = []), i.push(bi(e.value));
			break;
	}
	if (n === void 0) throw new S("NUT-10 spending condition missing required kind field");
	if (r === void 0) throw new S("NUT-10 spending condition missing required data field");
	let a = i && i.length > 0 ? i : void 0;
	return {
		kind: gi(n),
		data: r,
		tags: a
	};
}
function bi(e) {
	let t = [], n = 0;
	for (; n < e.length;) {
		let r = e[n];
		if (n += 1, e.length - n < r) throw new S(`Tag tuple data too short: expected ${r} bytes, got ${e.length - n}`);
		let i = z(e.subarray(n, n + r));
		t.push(i), n += r;
	}
	return t;
}
function xi(e) {
	let t = [];
	if (e.id && t.push(B(Hr, V(e.id))), e.amount !== void 0 && t.push(B(Ur, Si(e.amount))), e.unit) if (e.unit === "sat") t.push(B(Wr, new Uint8Array([0])));
	else {
		let n = V(e.unit);
		if (n.length === 1 && n[0] === 0) throw new S("invalid pr: unit encoding is reserved for sat");
		t.push(B(Wr, n));
	}
	if (e.singleUse !== void 0 && t.push(B(Gr, Ci(+!!e.singleUse))), e.mints && e.mints.length > 0) for (let n of e.mints) t.push(B(Kr, V(n)));
	if (e.description && t.push(B(qr, V(e.description))), e.transports && e.transports.length > 0) for (let n of e.transports) t.push(B(Jr, Ei(n)));
	if (e.nut10 && t.push(B(Yr, Oi(e.nut10))), e.mintsPreferred !== void 0 && t.push(B(Xr, Ci(+!!e.mintsPreferred))), e.supportedMethods && e.supportedMethods.length > 0) for (let n of e.supportedMethods) t.push(B(Zr, Di(n)));
	let n = t.reduce((e, t) => e + t.length, 0), r = new Uint8Array(n), i = 0;
	for (let e of t) r.set(e, i), i += e.length;
	return r;
}
function B(e, t) {
	let n = t.length;
	if (n > 65535) throw new S(`TLV value too long: ${n} bytes (max 65535)`);
	let r = new Uint8Array(3 + n);
	return r[0] = e, r[1] = n >> 8 & 255, r[2] = n & 255, r.set(t, 3), r;
}
function V(e) {
	if (ke(e)) throw new S("TLV text must be well-formed UTF-16");
	return new TextEncoder().encode(e);
}
function Si(e) {
	let t = /* @__PURE__ */ new ArrayBuffer(8);
	return new DataView(t).setBigUint64(0, e, !1), new Uint8Array(t);
}
function Ci(e) {
	return new Uint8Array([e]);
}
function wi(e) {
	switch (e) {
		case Vr.NOSTR: return ri;
		case Vr.POST: return ii;
		default: throw new S(`Unsupported transport type: ${e}`);
	}
}
function Ti(e) {
	switch (e) {
		case "P2PK": return ci;
		case "HTLC": return li;
		default: throw new S(`Unsupported NUT-10 type: ${e}`);
	}
}
function Ei(e) {
	let t = [], n = wi(e.type);
	t.push(B(ei, Ci(n)));
	let r, i = [];
	if (e.type === Vr.NOSTR) {
		let { pubkey: t, relays: n } = Ai(e.target);
		r = t, i = n.map((e) => ["r", e]);
	} else r = V(e.target);
	t.push(B(ti, r));
	let a = [...i, ...e.tags || []];
	if (a.length > 0) for (let e of a) t.push(B(ni, ki(e)));
	let o = t.reduce((e, t) => e + t.length, 0), s = new Uint8Array(o), c = 0;
	for (let e of t) s.set(e, c), c += e.length;
	return s;
}
function Di(e) {
	let t = [];
	t.push(B(Qr, V(e.method))), e.fee !== void 0 && t.push(B($r, Si(e.fee)));
	let n = t.reduce((e, t) => e + t.length, 0), r = new Uint8Array(n), i = 0;
	for (let e of t) r.set(e, i), i += e.length;
	return r;
}
function Oi(e) {
	let t = [], n = Ti(e.kind);
	if (t.push(B(ai, Ci(n))), t.push(B(oi, V(e.data))), e.tags && e.tags.length > 0) for (let n of e.tags) t.push(B(si, ki(n)));
	let r = t.reduce((e, t) => e + t.length, 0), i = new Uint8Array(r), a = 0;
	for (let e of t) i.set(e, a), a += e.length;
	return i;
}
function ki(e) {
	let t = new TextEncoder(), n = [];
	for (let r of e) {
		let e = t.encode(r);
		if (e.length > 255) throw new S(`Tag tuple string too long: ${r} (max 255 bytes)`);
		let i = new Uint8Array(1 + e.length);
		i[0] = e.length, i.set(e, 1), n.push(i);
	}
	let r = n.reduce((e, t) => e + t.length, 0), i = new Uint8Array(r), a = 0;
	for (let e of n) i.set(e, a), a += e.length;
	return i;
}
function Ai(e) {
	let t = d.decode(e, 1024);
	if (t.prefix !== "nprofile") throw new S(`Invalid nprofile: expected prefix 'nprofile', got '${t.prefix}'`);
	let n = d.fromWords(t.words), r = new Uint8Array(n), i, a = [], o = 0;
	for (; o < r.length;) {
		if (o + 2 > r.length) throw new S("Nprofile TLV data too short");
		let e = r[o], t = r[o + 1];
		if (o += 2, o + t > r.length) throw new S(`Nprofile TLV value too short: expected ${t} bytes`);
		let n = r.subarray(o, o + t);
		if (o += t, e === 0) {
			if (i !== void 0) throw new S("Nprofile contains multiple pubkeys");
			if (n.length !== 32) throw new S(`Invalid pubkey length: expected 32 bytes, got ${n.length}`);
			i = n;
		} else e === 1 && a.push(Te(n));
	}
	if (!i) throw new S("Nprofile missing required pubkey");
	return {
		pubkey: i,
		relays: a
	};
}
function ji(e, t) {
	let n = Mi(e, t), r = d.toWords(n);
	return d.encode("nprofile", r, 1024);
}
function Mi(e, t) {
	if (e.length !== 32) throw new S(`Invalid pubkey: expected 32 bytes, got ${e.length}`);
	let n = new TextEncoder(), r = t.map((e) => n.encode(e));
	for (let e = 0; e < r.length; e++) if (r[e].length > 255) throw new S(`Relay URL too long: ${t[e]} (max 255 bytes)`);
	let i = 34 + r.reduce((e, t) => e + 2 + t.length, 0), a = new Uint8Array(i), o = 0;
	a[o++] = 0, a[o++] = 32, a.set(e, o), o += 32;
	for (let e of r) a[o++] = 1, a[o++] = e.length, a.set(e, o), o += e.length;
	return a;
}
//#endregion
//#region src/model/PaymentRequest.ts
function Ni(e, t) {
	if (e != null) {
		if (e === !0 || e === 1) return !0;
		if (e === !1 || e === 0) return !1;
		throw new S(`invalid payment request: ${t} must be a boolean (or 0/1)`);
	}
}
function Pi(e) {
	if (e != null) {
		if (!Array.isArray(e)) throw new S("invalid payment request: sm must be a list");
		return e.map((e, t) => {
			if (typeof e != "object" || !e || typeof e.mn != "string") throw new S(`invalid payment request: malformed sm entry at index ${t}`);
			let { mn: n, mf: r } = e;
			return {
				method: n,
				fee: r ?? void 0
			};
		});
	}
}
var Fi = class e {
	constructor(e, t, n, r, i, a, o = !1, s, c, l) {
		this.transport = e, this.id = t, this.unit = r, this.mints = i, this.description = a, this.singleUse = o, this.nut10 = s, this.mintsPreferred = c, this.amount = n === void 0 ? void 0 : j.from(n), this.mintsPreferred = Ni(c, "mp"), this.supportedMethods = l?.map((e) => ({
			method: e.method,
			fee: e.fee === void 0 ? void 0 : j.from(e.fee)
		}));
	}
	assertUnitRule() {
		if (!this.unit && (this.amount !== void 0 || this.supportedMethods?.length)) throw new S("invalid payment request: unit (u) is required when an amount (a) or supported methods (sm) are set");
	}
	feesFor(e, t) {
		if (this.assertUnitRule(), !this.supportedMethods?.length || this.includesMint(e)) return j.zero();
		let n = this.supportedMethods.filter((e) => t?.includes(e.method)).map((e) => e.fee ?? j.zero());
		return n.length ? n.reduce((e, t) => j.min(e, t)) : j.zero();
	}
	amountToSend(e, t) {
		if (!this.amount) throw new S("cannot compute amount to send: request has no amount; use feesFor() and add the payer-chosen amount");
		return this.amount.add(this.feesFor(e, t));
	}
	get isMintListStrict() {
		if (this.mints?.length) return this.mintsPreferred !== !0;
	}
	includesMint(e) {
		let t = (e) => {
			try {
				return ua(e);
			} catch {
				return e;
			}
		}, n = t(e);
		return this.mints?.some((e) => t(e) === n) ?? !1;
	}
	encodePayload(e, t, n) {
		if (this.isMintListStrict && !this.includesMint(e)) throw new S("mint is not in the request's strict mint list");
		let r = {
			...this.id !== void 0 && { id: this.id },
			...n?.memo !== void 0 && { memo: n.memo },
			unit: this.unit ?? n?.unit ?? "sat",
			mint: e,
			proofs: t
		};
		return M.stringify(r);
	}
	static decodePayload(e) {
		let t;
		try {
			t = M.parse(e, void 0, { strict: !0 });
		} catch (e) {
			throw new S("invalid payment payload: not valid JSON", { cause: e });
		}
		if (typeof t != "object" || !t || Array.isArray(t)) throw new S("invalid payment payload: expected a JSON object");
		let { id: n, memo: r, unit: i, mint: a, proofs: o } = t;
		if (typeof a != "string" || !a) throw new S("invalid payment payload: missing mint");
		if (typeof i != "string" || !i) throw new S("invalid payment payload: missing unit");
		if (n !== void 0 && typeof n != "string") throw new S("invalid payment payload: id must be a string");
		if (r !== void 0 && typeof r != "string") throw new S("invalid payment payload: memo must be a string");
		if (!Array.isArray(o) || o.length === 0) throw new S("invalid payment payload: missing proofs");
		let s = o.map((e, t) => {
			if (typeof e != "object" || !e || Array.isArray(e) || typeof e.id != "string" || typeof e.secret != "string" || typeof e.C != "string") throw new S(`invalid payment payload: malformed proof at index ${t}`);
			let n = e.amount;
			if (typeof n != "number" && typeof n != "bigint" && typeof n != "string") throw new S(`invalid payment payload: malformed proof amount at index ${t}`);
			let { dleq: r, ...i } = e;
			return {
				...i,
				amount: j.from(n),
				...r?.r && r.s && r.e && { dleq: r }
			};
		});
		return {
			...n !== void 0 && { id: n },
			...r !== void 0 && { memo: r },
			unit: i,
			mint: a,
			proofs: s
		};
	}
	toRawRequest() {
		let e = {};
		return this.transport && (e.t = this.transport.map((e) => ({
			t: e.type,
			a: e.target,
			g: e.tags
		}))), this.id && (e.i = this.id), this.amount && (e.a = this.amount.toBigInt()), this.unit && (e.u = this.unit), this.mints && (e.m = this.mints), this.description && (e.d = this.description), this.singleUse && (e.s = this.singleUse), this.nut10 && (e.nut10 = {
			k: this.nut10.kind,
			d: this.nut10.data,
			t: this.nut10.tags
		}), this.mintsPreferred !== void 0 && (e.mp = this.mintsPreferred), this.supportedMethods && this.supportedMethods.length > 0 && (e.sm = this.supportedMethods.map((e) => e.fee === void 0 ? { mn: e.method } : {
			mn: e.method,
			mf: e.fee.toBigInt()
		})), e;
	}
	toEncodedRequest() {
		return "creqA" + et(ft(this.toRawRequest()));
	}
	toEncodedCreqA() {
		return this.toEncodedRequest();
	}
	toEncodedCreqB() {
		return ct("creqb", xi({
			id: this.id,
			amount: this.amount === void 0 ? void 0 : this.amount.toBigInt(),
			unit: this.unit,
			singleUse: this.singleUse,
			mints: this.mints,
			description: this.description,
			transports: this.transport,
			nut10: this.nut10 ? {
				kind: this.nut10.kind,
				data: this.nut10.data,
				tags: this.nut10.tags
			} : void 0,
			mintsPreferred: this.mintsPreferred,
			supportedMethods: this.supportedMethods?.map((e) => ({
				method: e.method,
				fee: e.fee === void 0 ? void 0 : e.fee.toBigInt()
			}))
		})).toUpperCase();
	}
	getTransport(e) {
		return this.transport?.find((t) => t.type === e);
	}
	toP2PKOptions() {
		return Ii(this.nut10);
	}
	static fromRawRequest(t) {
		let n = t.t ? t.t.map((e) => ({
			type: e.t,
			target: e.a,
			tags: e.g
		})) : void 0, r = t.nut10 ? {
			kind: t.nut10.k,
			data: t.nut10.d,
			tags: t.nut10.t
		} : void 0;
		return new e(n, t.i, t.a, t.u, t.m, t.d, t.s, r, t.mp, Pi(t.sm));
	}
	static fromEncodedRequest(t) {
		let n = t.toLowerCase();
		if (n.startsWith("creqb")) {
			let t = ui(ut(n)), r = t.nut10 ? {
				kind: t.nut10.kind,
				data: t.nut10.data,
				tags: t.nut10.tags ?? []
			} : void 0;
			return new e(t.transports, t.id, t.amount, t.unit, t.mints, t.description, t.singleUse ?? !1, r, t.mintsPreferred, t.supportedMethods);
		}
		if (!t.startsWith("creq")) throw new S("unsupported pr: invalid prefix");
		if (t[4] !== "A") throw new S("unsupported pr version");
		let r = wt(rt(t.slice(5)));
		return this.fromRawRequest(r);
	}
};
function Ii(e) {
	let t = e?.kind === "HTLC";
	if (!e || e.kind !== "P2PK" && !t) return;
	if (!e.data) throw new S(`NUT-10 ${e.kind} option is missing its data field`);
	let n = L([e.kind, {
		nonce: "",
		data: e.data,
		tags: e.tags ?? []
	}]), r = Sn(n, "pubkeys") ?? [], i = [e.data, ...r], a = t ? {
		hashlock: e.data.toLowerCase(),
		pubkey: r
	} : { pubkey: i.length === 1 ? i[0] : i };
	a.locktime = I(n, "locktime"), a.refundKeys = Sn(n, "refund"), a.requiredSignatures = I(n, "n_sigs"), a.requiredRefundSignatures = I(n, "n_sigs_refund"), Cn(n, "sigflag") === "SIG_ALL" && (a.sigFlag = "SIG_ALL");
	let o = (e.tags ?? []).filter((e) => e.length > 0 && !Nn(e[0]));
	return o.length > 0 && (a.additionalTags = o), a;
}
//#endregion
//#region src/utils/core.ts
function H(e, t, n, r) {
	let i = Bi(e, "splitAmount.value", !0), a = n?.map((e) => Bi(e, "splitAmount.split", !0));
	if (a) {
		if (a.length > 8192) throw new S(`Cannot split amount: split would exceed ${le} outputs`);
		let e = j.sum(a);
		if (i.isZero() && e.isZero()) return a;
		let n = a.filter((e) => !e.isZero()), r = j.sum(n);
		if (r.greaterThan(i)) throw new S(`Split is greater than total amount: ${r.toString()} > ${i.toString()}`);
		if (n.some((e) => !zi(e, t))) throw new S("Provided amount preferences do not match the amounts of the mint keyset.");
		if (r.equals(i)) return n;
		a = n, i = i.subtract(r);
	} else a = [];
	let o = Ri(t, "desc");
	if (o.length === 0) throw new S("Cannot split amount, keyset is inactive or contains no keys");
	for (let e of o) {
		if (e.isZero()) continue;
		let t = i.divideBy(e), n = le - a.length;
		if (n <= 0 || t.greaterThan(n)) throw new S(`Cannot split amount: fill would exceed ${le} outputs`);
		let r = t.toNumber();
		for (let t = 0; t < r; t++) a.push(e);
		if (i = i.subtract(e.multiplyBy(t)), i.isZero()) break;
	}
	if (!i.isZero()) throw new S(`Unable to split remaining amount: ${i.toString()}`);
	return r && (a = a.sort((e, t) => r === "desc" ? t.compareTo(e) : e.compareTo(t))), a;
}
function Li(e, t = "desc") {
	return Ri(e, t);
}
function Ri(e, t) {
	let n = Object.keys(e).map((e) => j.from(e));
	return n.sort((e, n) => t === "desc" ? n.compareTo(e) : e.compareTo(n)), n;
}
function zi(e, t) {
	let n = Bi(e, "hasCorrespondingKey.amount", !0).toString();
	return Object.prototype.hasOwnProperty.call(t, n);
}
function Bi(e, t, n = !1) {
	let r = j.from(e);
	if (!n && r.isZero()) throw new S(`Amount must be positive: ${r.toString()}, op: ${t}`);
	return r;
}
function Vi(e) {
	return e ? BigInt(`0x${e}`) : 0n;
}
function Hi(e) {
	return e.toString(16).padStart(64, "0");
}
function U(e) {
	return typeof e == "string" && e.length % 2 == 0 && /^[a-f0-9]+$/i.test(e);
}
function Ui(e) {
	return e.some((e) => !U(e.id) || Gt(e.id));
}
function Wi(e, t) {
	return typeof t == "bigint" ? t.toString() : t;
}
function Gi(e) {
	return e.map((e) => {
		let t = { ...e };
		return t.id = t.id.slice(0, 16), t;
	});
}
function Ki(e, t) {
	let n = Y(e.proofs);
	if (Ui(n)) throw new S("Proofs contain a legacy keyset ID and cannot be encoded. Swap them at the mint first.");
	return qi({
		...e,
		proofs: n
	}, t?.removeDleq);
}
function qi(e, t) {
	let n = e.proofs;
	if (t && (n = ma(n)), Ui(n)) throw new S("can not encode to v4 token if proofs contain non-hex keyset id");
	return n = Gi(n), "cashuB" + $e(ft(Yi({
		...e,
		proofs: n
	})));
}
function Ji(e) {
	if (e.r == null) throw new S("Missing blinding factor in included DLEQ proof");
	return {
		e: i(e.e),
		s: i(e.s),
		r: i(e.r)
	};
}
function Yi(e) {
	let t = Object.create(null), n = e.mint;
	for (let n = 0; n < e.proofs.length; n++) {
		let r = e.proofs[n];
		t[r.id] ? t[r.id].push(r) : t[r.id] = [r];
	}
	let r = {
		m: n,
		u: e.unit || "sat",
		t: Object.keys(t).map((e) => ({
			i: i(e),
			p: t[e].map((e) => ({
				a: e.amount.toBigInt(),
				s: e.secret,
				c: i(e.C),
				...e.dleq && { d: Ji(e.dleq) },
				...e.p2pk_e && { pe: i(e.p2pk_e) },
				...e.witness && { w: typeof e.witness == "string" ? e.witness : JSON.stringify(e.witness) }
			}))
		}))
	};
	return e.memo && (r.d = e.memo), r;
}
function Xi(e, t) {
	if (!(e instanceof Uint8Array) || e.length === 0) throw new S(`Invalid token: ${t} must be a non-empty byte string`);
	return n(e);
}
function Zi(e, t) {
	if (typeof e != "string" || e.length === 0) throw new S(`Invalid token: ${t} must be a non-empty string`);
	return e;
}
function Qi(e) {
	return e != null && e.e != null && e.s != null && e.r != null;
}
function $i(e) {
	if (!e || !Array.isArray(e.t)) throw new S("Invalid token");
	let t = Zi(e.m, "mint"), n = e.u ? Zi(e.u, "unit") : "sat", r = e.d ? Zi(e.d, "memo") : void 0, i = [];
	return e.t.forEach((e) => {
		if (!e || !Array.isArray(e.p)) throw new S("Invalid token");
		e.p.forEach((t) => {
			let n = Xi(e.i, "keyset id"), r = Zi(t.s, "secret"), a = t.w ? Zi(t.w, "witness") : void 0;
			i.push({
				secret: r,
				C: Xi(t.c, "proof C"),
				amount: j.from(t.a),
				id: n,
				...Qi(t.d) && { dleq: {
					r: Xi(t.d.r, "dleq r"),
					s: Xi(t.d.s, "dleq s"),
					e: Xi(t.d.e, "dleq e")
				} },
				...t.pe && { p2pk_e: Xi(t.pe, "p2pk_e") },
				...a && { witness: a }
			});
		});
	}), {
		mint: t,
		proofs: i,
		unit: n,
		...r && { memo: r }
	};
}
function ea(e, t) {
	if (!Array.isArray(t)) throw new S("getDecodedToken requires keysetIds (the wallet keyset id list) as its second argument; see the v4 migration guide, or use wallet.decodeToken()");
	let n = na(ba(e));
	return n.proofs = ha(n.proofs, t), n;
}
function ta(e) {
	e = ba(e);
	let t = na(e);
	return {
		unit: t.unit || "sat",
		mint: t.mint,
		amount: J(t.proofs),
		...t.memo && { memo: t.memo },
		incompleteProofs: t.proofs.map((e) => {
			let { id: t, ...n } = e;
			return n;
		}),
		proofAmounts: t.proofs.map((e) => e.amount)
	};
}
function na(e) {
	let t = e.slice(0, 1), n = e.slice(1);
	if (t === "A") {
		let e = it(n);
		if (!e || !Array.isArray(e.token)) throw new S("Invalid token");
		if (e.token.length > 1) throw new S("Multi entry token are not supported");
		let t = e.token[0];
		if (!t || !Array.isArray(t.proofs)) throw new S("Invalid token");
		let r = t.proofs.map((e) => {
			let { dleq: t, ...n } = e;
			return {
				...n,
				amount: j.from(e.amount),
				...t?.r && t.s && t.e && { dleq: t }
			};
		}), i = {
			mint: t.mint,
			proofs: r,
			unit: e.unit || "sat"
		};
		return e.memo && (i.memo = e.memo), i;
	} else if (t === "B") return $i(wt(rt(n)));
	throw new S("Token version is not supported");
}
var ra = /^[a-z0-9_-]+$/, ia = 1, aa = (e) => `v${e + 1}`;
function W(e, t) {
	let n = U(e) && !Gt(e) ? Number.parseInt(e.slice(0, 2), 16) : -1;
	k(n > 1, `Keyset '${e}' is a ${aa(n)} keyset; this build of cashu-ts supports up to ${aa(1)}. Upgrade to use this keyset.`, t, {
		keysetId: e,
		versionByte: n,
		supported: 1
	});
}
function oa(t, r) {
	for (let e of Object.keys(t)) if (e.length > 20) throw new S("Invalid keyset denomination: exceeds 20 digits");
	let a = r?.unit ?? "sat", s = r?.expiry, c = r?.versionByte ?? 1, l = r?.input_fee_ppk;
	if (r?.isDeprecatedBase64 ?? !1) return Qe(e(o(Object.entries(t).sort(([e], [t]) => j.from(e).compareTo(t)).map(([, e]) => e).reduce((e, t) => e + t, "")))).slice(0, 12);
	switch (c) {
		case 0: return "00" + n(e(sa(...Object.entries(t).sort(([e], [t]) => j.from(e).compareTo(t)).map(([, e]) => i(e))))).slice(0, 14);
		case 1: {
			if (!a) throw new S("Cannot compute keyset ID version 01: unit is required.");
			if (!ra.test(a.toLowerCase())) throw new S(`Invalid keyset unit: ${a}`);
			let r = Object.entries(t).sort(([e], [t]) => j.from(e).compareTo(t)).map(([e, t]) => `${e}:${t}`).join(",");
			return r += `|unit:${a}`, l && (r += `|input_fee_ppk:${l}`), s && (r += `|final_expiry:${s}`), "01" + n(e(o(r)));
		}
		default: throw new S(`Unrecognized keyset ID version: ${c}`);
	}
}
function sa(...e) {
	let t = e.reduce((e, t) => e + t.length, 0), n = new Uint8Array(t), r = 0;
	for (let t of e) n.set(t, r), r += t.length;
	return n;
}
function ca(e) {
	return [...e].sort((e, t) => e.id.localeCompare(t.id));
}
function G(e) {
	return typeof e == "object" && !!e;
}
function K(e) {
	return G(e) && !Array.isArray(e);
}
function la(e, ...t) {
	for (let n of t) e[n] === void 0 && (e[n] = null);
}
function q(...e) {
	return e.map((e) => e.replace(/(^\/+|\/+$)/g, "")).join("/");
}
function ua(e) {
	let t;
	try {
		t = new URL(e);
	} catch (t) {
		throw new S(`Invalid mint URL: ${e}`, { cause: t });
	}
	if (t.protocol !== "http:" && t.protocol !== "https:") throw new S(`Invalid mint URL scheme: ${t.protocol}`);
	if (t.username || t.password) throw new S("Mint URL must not contain credentials");
	if (t.search || t.href.includes("?")) throw new S("Mint URL must not contain query parameters");
	if (t.hash || t.href.includes("#")) throw new S("Mint URL must not contain a fragment");
	if (/%[0-9a-f]{2}/i.test(t.pathname)) throw new S("Mint URL path must not contain percent-encoded characters");
	return t.href.replace(/\/+$/, "");
}
function J(e) {
	return j.sum(e.map((e) => e.amount));
}
function Y(e) {
	return e.map((e) => ({
		...e,
		amount: j.from(e.amount)
	}));
}
function da(e) {
	return (Array.isArray(e) ? e : [e]).map((e) => M.stringify(e));
}
function fa(e) {
	if (!Array.isArray(e)) {
		let t = M.parse(e);
		if (!Array.isArray(t)) throw TypeError("deserializeProofs: expected a JSON array of proofs");
		e = t;
	}
	return Y(e.map((e) => typeof e == "string" ? M.parse(e) : e));
}
function pa(e) {
	return Fi.fromEncodedRequest(e);
}
function ma(e) {
	return e.map((e) => {
		let { dleq: t, ...n } = e;
		return n;
	});
}
function ha(e, t) {
	let n = [...new Set(t.map((e) => e.toLowerCase()))], r = [];
	for (let t of e) {
		let e;
		try {
			e = i(t.id);
		} catch {
			r.push(t);
			continue;
		}
		if (e[0] === 0) r.push(t);
		else if (e[0] === 1) {
			if (!n.length) throw new S("A short keyset ID v2 was encountered, but got no keysets to map it to.");
			let e = t.id.toLowerCase(), i = n.filter((t) => e === t.slice(0, e.length));
			if (i.length > 1) throw new S(`Short keyset ID ${t.id} is ambiguous.`);
			if (i.length === 0) throw new S(`Couldn't map short keyset ID ${t.id} to any known keysets of the current Mint`);
			t.id = i[0], r.push(t);
		} else throw new S(`Unknown keyset ID version: ${e[0]}`);
	}
	return r;
}
function ga(e, t, n) {
	let r = n?.require ?? !0;
	if (!zi(e.amount, t.keys)) throw new S(Object.keys(t.keys).length === 0 ? `No keys loaded for keyset ${t.id}` : `Undefined key for amount ${e.amount.toString()} in keyset ${t.id}`);
	if (e?.dleq == null) return !r;
	if (e.dleq.r == null) return !1;
	let a = t.keys[e.amount.toString()];
	try {
		let t = {
			e: i(e.dleq.e),
			s: i(e.dleq.s),
			r: Vi(e.dleq.r)
		};
		return dr(new TextEncoder().encode(e.secret), t, P(e.C), P(a));
	} catch {
		return !1;
	}
}
function _a(e, t) {
	return ga(e, t, { require: !1 });
}
function va(e) {
	let t = new TextEncoder(), n = Y(e.proofs);
	if (Ui(n)) throw new S("Proofs contain a legacy keyset ID and cannot be encoded. Swap them at the mint first.");
	let r = ft(Yi({
		...e,
		proofs: n
	}));
	return sa(t.encode("craw"), t.encode("B"), r);
}
function ya(e, t) {
	if (e.length < 5 || [
		99,
		114,
		97,
		119,
		66
	].some((t, n) => e[n] !== t)) throw new S("not a valid binary token");
	let n = $i(wt(e.slice(5)));
	return Array.isArray(t) && (n.proofs = ha(n.proofs, t)), n;
}
function ba(e) {
	for (let t of [
		"web+cashu://",
		"cashu://",
		"cashu:"
	]) if (e.startsWith(t)) {
		e = e.slice(t.length);
		break;
	}
	return e.startsWith("cashu") && (e = e.slice(5)), e;
}
function xa(e) {
	try {
		return wa(e) !== null;
	} catch {
		return !1;
	}
}
var Sa = {
	m: 100000000n,
	u: 100000n,
	n: 100n
}, Ca = 100000000000n;
function wa(e) {
	if (typeof e != "string") throw new S("BOLT11 invoice must be a string");
	let t = e.toLowerCase(), n = t.lastIndexOf("1");
	if (!t.startsWith("ln") || n < 3 || n > 100 || n === t.length - 1) throw new S("Invalid BOLT11 invoice");
	let r = /^ln[a-z]+?(\d*)([munp]?)$/.exec(t.slice(0, n));
	if (!r) throw new S("Invalid BOLT11 invoice");
	let [, i, a] = r;
	if (i === "") return null;
	if (i.startsWith("0")) throw new S("Invalid BOLT11 amount");
	let o = BigInt(i);
	if (a === "") return o * Ca;
	if (a === "p") {
		if (o % 10n != 0n) throw new S("Invalid BOLT11 amount");
		return o / 10n;
	}
	return o * Sa[a];
}
function Ta(e) {
	if (typeof e != "string") throw new S("BOLT11 invoice must be a string");
	let t;
	try {
		({words: t} = d.decode(e.toLowerCase(), !1));
	} catch (e) {
		throw new S("Invalid BOLT11 invoice", { cause: e });
	}
	let r = t.length - 104;
	for (let e = 7; e + 3 <= r;) {
		let i = t[e], a = t[e + 1] << 5 | t[e + 2], o = e + 3 + a;
		if (o > r) break;
		if (i === 1 && a === 52) try {
			return n(d.fromWords(t.slice(e + 3, o)));
		} catch (e) {
			throw new S("Invalid BOLT11 payment hash", { cause: e });
		}
		e = o;
	}
	throw new S("BOLT11 invoice has no payment hash");
}
function Ea(t, r) {
	let a = Ta(t);
	return /^[0-9a-fA-F]{64}$/.test(r) && n(e(i(r))) === a;
}
function Da(e) {
	let t = e ?? 32;
	if (!Number.isSafeInteger(t) || t < 1) throw new S("chunkSize must be a positive safe integer");
	return t;
}
function Oa(e) {
	let t = e ?? 50;
	if (!Number.isFinite(t) || t < 0) throw new S("budgetMs must be a finite, non-negative number");
	return t;
}
async function ka() {
	let e = globalThis;
	if (typeof e.scheduler?.yield == "function") return e.scheduler.yield();
	if (typeof e.MessageChannel == "function") {
		await new Promise((t) => {
			let { port1: n, port2: r } = new e.MessageChannel();
			n.onmessage = () => {
				n.close(), t();
			}, r.postMessage(null);
		});
		return;
	}
	await new Promise((e) => setTimeout(e, 0));
}
async function Aa(e, t, n) {
	let r = Da(n?.chunkSize), i = Oa(n?.budgetMs), a = e.length, o = Array(a), s = () => {
		if (n?.signal?.aborted) throw new T("Operation aborted by caller");
	};
	s();
	let c = performance.now(), l = 0;
	for (let u = 0; u < a; u++) o[u] = t(e[u], u), l++, u + 1 !== a && (l >= r || performance.now() - c >= i) && (n?.onProgress?.(u + 1, a), await ka(), s(), c = performance.now(), l = 0);
	return n?.onProgress?.(a, a), o;
}
//#endregion
//#region src/utils/verifyProofs.ts
function ja(e) {
	return e instanceof Error ? e.message : String(e);
}
function Ma(e, t, n) {
	let r;
	try {
		r = {
			...e,
			amount: j.from(e.amount)
		};
	} catch (t) {
		return new S(`Invalid amount ${String(e.amount)} in keyset ${e.id}`, { cause: t });
	}
	let a;
	try {
		a = t(r.id);
	} catch (e) {
		return new S(ja(e), { cause: e });
	}
	if (Object.keys(a.keys).length === 0) return new S(`No keys loaded for keyset ${a.id}`);
	let o = a.keys[r.amount.toString()];
	if (!o) return new S(`Undefined key for amount ${r.amount.toString()} in keyset ${a.id}`);
	let s = () => new S(`${n ? "Token contains proofs with invalid or missing DLEQ" : "Token contains a proof with an invalid DLEQ"} (keyset ${r.id}, amount ${r.amount.toString()})`);
	if (r.dleq == null) return n ? s() : void 0;
	if (r.dleq.r == null) return s();
	try {
		return dr(new TextEncoder().encode(r.secret), {
			e: i(r.dleq.e),
			s: i(r.dleq.s),
			r: Vi(r.dleq.r)
		}, P(r.C), P(o)) ? void 0 : s();
	} catch {
		return s();
	}
}
async function Na(e, t, n) {
	if (e.length > 1e4) throw new S(`Token contains too many proofs: ${e.length}, maximum is ${ue}`);
	let r = n?.require ?? !1, i = await Aa(e, (e) => Ma(e, t, r), {
		chunkSize: Da(n?.chunkSize),
		budgetMs: Oa(n?.budgetMs),
		signal: n?.signal,
		onProgress: (e, t) => A(() => n?.onProgress?.(e, t), void 0)
	}), a = {
		valid: [],
		invalid: []
	};
	return i.forEach((t, n) => {
		t ? a.invalid.push({
			proof: e[n],
			error: t
		}) : a.valid.push(e[n]);
	}), a;
}
function Pa(e, t, n) {
	return Na(e, t, n);
}
function Fa(e, t, n) {
	return Na(e, t, n);
}
//#endregion
//#region src/utils/meltPreimage.ts
function Ia(e, t, n, r) {
	let i = e.payment_preimage ?? null;
	if (typeof i != "string") {
		i !== null && n.warn("Mint returned a payment_preimage that is not a string", { op: r }), e.payment_preimage = null;
		return;
	}
	e.payment_preimage = t === "bolt11" && typeof e.request == "string" ? La(e.request, i, n, r) : i;
}
function La(e, t, n, r) {
	let i;
	try {
		i = Ea(e, t);
	} catch (e) {
		return n.debug("Melt quote request is not a parseable BOLT11 invoice", {
			op: r,
			err: e
		}), t;
	}
	return i ? t : (n.warn("Mint returned a payment_preimage that does not match the invoice", { op: r }), null);
}
//#endregion
//#region src/utils/normalizeNumbers.ts
function X(e, t, n) {
	if (e == null) {
		if (arguments.length >= 3) return n;
		throw new S(`Invalid ${t}: missing value`);
	}
	try {
		return j.from(e).toNumber();
	} catch (e) {
		throw new S(`Invalid ${t}: ${e instanceof Error ? e.message : String(e)}`, { cause: e });
	}
}
function Ra(e) {
	return {
		...e,
		input_fee_ppk: X(e.input_fee_ppk, "keyset.input_fee_ppk", void 0),
		final_expiry: X(e.final_expiry, "keyset.final_expiry", void 0)
	};
}
function za(e) {
	return {
		...e,
		input_fee_ppk: X(e.input_fee_ppk, "keys.input_fee_ppk", void 0),
		final_expiry: X(e.final_expiry, "keys.final_expiry", void 0)
	};
}
//#endregion
//#region src/auth/OIDCAuth.ts
function Ba(e, t) {
	let n;
	try {
		n = new URL(e).protocol;
	} catch {
		throw new S(`OIDCAuth: ${t} is not a valid URL`);
	}
	if (n !== "https:" && n !== "http:") throw new S(`OIDCAuth: ${t} must be an http(s) URL`);
	return n;
}
var Va = class t {
	static fromMintInfo(e, n) {
		let r = e?.nuts?.["21"];
		if (!r?.openid_discovery) throw new S("OIDCAuth: mint does not advertise NUT-21 openid_discovery");
		let i = n?.clientId ?? r.client_id ?? "cashu-client";
		return new t(r.openid_discovery, {
			...n,
			clientId: i
		});
	}
	constructor(e, t) {
		this.tokenListeners = [], this.inflightRefreshes = /* @__PURE__ */ new Map(), this.discoveryUrl = e, this.secureDiscovery = e.startsWith("https:"), this.logger = t?.logger ?? O, this.clientId = t?.clientId ?? "cashu-client", this.scope = t?.scope ?? "openid", this.onTokens = t?.onTokens;
	}
	setClient(e) {
		this.clientId = e;
	}
	setScope(e) {
		this.scope = e ?? "openid";
	}
	addTokenListener(e) {
		this.tokenListeners = [...this.tokenListeners, e];
	}
	removeTokenListener(e) {
		this.tokenListeners = this.tokenListeners.filter((t) => t !== e);
	}
	async loadConfig() {
		if (this.config) return this.config;
		let e = await fetch(this.discoveryUrl, {
			method: "GET",
			headers: { Accept: "application/json" }
		}), t = await e.text(), n, r;
		try {
			n = t ? JSON.parse(t) : void 0;
		} catch (e) {
			r = e, this.logger.warn("OIDCAuth: bad discovery JSON", { err: e });
		}
		if (!e.ok || !n) throw new S("OIDCAuth: invalid discovery document", { cause: r });
		let i = n;
		if (typeof i.token_endpoint != "string" || i.token_endpoint.length === 0) throw new S("OIDCAuth: invalid discovery document, missing token_endpoint");
		return this.assertEndpoint(i.token_endpoint, "token_endpoint"), this.config = i, i;
	}
	assertEndpoint(e, t) {
		let n = Ba(e, t);
		this.secureDiscovery && n !== "https:" && this.logger.warn(`OIDCAuth: ${t} is not https although discovery is; v5 rejects this`, { label: t });
	}
	generatePKCE() {
		let t = $e(a(48));
		return {
			verifier: t,
			challenge: $e(e(o(t)))
		};
	}
	async buildAuthCodeUrl(e) {
		let t = await this.loadConfig(), n = e.scope ?? this.scope, r = e.codeChallengeMethod;
		r !== void 0 && r !== "S256" && this.logger.warn(`OIDCAuth: codeChallengeMethod '${typeof r == "string" ? r : typeof r}' is not supported by cashu-ts v5, which accepts S256 only.`);
		let i = new URLSearchParams({
			response_type: "code",
			client_id: this.clientId,
			redirect_uri: e.redirectUri,
			scope: n,
			code_challenge_method: e.codeChallengeMethod ?? "S256",
			code_challenge: e.codeChallenge
		});
		if (e.state && i.set("state", e.state), !t.authorization_endpoint) throw new S("OIDCAuth: discovery lacks authorization_endpoint");
		return this.assertEndpoint(t.authorization_endpoint, "authorization_endpoint"), `${t.authorization_endpoint}?${i.toString()}`;
	}
	async exchangeAuthCode(e) {
		let t = await this.loadConfig(), n = this.toForm({
			grant_type: "authorization_code",
			code: e.code,
			redirect_uri: e.redirectUri,
			client_id: this.clientId,
			code_verifier: e.codeVerifier
		}), r = await this.postFormStrict(t.token_endpoint, n);
		return this.handleTokens(r, "signin"), r;
	}
	async deviceStart() {
		let e = (await this.loadConfig()).device_authorization_endpoint;
		if (!e) throw new S("OIDCAuth: provider lacks device_authorization_endpoint");
		this.assertEndpoint(e, "device_authorization_endpoint");
		let t = this.toForm({
			client_id: this.clientId,
			scope: this.scope
		}), n = await this.postFormStrict(e, t);
		return this.assertEndpoint(n.verification_uri, "verification_uri"), n.verification_uri_complete !== void 0 && this.assertEndpoint(n.verification_uri_complete, "verification_uri_complete"), n;
	}
	async devicePoll(e, t = 5) {
		let n = await this.loadConfig(), r = Number(t), i = Number.isFinite(r) ? Math.max(1, r) : 5;
		for (;;) {
			await this.sleep(i * 1e3);
			let t = this.toForm({
				grant_type: "urn:ietf:params:oauth:grant-type:device_code",
				device_code: e,
				client_id: this.clientId
			}), r = await this.postFormLoose(n.token_endpoint, t);
			if (r.access_token) return this.handleTokens(r, "signin", this.tokenListeners), r;
			let a = (r.error ?? "").toString();
			if (a !== "authorization_pending") {
				if (a === "slow_down") {
					i = Math.max(i + 5, i * 2);
					continue;
				}
				throw new S(`OIDCAuth: ${r.error_description || a || "device authorization failed"}`);
			}
		}
	}
	async startDeviceAuth(e = 5) {
		let t = await this.deviceStart(), n = Number(t.interval), r = Number.isFinite(n) && n > 0 ? n : 1, i = Number(e), a = Math.max(r, Number.isFinite(i) ? i : 5), o = new AbortController(), s, c = new Promise((e) => {
			s = e;
		}), l = /* @__PURE__ */ new Set(), u = () => {
			if (o.signal.aborted) throw new S("OIDCAuth: device polling cancelled");
		}, d = async () => {
			let e = await this.loadConfig(), n = Math.max(1, a);
			for (;;) {
				u();
				let r;
				await this.sleep(n * 1e3, (e) => {
					r = e, l.add(e);
				}), l.delete(r), u();
				let i = this.toForm({
					grant_type: "urn:ietf:params:oauth:grant-type:device_code",
					device_code: t.device_code,
					client_id: this.clientId
				}), a = await this.postFormLoose(e.token_endpoint, i, o.signal);
				if (u(), a.access_token) return this.handleTokens(a, "signin"), a;
				let s = (a.error ?? "").toString();
				if (s !== "authorization_pending") {
					if (s === "slow_down") {
						n = Math.max(n + 5, n * 2);
						continue;
					}
					throw new S(`OIDCAuth: ${a.error_description || s || "device authorization failed"}`);
				}
			}
		}, f = () => {
			o.abort(), s();
			for (let e of l) e();
			l.clear();
		};
		return {
			...t,
			poll: () => Promise.race([d(), c.then(() => {
				throw new S("OIDCAuth: device polling cancelled");
			})]),
			cancel: f
		};
	}
	async refresh(e) {
		let t = this.clientId, n = this.tokenListeners, r = this.inflightRefreshes.get(e);
		if (r?.listeners === n && r.clientId === t) return r.promise;
		let i = (async () => {
			let r = await this.loadConfig(), i = this.toForm({
				grant_type: "refresh_token",
				refresh_token: e,
				client_id: t
			}), a = await this.postFormStrict(r.token_endpoint, i);
			return a.refresh_token || (a.refresh_token = e), this.handleTokens(a, "refresh", n), a;
		})();
		this.inflightRefreshes.set(e, {
			clientId: t,
			listeners: n,
			promise: i
		});
		try {
			return await i;
		} finally {
			this.inflightRefreshes.get(e)?.promise === i && this.inflightRefreshes.delete(e);
		}
	}
	async passwordGrant(e, t) {
		let n = await this.loadConfig(), r = this.toForm({
			grant_type: "password",
			client_id: this.clientId,
			username: e,
			password: t,
			scope: this.scope
		}), i = await this.postFormStrict(n.token_endpoint, r);
		return this.handleTokens(i, "signin"), i;
	}
	handleTokens(e, t, n = this.tokenListeners) {
		if (!e.access_token) throw new S(`OIDCAuth: ${e.error_description || e.error || "token response missing access_token"}`);
		queueMicrotask(() => A((e) => this.onTokens?.(e, t), e, this.logger, { where: "OIDCAuth.handleTokens" }));
		let r = new Set(this.tokenListeners);
		for (let i of n) r.has(i) && queueMicrotask(() => A((e) => i(e, t), e, this.logger, { where: "OIDCAuth.handleTokens.listener" }));
	}
	toForm(e) {
		let t = (e) => encodeURIComponent(e).replace(/%20/g, "+");
		return Object.entries(e).map(([e, n]) => `${t(e)}=${t(n)}`).join("&");
	}
	async postFormStrict(e, t) {
		try {
			let n = await fetch(e, {
				method: "POST",
				headers: {
					"Content-Type": "application/x-www-form-urlencoded",
					Accept: "application/json"
				},
				body: t,
				redirect: "error"
			}), r = await n.text(), i, a;
			try {
				i = r ? JSON.parse(r) : void 0;
			} catch (e) {
				a = e, this.logger.warn("OIDCAuth: bad JSON (strict)", { err: e });
			}
			if (!n.ok) {
				let e = i ?? {};
				throw new S(`OIDCAuth: ${e.error_description || e.error || `HTTP ${n.status}`}`, { cause: a });
			}
			return this.logger.debug("OIDCAuth Response", { status: n.status }), i ?? {};
		} catch (e) {
			throw this.logger.error("OIDCAuth: postFormStrict failed", { err: e }), e;
		}
	}
	async postFormLoose(e, t, n) {
		try {
			let r = await fetch(e, {
				method: "POST",
				signal: n,
				headers: {
					"Content-Type": "application/x-www-form-urlencoded",
					Accept: "application/json"
				},
				body: t,
				redirect: "error"
			}), i = await r.text(), a;
			try {
				a = i ? JSON.parse(i) : void 0;
			} catch (e) {
				this.logger.warn("OIDCAuth: bad JSON (loose)", { err: e });
			}
			return this.logger.debug("OIDCAuth Response", { status: r.status }), a ?? {};
		} catch (e) {
			return this.logger.error("OIDCAuth: postFormLoose network error", { err: e }), {
				error: "network_error",
				error_description: String(e)
			};
		}
	}
	sleep(e, t) {
		let n = Math.min(e, me);
		return new Promise((e) => {
			let r = setTimeout(e, n);
			t?.(() => {
				clearTimeout(r), e();
			});
		});
	}
}, Z = class e {
	constructor(t, n) {
		let r = n ?? O;
		this._mintInfo = e.normalizeInfo(t, r), this._maxArrayLength = e.resolveMaxArrayLength(t.max_array_length, r);
		let i = this.toEndpoints(this._mintInfo?.nuts?.[22]?.protected_endpoints, r);
		this._protected22 = this.buildIndex(i);
		let a = this.toEndpoints(this._mintInfo?.nuts?.[21]?.protected_endpoints, r);
		this._protected21 = this.buildIndex(a);
	}
	static normalizeInfo(t, n = O) {
		if (!K(t) || !K(t.nuts)) throw n.error("MintInfo: malformed info response", { op: "normalizeInfo" }), new S("Invalid response from mint");
		return {
			...t,
			...Array.isArray(t.contact) ? { contact: e.capList(t.contact, "contact", n) } : {},
			...Array.isArray(t.urls) ? { urls: e.capList(t.urls, "urls", n) } : {},
			nuts: {
				...t.nuts,
				...t.nuts[4] ? { 4: e.normalizeSwapSection(t.nuts[4], "nuts.4", n) } : {},
				...t.nuts[5] ? { 5: e.normalizeSwapSection(t.nuts[5], "nuts.5", n) } : {},
				...t.nuts[15] ? { 15: e.normalizeNut15(t.nuts[15], n) } : {},
				...t.nuts[17] ? { 17: e.normalizeNut17(t.nuts[17], n) } : {},
				...t.nuts[19] ? { 19: e.normalizeNut19(t.nuts[19], n) } : {},
				...t.nuts[21] ? { 21: e.normalizeNut21(t.nuts[21], n) } : {},
				...t.nuts[22] ? { 22: e.normalizeNut22(t.nuts[22], n) } : {},
				...t.nuts[29] ? { 29: e.normalizeNut29(t.nuts[29], n) } : {}
			}
		};
	}
	static dropSection(e, t) {
		t.warn(`MintInfo: ${e} is malformed and was omitted`);
	}
	static normalizeSwapSection(t, n, r) {
		if (!K(t)) throw r.error("MintInfo: malformed info response", { op: n }), new S("Invalid response from mint");
		return {
			...t,
			methods: e.normalizeSwapMethods(t.methods, r)
		};
	}
	static normalizeSwapMethods(t, n) {
		return Array.isArray(t) ? e.capList(t, "nuts.4/5.methods", n).filter((e) => K(e)).map((e) => {
			let t = { ...e };
			return la(t, "min_amount", "max_amount"), t;
		}) : (n.warn("MintInfo: nuts.4/5.methods is malformed and was omitted"), []);
	}
	static resolveMaxArrayLength(e, t) {
		if (e == null) return 500;
		let n;
		try {
			n = X(e, "max_array_length");
		} catch {
			return t.warn("MintInfo: max_array_length is malformed, defaulting to internal default", { value: e }), 500;
		}
		if (n < 1 || n > 1e4) {
			let e = Math.min(Math.max(n, 1), ue);
			return t.warn("MintInfo: max_array_length is out of range and was clamped", {
				advertised: n,
				clampedTo: e
			}), e;
		}
		return n;
	}
	static normalizeNut15(t, n) {
		return K(t) ? Array.isArray(t.methods) ? {
			...t,
			methods: e.capList(t.methods, "nuts.15.methods", n)
		} : t : e.dropSection("nuts.15", n);
	}
	static normalizeNut17(t, n) {
		return K(t) ? Array.isArray(t.supported) ? {
			...t,
			supported: e.capList(t.supported, "nuts.17.supported", n)
		} : t : e.dropSection("nuts.17", n);
	}
	static normalizeNut19(t, n) {
		return K(t) ? {
			...t,
			...Array.isArray(t.cached_endpoints) ? { cached_endpoints: e.capList(t.cached_endpoints, "nuts.19.cached_endpoints", n) } : {},
			ttl: X(t.ttl, "nuts.19.ttl", null)
		} : e.dropSection("nuts.19", n);
	}
	static normalizeNut21(t, n) {
		return K(t) ? Array.isArray(t.protected_endpoints) ? {
			...t,
			protected_endpoints: e.capList(t.protected_endpoints, "nuts.21.protected_endpoints", n)
		} : t : e.dropSection("nuts.21", n);
	}
	static normalizeNut22(t, n) {
		if (!K(t)) return e.dropSection("nuts.22", n);
		let r = 100;
		try {
			r = X(t.bat_max_mint, "nuts.22.bat_max_mint", 100);
		} catch {
			n.warn("MintInfo: nuts.22.bat_max_mint is malformed, defaulting to internal cap", { value: t.bat_max_mint });
		}
		return r > 100 && (n.warn("MintInfo: nuts.22.bat_max_mint exceeds internal cap and was clamped", {
			advertised: r,
			clampedTo: 100
		}), r = 100), {
			...t,
			...Array.isArray(t.protected_endpoints) ? { protected_endpoints: e.capList(t.protected_endpoints, "nuts.22.protected_endpoints", n) } : {},
			bat_max_mint: r
		};
	}
	static normalizeNut29(t, n) {
		if (!K(t)) return e.dropSection("nuts.29", n);
		let r = 100;
		try {
			r = X(t.max_batch_size, "nuts.29.max_batch_size", 100);
		} catch {
			n.warn("MintInfo: nuts.29.max_batch_size is malformed, defaulting to internal cap", { value: t.max_batch_size });
		}
		return r > 100 && (n.warn("MintInfo: nuts.29.max_batch_size exceeds internal cap and was clamped", {
			advertised: r,
			clampedTo: 100
		}), r = 100), {
			methods: Array.isArray(t.methods) ? e.capList(t.methods, "nuts.29.methods", n) : t.methods,
			max_batch_size: r
		};
	}
	isSupported(e) {
		switch (e) {
			case 4:
			case 5: return this.checkMintMelt(e);
			case 7:
			case 8:
			case 9:
			case 10:
			case 11:
			case 12:
			case 14:
			case 20: return this.checkGenericNut(e);
			case 17: return this.checkNut17();
			case 15: return this.checkNut15();
			case 19: return this.checkNut19();
			case 29: return this.checkNut29();
			default: throw new S("nut is not supported by cashu-ts");
		}
	}
	requiresBlindAuthToken(e, t) {
		return this.matchesProtected(this._protected22, e, t);
	}
	requiresClearAuthToken(e, t) {
		return this.matchesProtected(this._protected21, e, t);
	}
	matchesProtected(e, t, n) {
		if (!e) return !1;
		let r = e.exact[t], i = e.prefix[t];
		if (!r || !i) return !1;
		if (e.exact[t].has(n)) return !0;
		for (let r of e.prefix[t]) if (n.startsWith(r)) return !0;
		return !1;
	}
	checkGenericNut(e) {
		return { supported: this._mintInfo.nuts[e]?.supported === !0 };
	}
	checkMintMelt(t) {
		let n = this._mintInfo.nuts[t];
		return n && n.methods.length > 0 && !n.disabled ? {
			disabled: !1,
			params: e.snapshot(n.methods)
		} : {
			disabled: !0,
			params: e.snapshot(n?.methods ?? [])
		};
	}
	checkNut17() {
		return this._mintInfo.nuts[17] && this._mintInfo.nuts[17].supported.length > 0 ? {
			supported: !0,
			params: e.snapshot(this._mintInfo.nuts[17].supported)
		} : { supported: !1 };
	}
	checkNut15() {
		return this._mintInfo.nuts[15] && this._mintInfo.nuts[15].methods.length > 0 ? {
			supported: !0,
			params: e.snapshot(this._mintInfo.nuts[15].methods)
		} : { supported: !1 };
	}
	checkNut19() {
		let t = this._mintInfo.nuts?.[19];
		if (t && (t?.cached_endpoints?.length || 0) > 0) {
			let n = X(t.ttl, "nuts.19.ttl", null);
			return {
				supported: !0,
				params: {
					ttl: n === null ? Infinity : Math.max(n, 0) * 1e3,
					cached_endpoints: e.snapshot(t.cached_endpoints)
				}
			};
		}
		return { supported: !1 };
	}
	checkNut29() {
		let t = this._mintInfo.nuts?.[29];
		return t ? {
			supported: !0,
			params: e.snapshot(t)
		} : { supported: !1 };
	}
	static capList(e, t, n) {
		return e.length <= 1024 ? e : (n.warn(`MintInfo: ${t} exceeds internal cap and was truncated`, {
			advertised: e.length,
			cap: de
		}), e.slice(0, de));
	}
	toEndpoints(t, n) {
		if (!Array.isArray(t)) return [];
		let r = e.capList(t, "nuts.21/22.protected_endpoints", n), i = [];
		for (let e of r) if (e && typeof e == "object") {
			let t = e, n = t.method, r = t.path;
			if (typeof n == "string" && typeof r == "string") {
				let e = n.toUpperCase();
				(e === "GET" || e === "POST") && i.push({
					method: e,
					path: r
				});
			}
		}
		return i;
	}
	buildIndex(e) {
		if (!e?.length) return;
		let t = {
			GET: /* @__PURE__ */ new Set(),
			POST: /* @__PURE__ */ new Set()
		}, n = {
			GET: [],
			POST: []
		};
		for (let r of e) {
			let e = r.path;
			if (e.startsWith("^") && (e = e.slice(1)), e.endsWith("$") && (e = e.slice(0, -1)), e.endsWith(".*")) {
				n[r.method].push(e.slice(0, -2));
				continue;
			}
			if (e.endsWith("*")) {
				n[r.method].push(e.slice(0, -1));
				continue;
			}
			t[r.method].add(e);
		}
		return n.GET.sort((e, t) => t.length - e.length), n.POST.sort((e, t) => t.length - e.length), {
			exact: t,
			prefix: n
		};
	}
	static snapshot(t, n = 0) {
		if (typeof t != "object" || !t || t instanceof j) return t;
		if (n > 64) throw new S("Mint info nesting exceeds 64 levels");
		if (Array.isArray(t)) return t.map((t) => e.snapshot(t, n + 1));
		let r = {};
		for (let [i, a] of Object.entries(t)) r[i] = e.snapshot(a, n + 1);
		return r;
	}
	get cache() {
		return e.snapshot(this._mintInfo);
	}
	get contact() {
		return e.snapshot(this._mintInfo.contact);
	}
	get description() {
		return this._mintInfo.description;
	}
	get description_long() {
		return this._mintInfo.description_long;
	}
	get name() {
		return this._mintInfo.name;
	}
	get pubkey() {
		return this._mintInfo.pubkey;
	}
	get nuts() {
		return e.snapshot(this._mintInfo.nuts);
	}
	get version() {
		return this._mintInfo.version;
	}
	get motd() {
		return this._mintInfo.motd;
	}
	get icon_url() {
		return this._mintInfo.icon_url;
	}
	get urls() {
		return e.snapshot(this._mintInfo.urls);
	}
	get time() {
		return this._mintInfo.time;
	}
	get tos_url() {
		return this._mintInfo.tos_url;
	}
	get maxArrayLength() {
		return this._maxArrayLength;
	}
	supportsNut04Description(e, t) {
		return this._mintInfo.nuts[4]?.methods.some((n) => n.method === e && (t ? n.unit === t : !0) && (n.options?.description === !0 || n.description === !0));
	}
	supportsMintMeltMethod(e, t, n) {
		let { disabled: r, params: i } = this.isSupported(e === "mint" ? 4 : 5);
		return r ? !1 : i.some((e) => e.method === t && e.unit === n);
	}
	supportsAmountless(e = "bolt11", t = "sat") {
		let n = this._mintInfo?.nuts?.[5]?.methods ?? [];
		return Array.isArray(n) ? n.some((n) => n.method === e && n.unit === t && n.options?.amountless === !0) : !1;
	}
}, Ha = {
	UNPAID: "UNPAID",
	PAID: "PAID",
	ISSUED: "ISSUED"
}, Ua = {
	UNPAID: "UNPAID",
	PENDING: "PENDING",
	PAID: "PAID"
}, Wa = {
	UNSPENT: "UNSPENT",
	PENDING: "PENDING",
	SPENT: "SPENT"
};
//#endregion
//#region src/transport/request.ts
function Ga(e) {
	return e.window !== void 0 && e.window.document !== void 0 ? !0 : e.WorkerGlobalScope !== void 0 && e.self !== void 0 && e.self instanceof e.WorkerGlobalScope;
}
var Ka = Ga(globalThis);
function qa(e, t, n = Ka) {
	return vo({
		Accept: "application/json, text/plain, */*",
		...e ? { "Content-Type": "application/json" } : void 0,
		...n ? void 0 : { "User-Agent": "Mozilla/5.0" }
	}, t);
}
function Ja(e, t) {
	return e instanceof Error ? e.message : t;
}
function Ya(e, t, n) {
	let r = e;
	for (;;) {
		let e = r.cause;
		if (!(e instanceof Error) || e === r) break;
		r = e;
	}
	let i = "";
	if (r instanceof Error && r !== e && r.message) {
		let e = r.code;
		i = ` (${r.message}${typeof e == "string" ? `, ${e}` : ""})`;
	}
	return `${Ja(e, t)}${i} at ${Xa(n)}`;
}
function Xa(e) {
	try {
		return new URL(e).origin;
	} catch {
		return e;
	}
}
function Za(e, t, n, r) {
	if (t?.signal.aborted) return new E(`Request timed out after ${n}ms`, { cause: e });
	if (r?.aborted) return new T(Ja(e, "Request aborted by caller"));
}
async function Qa(e, t) {
	if (t?.aborted) throw new S("response body read aborted");
	let n = e.text();
	if (!t) return n;
	n.catch(() => void 0);
	let r, i = new Promise((e, n) => {
		r = () => n(new S("response body read aborted")), t.aborted ? r() : t.addEventListener("abort", r, { once: !0 });
	});
	i.catch(() => void 0);
	try {
		return await Promise.race([n, i]);
	} finally {
		r && t.removeEventListener("abort", r);
	}
}
function $a(e) {
	if (e === null) return;
	let t = e.trim();
	if (t !== "") {
		if (/^\d+$/.test(t)) return Math.max(Number(t) * 1e3, 0);
		if (/[a-zA-Z]/.test(t)) {
			let e = new Date(t).getTime();
			if (!Number.isNaN(e)) return Math.max(e - Date.now(), 0);
		}
	}
}
var eo = {
	endpoint: !0,
	requestBody: !0,
	logger: !0,
	ttl: !0,
	cached_endpoints: !0,
	requestTimeout: !0,
	onResponseMeta: !0
}, to = {}, no = O;
function ro(e) {
	to = e;
}
function io(e) {
	no = e;
}
var ao = 9, oo = 1e3, so = 3e5, co = 100, lo = ["blind-auth", "clear-auth"];
function uo(e) {
	return e instanceof T || e instanceof E ? !1 : e instanceof w ? !0 : e instanceof C && e.status >= 500;
}
function fo(e, t) {
	return t ? new Promise((n, r) => {
		if (t.aborted) {
			r(new T("Request aborted by caller"));
			return;
		}
		let i = () => {
			clearTimeout(a), t.removeEventListener("abort", i), r(new T("Request aborted by caller"));
		};
		t.addEventListener("abort", i, { once: !0 });
		let a = setTimeout(() => {
			t.removeEventListener("abort", i), n();
		}, e);
	}) : new Promise((t) => setTimeout(t, e));
}
function po(e) {
	try {
		return new URL(e).pathname;
	} catch {
		return e.startsWith("/") ? e.split(/[?#]/, 1)[0] : void 0;
	}
}
function mo(e, t) {
	return e === t ? !0 : e.endsWith(t);
}
async function ho(e) {
	let { ttl: t, cached_endpoints: n, endpoint: r } = e, i = e.logger ?? no;
	if (Object.keys(e.headers ?? {}).some((e) => e.toLowerCase() === "blind-auth")) return await go(e);
	let a = po(r), o = e.method?.toUpperCase() ?? "GET";
	if (!(a !== void 0 && n?.some((e) => mo(a, e.path) && e.method === o) && t)) return await go(e);
	let s = 0, c = Date.now(), l = async () => {
		try {
			return await go(e);
		} catch (n) {
			if (uo(n)) {
				let r = Date.now() - c;
				if (s < ao && (!t || r < t)) {
					let a = Math.min(2 ** s * co, oo) / 2, o = a + Math.random() * a;
					if (r + o > t) throw i.warn(`Network Error: request abandoned after ${s} retries`, {
						e: n,
						retries: s
					}), n;
					return s++, i.info(`Network Error: attempting retry ${s} in ${o}ms`, {
						e: n,
						retries: s,
						delay: o
					}), await fo(o, e.signal), l();
				}
			}
			throw i.debug("Request failed and could not be retried", { e: n }), n;
		}
	};
	return l();
}
async function go(e) {
	let { endpoint: t, requestBody: n, headers: r, requestTimeout: i, onResponseMeta: a, cached_endpoints: o, ttl: s, logger: c, ...l } = e, u = c ?? no, d = n ? M.stringify(n) : void 0, f = qa(d, r), p = Object.keys(f).some((e) => lo.includes(e.toLowerCase())), m = e.signal ?? void 0;
	if (m?.aborted) throw new T("Request aborted by caller");
	let h = i !== void 0 && Number.isFinite(i) ? new AbortController() : void 0, g = m, _, v;
	if (h) if (_ = setTimeout(() => h.abort(), i), !m) g = h.signal;
	else {
		let e = new AbortController(), t = () => e.abort();
		m.addEventListener("abort", t, { once: !0 }), h.signal.addEventListener("abort", t, { once: !0 }), v = () => {
			m.removeEventListener("abort", t), h.signal.removeEventListener("abort", t);
		}, g = e.signal;
	}
	try {
		let e;
		try {
			e = await fetch(t, {
				body: d,
				headers: f,
				cache: "no-store",
				credentials: "omit",
				referrer: "",
				referrerPolicy: "no-referrer",
				...d === void 0 ? void 0 : { redirect: "error" },
				...l,
				...p ? { redirect: "error" } : void 0,
				signal: g
			});
		} catch (e) {
			let n = !!h?.signal.aborted, r = !!m?.aborted;
			throw n ? new w(`Request timed out after ${i}ms at ${Xa(t)}`, { cause: e }) : r ? new T(Ja(e, "Request aborted by caller")) : e instanceof Error && (e.name === "AbortError" || e.name === "TimeoutError") ? new w(Ya(e, e.message, t), { cause: e }) : new w(Ya(e, "Network request failed", t), { cause: e });
		}
		let n = $a(e.headers.get("Retry-After"));
		if (a && e.headers && A(a, {
			endpoint: t,
			status: e.status,
			retryAfterMs: n,
			rateLimit: e.headers.get("RateLimit") ?? void 0,
			rateLimitPolicy: e.headers.get("RateLimit-Policy") ?? void 0,
			headers: e.headers
		}, u, {
			op: "request.onResponseMeta",
			status: e.status,
			endpoint: Xa(t)
		}), !e.ok) {
			if (e.status === 429) {
				let t = e.body;
				throw t && typeof t.cancel == "function" && t.cancel().catch(() => void 0), new ie("429 Too Many Requests", n);
			}
			let t, r;
			try {
				t = _o(await Qa(e, g));
			} catch (e) {
				let n = Za(e, h, i, m);
				if (n) throw n;
				r = e, t = { error: "bad response" };
			}
			if (e.status === 400 && "code" in t && typeof t.code == "number" && "detail" in t && typeof t.detail == "string") throw new ae(t.code, t.detail);
			let a = "HTTP request failed";
			throw "error" in t && typeof t.error == "string" ? a = t.error : "detail" in t && typeof t.detail == "string" && (a = t.detail), new C(a, e.status, { cause: r });
		}
		let r;
		try {
			r = await Qa(e, g);
		} catch (t) {
			throw Za(t, h, i, m) || (u.error("Failed to read HTTP response", { err: t }), new C("bad response", e.status, { cause: t }));
		}
		try {
			if (!r) throw new S("Empty response body");
			return M.parse(r, void 0, { strict: !0 });
		} catch (t) {
			throw u.error("Failed to parse HTTP response", { err: t }), new C("bad response", e.status, { cause: t });
		}
	} finally {
		clearTimeout(_), v?.();
	}
}
function _o(e) {
	if (!e) return { detail: "bad response" };
	let t;
	try {
		t = M.parse(e, void 0, { strict: !0 });
	} catch {
		return { detail: e };
	}
	return typeof t == "object" && t && ("detail" in t || "code" in t || "error" in t) ? t : { detail: t };
}
function vo(e, t) {
	let n = /* @__PURE__ */ new Map();
	for (let r of [e, t]) for (let [e, t] of Object.entries(r ?? {})) n.set(e.toLowerCase(), [e, t]);
	return Object.fromEntries(n.values());
}
async function yo(e) {
	let t = e.onResponseMeta, n = to.onResponseMeta, r = {
		...e,
		...to
	};
	for (let t of Object.keys(eo)) e[t] !== void 0 && (r[t] = e[t]);
	if (r.requestTimeout === void 0 && (r.requestTimeout = so), r.headers = vo(to.headers, e.headers), t && n && t !== n) {
		let i = r.logger ?? no;
		r.onResponseMeta = (r) => {
			A(t, r, i, {
				op: "request.onResponseMeta",
				scope: "per-request",
				endpoint: Xa(e.endpoint)
			}), A(n, r, i, {
				op: "request.onResponseMeta",
				scope: "global",
				endpoint: Xa(e.endpoint)
			});
		};
	}
	return await ho(r);
}
//#endregion
//#region src/transport/ws.ts
var bo;
typeof WebSocket < "u" && (bo = WebSocket);
function xo(e) {
	bo = e;
}
function So() {
	if (bo === void 0) throw new S("WebSocket implementation not initialized");
	return bo;
}
//#endregion
//#region src/transport/WSConnection.ts
var Co = 1006, wo = class {
	constructor(e) {
		this.next = null, this.value = e;
	}
}, To = class {
	constructor() {
		this._first = null, this._last = null, this.size = 0;
	}
	enqueue(e) {
		let t = new wo(e);
		return this._last ? this._last.next = t : this._first = t, this._last = t, this.size++, !0;
	}
	dequeue() {
		if (!this._first) return null;
		let e = this._first;
		return this._first = e.next, this._first || (this._last = null), this.size--, e.value;
	}
}, Eo = class {
	constructor(e, t) {
		this.subListeners = {}, this.rpcListeners = {}, this.rpcId = 0, this.onCloseCallbacks = [], this._WS = So(), this.url = new URL(e), this.messageQueue = new To(), this._logger = t ?? O;
	}
	setLogger(e) {
		this._logger = e;
	}
	connect(e = 1e4) {
		return this.connectionPromise || (this.connectionPromise = new Promise((t, n) => {
			let r = !1, i = !1, a = null, o = (e) => {
				i || (i = !0, a && clearTimeout(a), this.abandonConnect = void 0, e());
			};
			this.abandonConnect = (e) => o(() => n(e));
			let s = (e) => {
				if (this.ws) {
					try {
						this.ws.onopen = null, this.ws.onerror = null, this.ws.onmessage = null, this.ws.onclose = null;
					} catch {}
					try {
						this.ws.close();
					} catch {}
					this.ws = void 0, this.stopMessageHandling(e);
				}
			}, c = (e) => {
				this.connectionPromise = void 0;
				let t = e instanceof Error ? e : new S(String(e), { cause: e });
				s(t), this.failPendingRpc(t), o(() => n(t));
			}, l;
			try {
				l = new this._WS(this.url.toString()), this.ws = l;
			} catch (e) {
				c(e);
				return;
			}
			let u = () => this.ws === l;
			a = setTimeout(() => {
				c(new S(`WebSocket connect timeout after ${e}ms`));
			}, e), l.onopen = () => {
				u() && (r = !0, o(t));
			}, l.onerror = (e) => {
				if (u()) {
					if (!r) {
						c(new S("Failed to open WebSocket"));
						return;
					}
					this._logger.error("WebSocket error after open", { ev: e });
				}
			}, l.onmessage = (e) => {
				if (u()) {
					if (this.messageQueue.size >= 1e4) {
						this._logger.error("WebSocket message queue exceeded its bound, closing connection", { size: this.messageQueue.size });
						let e = new S("WebSocket message queue exceeded its bound");
						c(e);
						let t = {
							code: Co,
							reason: e.message,
							wasClean: !1
						};
						this.onCloseCallbacks.forEach((e) => A(e, t, this._logger));
						return;
					}
					this.messageQueue.enqueue(e.data), this.handlingInterval || (this.handlingInterval = setInterval(this.handleNextMessage.bind(this), 0));
				}
			}, l.onclose = (e) => {
				if (this.ws && this.ws !== l) return;
				if (this.connectionPromise = void 0, !r) {
					let t = e?.reason ? `, ${e.reason}` : "";
					c(new S(`WebSocket closed before open (code ${e?.code ?? 0}${t})`));
					return;
				}
				let t = e?.reason ? `, ${e.reason}` : "", n = e?.code ?? 0, i = typeof e.wasClean == "boolean" ? e.wasClean : !0, a = new S(`WebSocket closed (code ${n}${t})`);
				this.stopMessageHandling(a), !i || n !== 1e3 && n !== 1001 ? this.failPendingRpc(a) : this.rpcListeners = {}, this.onCloseCallbacks.forEach((t) => A(t, e, this._logger));
			};
		})), this.connectionPromise;
	}
	sendRequest(e, t) {
		if (this.ws?.readyState !== this._WS.OPEN) {
			if (e === "unsubscribe") return;
			throw this._logger.error("Attempted sendRequest, but socket was not open"), new S("Socket not open");
		}
		let n = this.rpcId;
		this.rpcId++, this.sendRpcMessage(e, t, n);
	}
	addSubListener(e, t, n) {
		(this.subListeners[e] = this.subListeners[e] || []).push({
			callback: t,
			errorCallback: n
		});
	}
	stopMessageHandling(e) {
		for (this.handlingInterval && (clearInterval(this.handlingInterval), this.handlingInterval = void 0); this.messageQueue.size > 0;) this.messageQueue.dequeue();
		let t = this.subListeners;
		this.subListeners = {};
		for (let n of Object.values(t)) for (let { errorCallback: t } of n) A(t, e, this._logger);
	}
	failPendingRpc(e) {
		let t = this.rpcListeners;
		this.rpcListeners = {};
		for (let n of Object.keys(t)) A(t[n].errorCallback, e, this._logger);
	}
	sendRpcMessage(e, t, n) {
		if (this.ws?.readyState !== this._WS.OPEN) throw new S("Socket not open");
		let r = JSON.stringify({
			jsonrpc: "2.0",
			method: e,
			params: t,
			id: n
		});
		try {
			this.ws.send(r);
		} catch (e) {
			this._logger.error("WebSocket send failed", { e }), this.connectionPromise = void 0;
			try {
				this.ws.close();
			} catch {}
			this.ws = void 0;
			let t = e instanceof Error ? e : new S(String(e), { cause: e });
			throw this.stopMessageHandling(t), this.failPendingRpc(t), t;
		}
	}
	addRpcListener(e, t, n) {
		this.rpcListeners[n] = {
			callback: e,
			errorCallback: t
		};
	}
	removeRpcListener(e) {
		delete this.rpcListeners[e];
	}
	removeListener(e, t) {
		if (this.subListeners[e]) {
			if (this.subListeners[e].length === 1) {
				delete this.subListeners[e];
				return;
			}
			this.subListeners[e] = this.subListeners[e].filter((e) => e.callback !== t);
		}
	}
	async ensureConnection(e) {
		this.ws?.readyState !== this._WS.OPEN && await this.connect(e);
	}
	handleNextMessage() {
		for (; this.messageQueue.size > 0;) {
			let e = this.messageQueue.dequeue();
			try {
				let t = M.parse(e, void 0, { strict: !0 });
				if ("result" in t && t.id != null) this.rpcListeners[t.id] && (this.rpcListeners[t.id].callback(), this.removeRpcListener(t.id));
				else if ("error" in t && t.id != null) {
					if (this.rpcListeners[t.id]) {
						let { errorCallback: e } = this.rpcListeners[t.id];
						A(e, new S(t.error.message), this._logger), this.removeRpcListener(t.id);
					}
				} else if ("method" in t && !("id" in t)) {
					let e = t.params?.subId;
					if (!e) continue;
					if (this.subListeners[e]?.length > 0) {
						let n = t;
						this.subListeners[e].forEach(({ callback: e }) => {
							try {
								let t = e(n.params?.payload);
								t && typeof t.then == "function" && Promise.resolve(t).catch((e) => {
									this._logger.error("Subscription handler threw", { e });
								});
							} catch (e) {
								this._logger.error("Subscription handler threw", { e });
							}
						});
					}
				}
			} catch (e) {
				this._logger.error("Error doing handleNextMessage", { e });
			}
		}
		this.handlingInterval && (clearInterval(this.handlingInterval), this.handlingInterval = void 0);
	}
	createSubscription(e, t, n) {
		if (this.ws?.readyState !== this._WS.OPEN) throw this._logger.error("Attempted createSubscription, but socket was not open"), new S("Socket is not open");
		let r = (Math.random() + 1).toString(36).substring(7), i = this.rpcId;
		this.addRpcListener(() => {
			this.addSubListener(r, t, n);
		}, n, i);
		try {
			this.sendRequest("subscribe", {
				...e,
				subId: r
			});
		} catch (e) {
			throw this.removeRpcListener(i), e;
		}
		return r;
	}
	cancelSubscription(e, t, n) {
		if (this.removeListener(e, t), this.ws?.readyState !== this._WS.OPEN) {
			this._logger.info("Socket not open, removed listener locally {subId}", { subId: e });
			return;
		}
		let r = this.rpcId;
		this.rpcId++, this.addRpcListener(() => {
			this._logger.info("Unsubscribed {subId}", { subId: e });
		}, n || ((e) => this._logger.info("Unsubscribe failed", { e })), r);
		try {
			this.sendRpcMessage("unsubscribe", { subId: e }, r);
		} catch (e) {
			throw this.removeRpcListener(r), e;
		}
	}
	get activeSubscriptions() {
		return Object.keys(this.subListeners);
	}
	close() {
		let e = new S("WebSocket closed");
		if (this.abandonConnect?.(e), this.ws) {
			try {
				this.ws.close();
			} catch {}
			this.ws = void 0;
		}
		this.connectionPromise = void 0, this.failPendingRpc(e), this.stopMessageHandling(e);
	}
	onClose(e) {
		this.onCloseCallbacks.push(e);
	}
};
//#endregion
//#region src/mint/Mint.ts
function Q(e) {
	return Array.isArray(e) ? "array" : typeof e;
}
function Do(e) {
	return typeof e != "string" || e.length < 16 ? "(hidden)" : `${e.slice(0, 4)}...${e.slice(-4)}`;
}
var Oo = class {
	constructor(e, t) {
		this._lastResponseMetadata = void 0, this._captureResponseMetadata = (e) => {
			this._lastResponseMetadata = e;
		}, this._mintUrl = ua(e), this._request = t?.customRequest ?? yo, this._authProvider = t?.authProvider, this._logger = t?.logger ?? O, io(this._logger);
	}
	get mintUrl() {
		return this._mintUrl;
	}
	get lastResponseMetadata() {
		return this._lastResponseMetadata;
	}
	async oidcAuth(e) {
		let t = (await this.getLazyMintInfo()).nuts[21];
		if (!t?.openid_discovery) throw new S("Mint: no NUT-21 openid_discovery");
		return new Va(t.openid_discovery, {
			...e,
			clientId: e?.clientId ?? t.client_id ?? "cashu-client"
		});
	}
	async getInfo(e) {
		let t = await (e ?? this._request)({
			endpoint: q(this._mintUrl, "/v1/info"),
			onResponseMeta: this._captureResponseMetadata,
			logger: this._logger
		});
		return Z.normalizeInfo(t, this._logger);
	}
	async getLazyMintInfo(e) {
		if (this._mintInfo) return this._mintInfo;
		let t = await this.getInfo(e);
		return this._mintInfo = new Z(t, this._logger), this._mintInfo;
	}
	setMintInfo(e) {
		this._mintInfo = e instanceof Z ? e : new Z(e, this._logger);
	}
	async swap(e, t) {
		k(!Array.isArray(e?.inputs), "swap: inputs must be an array of proofs", this._logger);
		let n = await this.requestWithAuth("POST", "/v1/swap", { requestBody: {
			...e,
			inputs: this.stripWalletFields(e.inputs)
		} }, t);
		if (!G(n) || !Array.isArray(n?.signatures)) throw this._logger.error("Invalid response from mint...", {
			type: Q(n),
			op: "swap"
		}), new S("Invalid response from mint");
		return this.assertSignatureCount(n.signatures, e.outputs, "swap"), n.signatures = this.normalizeSignatureAmounts(n.signatures), n;
	}
	async createMintQuote(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid mint quote method: ${e}`, this._logger);
		let r = await this.requestWithAuth("POST", `/v1/mint/quote/${e}`, { requestBody: t }, n?.customRequest);
		return this.normalizeMintQuoteResponse(e, r, n?.normalize);
	}
	async createMintQuoteBolt11(e, t) {
		return this.createMintQuote("bolt11", {
			...e,
			amount: j.from(e.amount).toBigInt()
		}, { customRequest: t });
	}
	async createMintQuoteBolt12(e, t) {
		let n = { ...e };
		return e.amount !== void 0 && (n.amount = j.from(e.amount).toBigInt()), this.createMintQuote("bolt12", n, { customRequest: t });
	}
	async createMintQuoteOnchain(e, t) {
		return this.createMintQuote("onchain", e, { customRequest: t });
	}
	async checkMintQuote(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid mint quote method: ${e}`, this._logger);
		let r = await this.requestWithAuth("GET", `/v1/mint/quote/${e}/${t}`, {}, n?.customRequest), i = this.normalizeMintQuoteResponse(e, r, n?.normalize);
		if (i.quote !== t) throw this._logger.error("Invalid response from mint...", {
			op: `checkMintQuote.${e}`,
			expected: Do(t),
			received: Do(i.quote)
		}), new S("Mint quote response is for a different quote");
		return i;
	}
	async checkMintQuoteBolt11(e, t) {
		return this.checkMintQuote("bolt11", e, { customRequest: t });
	}
	async checkMintQuoteBolt12(e, t) {
		return this.checkMintQuote("bolt12", e, { customRequest: t });
	}
	async checkMintQuoteOnchain(e, t) {
		return this.checkMintQuote("onchain", e, { customRequest: t });
	}
	async checkMintQuoteBatch(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid mint quote method: ${e}`, this._logger), k(t.length === 0, "checkMintQuoteBatch: no quote ids provided", this._logger), k(new Set(t).size !== t.length, "checkMintQuoteBatch: duplicate quote ids provided", this._logger);
		let r = await this.requestWithAuth("POST", `/v1/mint/quote/${e}/check`, { requestBody: { quotes: t } }, n?.customRequest);
		if (!Array.isArray(r) || r.length !== t.length) throw this._logger.error("Invalid response from mint...", {
			expectedCount: t.length,
			actualCount: Array.isArray(r) ? r.length : void 0,
			op: `checkMintQuoteBatch.${e}`
		}), new S("Invalid response from mint");
		return r.map((r, i) => {
			if (r.quote !== t[i]) throw this._logger.error("Invalid response from mint...", {
				index: i,
				op: `checkMintQuoteBatch.${e}`
			}), new S("Invalid response from mint");
			return this.normalizeMintQuoteResponse(e, r, n?.normalize);
		});
	}
	async checkMintQuoteBatchBolt11(e, t) {
		return this.checkMintQuoteBatch("bolt11", e, { customRequest: t });
	}
	async checkMintQuoteBatchBolt12(e, t) {
		return this.checkMintQuoteBatch("bolt12", e, { customRequest: t });
	}
	async mintBolt11(e, t) {
		return this.mint("bolt11", e, { customRequest: t });
	}
	async mintBolt12(e, t) {
		return this.mint("bolt12", e, { customRequest: t });
	}
	async mintOnchain(e, t) {
		return this.mint("onchain", e, { customRequest: t });
	}
	async mint(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid mint method: ${e}`, this._logger);
		let r = await this.requestWithAuth("POST", `/v1/mint/${e}`, { requestBody: t }, n?.customRequest);
		if (!G(r) || !Array.isArray(r?.signatures)) throw this._logger.error("Invalid response from mint...", {
			type: Q(r),
			op: `mint.${e}`
		}), new S("Invalid response from mint");
		return this.assertSignatureCount(r.signatures, t.outputs, `mint.${e}`), r.signatures = this.normalizeSignatureAmounts(r.signatures), n?.normalize ? n.normalize(r) : r;
	}
	async mintBatchBolt11(e, t) {
		return this.mintBatch("bolt11", e, { customRequest: t });
	}
	async mintBatchBolt12(e, t) {
		return this.mintBatch("bolt12", e, { customRequest: t });
	}
	async mintBatch(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid mint method: ${e}`, this._logger);
		let r = {
			...t,
			quote_amounts: t.quote_amounts.map((e) => j.from(e).toBigInt())
		}, i = await this.requestWithAuth("POST", `/v1/mint/${e}/batch`, { requestBody: r }, n?.customRequest);
		if (!G(i) || !Array.isArray(i?.signatures)) throw this._logger.error("Invalid response from mint...", {
			type: Q(i),
			op: `mintBatch.${e}`
		}), new S("Invalid response from mint");
		return this.assertSignatureCount(i.signatures, t.outputs, `mintBatch.${e}`), i.signatures = this.normalizeSignatureAmounts(i.signatures), n?.normalize ? n.normalize(i) : i;
	}
	async createMeltQuote(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid melt quote method: ${e}`, this._logger);
		let r = await this.requestWithAuth("POST", `/v1/melt/quote/${e}`, { requestBody: t }, n?.customRequest);
		return this.normalizeMeltQuoteResponse(e, r, n?.normalize);
	}
	async createMeltQuoteBolt11(e, t) {
		let n = await this.createMeltQuote("bolt11", this.normalizeMeltQuoteRequestOptions(e), { customRequest: t });
		return k(n.request !== "" && n.request.toLowerCase() !== e.request.toLowerCase(), "Melt quote is for a different payment request", this._logger), n;
	}
	async createMeltQuoteBolt12(e, t) {
		return this.createMeltQuote("bolt12", this.normalizeMeltQuoteRequestOptions(e), { customRequest: t });
	}
	async createMeltQuoteOnchain(e, t) {
		return this.createMeltQuote("onchain", {
			...e,
			amount: j.from(e.amount).toBigInt()
		}, { customRequest: t });
	}
	async checkMeltQuote(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid melt quote method: ${e}`, this._logger);
		let r = await this.requestWithAuth("GET", `/v1/melt/quote/${e}/${t}`, {}, n?.customRequest), i = this.normalizeMeltQuoteResponse(e, r, n?.normalize);
		if (i.quote !== t) throw this._logger.error("Invalid response from mint...", {
			op: `checkMeltQuote.${e}`,
			expected: Do(t),
			received: Do(i.quote)
		}), new S("Melt quote response is for a different quote");
		return i;
	}
	async checkMeltQuoteBolt11(e, t) {
		return this.checkMeltQuote("bolt11", e, { customRequest: t });
	}
	async checkMeltQuoteBolt12(e, t) {
		return this.checkMeltQuote("bolt12", e, { customRequest: t });
	}
	async checkMeltQuoteOnchain(e, t) {
		return this.checkMeltQuote("onchain", e, { customRequest: t });
	}
	async melt(e, t, n) {
		k(!this.isValidMethodString(e), `Invalid melt method: ${e}`, this._logger), k(!Array.isArray(t?.inputs), "melt: inputs must be an array of proofs", this._logger);
		let r = await this.requestWithAuth("POST", `/v1/melt/${e}`, { requestBody: {
			...t,
			inputs: this.stripWalletFields(t.inputs)
		} }, n?.customRequest), i = this.normalizeMeltQuoteResponse(e, r, n?.normalize, !0);
		return i.quote !== t.quote && (this._logger.warn("Melt response reports a different quote id", {
			op: `melt.${e}`,
			expected: Do(t.quote),
			received: Do(i.quote)
		}), i.quote = t.quote), i;
	}
	async meltBolt11(e, t) {
		return this.melt("bolt11", e, t);
	}
	async meltBolt12(e, t) {
		return this.melt("bolt12", e, t);
	}
	async meltOnchain(e, t) {
		return this.melt("onchain", e, t);
	}
	async check(e, t) {
		let n = await this.requestWithAuth("POST", "/v1/checkstate", { requestBody: e }, t);
		if (!G(n) || !Array.isArray(n?.states) || n.states.length > e.Ys.length) throw this._logger.error("Invalid response from mint...", {
			type: Q(n),
			op: "check"
		}), new S("Invalid response from mint");
		this.assertRecordEntries(n.states, "check");
		for (let e of n.states) la(e, "witness");
		return n;
	}
	async getKeys(e, t, n) {
		let r = t || this._mintUrl;
		e && (e = e.replace(/\//g, "_").replace(/\+/g, "-"));
		let i = await (n ?? this._request)({
			endpoint: e ? q(r, "/v1/keys", e) : q(r, "/v1/keys"),
			onResponseMeta: this._captureResponseMetadata,
			logger: this._logger
		});
		if (!G(i) || !Array.isArray(i.keysets)) throw this._logger.error("Invalid response from mint...", {
			type: Q(i),
			op: "getKeys"
		}), new S("Invalid response from mint");
		return this.assertKeysetList(i.keysets, "getKeys"), {
			...i,
			keysets: i.keysets.map((e) => za(e))
		};
	}
	async getKeySets(e) {
		let t = await (e ?? this._request)({
			endpoint: q(this._mintUrl, "/v1/keysets"),
			onResponseMeta: this._captureResponseMetadata,
			logger: this._logger
		});
		if (!G(t) || !Array.isArray(t.keysets)) throw this._logger.error("Invalid response from mint...", {
			type: Q(t),
			op: "getKeySets"
		}), new S("Invalid response from mint");
		return this.assertKeysetList(t.keysets, "getKeySets"), {
			...t,
			keysets: t.keysets.map((e) => Ra(e))
		};
	}
	async restore(e, t) {
		let n = await (t ?? this._request)({
			endpoint: q(this._mintUrl, "/v1/restore"),
			method: "POST",
			requestBody: e,
			onResponseMeta: this._captureResponseMetadata,
			logger: this._logger
		});
		if (!G(n) || !Array.isArray(n?.outputs) || !Array.isArray(n?.signatures) || n.outputs.length !== n.signatures.length || n.outputs.length > e.outputs.length) throw this._logger.error("Invalid response from mint...", {
			type: Q(n),
			op: "restore"
		}), new S("Invalid response from mint");
		return n.outputs = this.normalizeMessageAmounts(n.outputs), n.signatures = this.normalizeSignatureAmounts(n.signatures), n;
	}
	async connectWebSocket() {
		try {
			let e = new URL(this._mintUrl), t = "v1/ws";
			e.pathname.endsWith("/") ? e.pathname += t : e.pathname += "/" + t, e.protocol = e.protocol === "https:" ? "wss:" : "ws:";
			let n = e.toString();
			this.ws || (this.ws = new Eo(n, this._logger)), await this.ws.ensureConnection();
		} catch (e) {
			this._logger.error("Failed to connect to WebSocket...", { e });
			try {
				this.ws?.close();
			} catch {}
			throw this.ws = void 0, new S("Failed to connect to WebSocket...", { cause: e });
		}
	}
	disconnectWebSocket() {
		this.ws && this.ws.close();
	}
	get webSocketConnection() {
		return this.ws;
	}
	async handleClearAuth(e, t, n) {
		if (this._authProvider && (n ?? await this.getLazyMintInfo()).requiresClearAuthToken(e, t)) return this._authProvider.ensureCAT ? this._authProvider.ensureCAT() : this._authProvider.getCAT();
	}
	async handleBlindAuth(e, t, n) {
		if (this._authProvider && (n ?? await this.getLazyMintInfo()).requiresBlindAuthToken(e, t)) return await this._authProvider.getBlindAuthToken({
			method: e,
			path: t
		});
	}
	async requestWithAuth(e, t, n = {}, r) {
		let i = r ?? this._request, a = this._mintInfo;
		this._authProvider && (a = await this.getLazyMintInfo(r));
		let o = await this.handleBlindAuth(e, t, a), s = await this.handleClearAuth(e, t, a), c = {
			...n.headers ?? {},
			...o ? { "Blind-auth": o } : {},
			...s ? { "Clear-auth": s } : {}
		}, l = a?.isSupported(19);
		return i({
			...n,
			endpoint: q(this._mintUrl, t),
			method: e,
			headers: c,
			...o || s || n.requestBody ? { redirect: "error" } : {},
			...l?.supported && l.params ? l.params : {},
			onResponseMeta: this._captureResponseMetadata,
			logger: n.logger ?? this._logger
		});
	}
	normalizeMeltQuoteRequestOptions(e) {
		if (!e.options) return { ...e };
		let t = { ...e.options };
		return e.options.amountless && (t.amountless = { amount_msat: j.from(e.options.amountless.amount_msat).toBigInt() }), "mpp" in e.options && e.options.mpp && (t.mpp = { amount: j.from(e.options.mpp.amount).toBigInt() }), {
			...e,
			options: t
		};
	}
	isValidMethodString(e) {
		return !!(typeof e == "string" && /^[a-z0-9_-]+$/.test(e));
	}
	normalizeSignatureAmounts(e) {
		return this.assertRecordEntries(e, "signatures"), e.map((e) => ({
			...e,
			amount: j.from(e.amount)
		}));
	}
	normalizeMessageAmounts(e) {
		return this.assertRecordEntries(e, "outputs"), e.map((e) => ({
			...e,
			amount: j.from(e.amount)
		}));
	}
	stripWalletFields(e) {
		return e.map((e) => {
			let { dleq: t, p2pk_e: n, ...r } = e;
			return r;
		});
	}
	assertRecordEntries(e, t) {
		if (!e.every((e) => K(e))) throw this._logger.error("Invalid response from mint...", {
			entries: e.length,
			op: t
		}), new S("Invalid response from mint");
	}
	assertSignatureCount(e, t, n) {
		if (!(e.length <= t.length)) throw this._logger.error("Invalid response from mint...", {
			signatures: e.length,
			outputs: t.length,
			op: n
		}), new S(`Invalid response from mint: ${e.length} signatures, expected ${t.length}. The operation may already have been applied; if the wallet is seeded, try restoring (NUT-09) to recover.`);
	}
	assertKeysetList(e, t) {
		if (e.length > 1e4) throw this._logger.error("Invalid response from mint...", {
			keysets: e.length,
			op: t
		}), new S("Invalid response from mint");
		this.assertRecordEntries(e, t);
	}
	normalizeMintQuoteResponse(e, t, n) {
		let r = `${e} mint quote`;
		if (!K(t)) throw this._logger.error("Invalid response from mint...", {
			type: Q(t),
			op: r
		}), new S("Invalid response from mint");
		let i = { ...t };
		return this.normalizeMintBaseFields(i), e === "bolt11" ? this.normalizeMintQuoteBolt11Fields(i) : e === "bolt12" ? this.normalizeMintQuoteBolt12Fields(i) : e === "onchain" && this.normalizeMintQuoteOnchainFields(i), n ? n(i) : i;
	}
	normalizeMintBaseFields(e) {
		e.amount_paid != null && e.amount_issued != null && (e.amount_paid = j.from(e.amount_paid), e.amount_issued = j.from(e.amount_issued)), e.updated_at = X(e.updated_at, "mintQuote.updated_at", null);
	}
	deriveMintQuoteState(e, t) {
		return e.isZero() && t.isZero() ? Ha.UNPAID : e.greaterThan(t) ? Ha.PAID : Ha.ISSUED;
	}
	normalizeMintQuoteBolt11Fields(e) {
		e.amount = j.from(e.amount), e.expiry = X(e.expiry, "mintQuoteBolt11.expiry", null), (typeof e.state != "string" || !Object.values(Ha).includes(e.state)) && e.amount_paid instanceof j && e.amount_issued instanceof j && (e.state = this.deriveMintQuoteState(e.amount_paid, e.amount_issued));
	}
	normalizeMintQuoteBolt12Fields(e) {
		la(e, "amount"), e.amount = e.amount === null ? null : j.from(e.amount), e.expiry = X(e.expiry, "mintQuoteBolt12.expiry", null), e.amount_paid = j.from(e.amount_paid), e.amount_issued = j.from(e.amount_issued);
	}
	normalizeMintQuoteOnchainFields(e) {
		e.expiry = X(e.expiry, "mintQuoteOnchain.expiry", null), e.amount_paid = j.from(e.amount_paid), e.amount_issued = j.from(e.amount_issued);
	}
	normalizeMeltQuoteResponse(e, t, n, r = !1) {
		let i = `${e} melt quote`;
		if (!K(t)) throw this._logger.error("Invalid response from mint...", {
			type: Q(t),
			op: i
		}), new S("Invalid response from mint");
		let a = { ...t };
		return r && ((a.request == null || a.request === "") && delete a.request, a.fee_reserve ?? delete a.fee_reserve), this.normalizeMeltBaseFields(a, i), e === "bolt11" || e === "bolt12" ? this.normalizeMeltBoltFields(a, e, i, r) : e === "onchain" && this.normalizeMeltOnchainFields(a, r), n ? n(a) : a;
	}
	normalizeMeltBaseFields(e, t) {
		if (e.amount = j.from(e.amount), e.expiry = X(e.expiry, "meltQuote.expiry", void 0), e.change) {
			if (!Array.isArray(e.change) || e.change.length > 1024) throw this._logger.error("Invalid response from mint...", {
				type: Q(e.change),
				op: t
			}), new S("Invalid response from mint");
			e.change = this.normalizeSignatureAmounts(e.change);
		}
		if (!G(e) || typeof e.quote != "string" || !(e.amount instanceof j) || typeof e.unit != "string" || typeof e.state != "string" || typeof e.expiry != "number" || !Object.values(Ua).includes(e.state)) throw this._logger.error("Invalid response from mint...", { op: t }), new S("Invalid response from mint");
	}
	normalizeMeltBoltFields(e, t, n, r) {
		let i = (t) => r && e[t] === void 0;
		if (!i("fee_reserve") && e.fee_reserve != null && (e.fee_reserve = j.from(e.fee_reserve)), typeof e.request != "string" && !i("request") || !(e.fee_reserve instanceof j) && !i("fee_reserve")) throw this._logger.error("Invalid response from mint...", { op: n }), new S("Invalid response from mint");
		Ia(e, t, this._logger, n);
	}
	normalizeMeltOnchainFields(e, t) {
		if (t && e.fee_options === void 0) {
			la(e, "selected_fee_index", "outpoint");
			return;
		}
		if (!Array.isArray(e.fee_options) || e.fee_options.length === 0 || e.fee_options.length > 1024 || (e.fee_options = e.fee_options.map((e) => {
			let t = e;
			if (!Number.isSafeInteger(t.fee_index)) throw this._logger.error("Invalid response from mint...", { op: "onchain melt quote" }), new S("Invalid response from mint");
			return {
				...t,
				fee_index: t.fee_index,
				fee_reserve: j.from(t.fee_reserve),
				estimated_blocks: t.estimated_blocks
			};
		}), la(e, "selected_fee_index", "outpoint"), typeof e.request != "string" || e.selected_fee_index !== null && !Number.isSafeInteger(e.selected_fee_index) || e.outpoint !== null && typeof e.outpoint != "string")) throw this._logger.error("Invalid response from mint...", { op: "onchain melt quote" }), new S("Invalid response from mint");
	}
}, ko = class e {
	constructor(e, t, n, r, i) {
		this._keys = {}, this._id = e, this._unit = t, this._active = n, this._input_fee_ppk = r, this._final_expiry = i;
	}
	get id() {
		return this._id;
	}
	get unit() {
		return this._unit;
	}
	get isActive() {
		return this._active;
	}
	get fee() {
		return this._input_fee_ppk ?? 0;
	}
	get expiry() {
		return this._final_expiry;
	}
	get hasKeys() {
		return Object.keys(this._keys).length > 0;
	}
	get hasHexId() {
		return U(this._id) && !Gt(this._id);
	}
	get version() {
		return this.hasHexId ? i(this._id)[0] : -1;
	}
	get keys() {
		return this._keys;
	}
	set keys(e) {
		this._keys = e;
	}
	toMintKeyset() {
		return {
			id: this._id,
			unit: this._unit,
			active: this._active,
			input_fee_ppk: this._input_fee_ppk,
			final_expiry: this._final_expiry
		};
	}
	toMintKeys() {
		return this.hasKeys ? {
			id: this._id,
			unit: this._unit,
			active: this._active,
			input_fee_ppk: this._input_fee_ppk,
			final_expiry: this._final_expiry,
			keys: this._keys
		} : null;
	}
	verify() {
		return this.hasKeys ? e.verifyKeysetId(this.toMintKeys()) : !1;
	}
	static verifyKeysetId(e) {
		try {
			let t = e.keys ? Object.keys(e.keys).length : 0;
			if (t === 0 || t > 256 || new Set(Object.values(e.keys).map((e) => e.toLowerCase())).size !== t) return !1;
			let n = Gt(e.id), r = !n && U(e.id) ? i(e.id)[0] : 0;
			return oa(e.keys, {
				input_fee_ppk: e.input_fee_ppk,
				expiry: e.final_expiry,
				unit: e.unit,
				versionByte: r,
				isDeprecatedBase64: n
			}) === e.id;
		} catch {
			return !1;
		}
	}
	static fromMintApi(t, n) {
		let r = Ra(t), i = n ? za(n) : void 0, a = new e(r.id, r.unit, r.active, r.input_fee_ppk, r.final_expiry);
		if (i) {
			if (i.id !== r.id) throw new S(`Mismatched keyset ids: meta=${r.id}, keys=${i.id}`);
			if (i.unit !== r.unit) throw new S(`Mismatched keyset units: meta=${r.unit}, keys=${i.unit}`);
			if (i.final_expiry !== void 0 && r.final_expiry !== void 0 && i.final_expiry !== r.final_expiry) throw new S(`Mismatched keyset expiry for id=${r.id}`);
			a.keys = i.keys;
		}
		return a;
	}
}, Ao = class e {
	assertInitialized() {
		k(Object.keys(this.keysets).length === 0, "KeyChain not initialized", this._logger);
	}
	constructor(e, t, n = O) {
		this.keysets = Object.create(null), this.pendingKeyFetches = /* @__PURE__ */ new Map(), this.generation = 0, this.refreshSeq = 0, this.mint = typeof e == "string" ? new Oo(e) : e, this.unit = t, this._logger = n;
	}
	static fromCache(t, n, r, i) {
		let a = new e(t, n, i);
		return a.loadFromCache(r), a;
	}
	static mintToCacheDTO(e, t, n) {
		let r = new Map(n.map((e) => [e.id, e]));
		return {
			keysets: t.map((e) => {
				let t = r.get(e.id), n = { ...e };
				return t && (n.keys = { ...t.keys }), n;
			}),
			mintUrl: e,
			savedAt: Date.now()
		};
	}
	static cacheToMintDTO(e) {
		return {
			keysets: e.keysets.map((e) => ({
				id: e.id,
				unit: e.unit,
				active: e.active,
				input_fee_ppk: e.input_fee_ppk,
				final_expiry: e.final_expiry
			})),
			keys: e.keysets.filter((e) => !!e.keys).map((e) => ({
				id: e.id,
				unit: e.unit,
				active: e.active,
				input_fee_ppk: e.input_fee_ppk,
				final_expiry: e.final_expiry,
				keys: { ...e.keys }
			}))
		};
	}
	async init(e) {
		if (Object.keys(this.keysets).length > 0 && !e) return;
		let t = ++this.refreshSeq, [n, r] = await Promise.all([this.mint.getKeySets(), this.mint.getKeys()]);
		if (t !== this.refreshSeq) {
			this._logger.debug("Discarding keychain refresh superseded by a later one", { seq: t });
			return;
		}
		this.buildKeychain(n.keysets, r.keysets), this.savedAt = Date.now();
	}
	loadFromCache(t) {
		let n;
		try {
			n = ua(String(t.mintUrl));
		} catch {
			n = void 0;
		}
		n !== this.mint.mintUrl && this._logger.warn(`KeyChain cache is for a different mint: ${n ?? "unknown"} (expected ${this.mint.mintUrl}). This will become an error in cashu-ts v5.`, {
			cacheMintUrl: t.mintUrl,
			mintUrl: this.mint.mintUrl
		});
		let { keysets: r, keys: i } = e.cacheToMintDTO(t);
		this.buildKeychain(r, i), this.savedAt = t.savedAt;
	}
	buildKeychain(e, t) {
		let n = this.keysets, r = Object.create(null), i = new Map(t.map((e) => [e.id, e]));
		for (let t of e) {
			let e = i.get(t.id), a = e ? ko.fromMintApi(t, e) : ko.fromMintApi(t);
			a.verify() || (a.hasKeys && this._logger.warn("Discarding keys that do not derive their keyset id", { id: a.id }), a.keys = {});
			let o = n[t.id];
			!a.hasKeys && o?.hasKeys && o.unit === a.unit && (a.keys = { ...o.keys }, a.verify() || (this._logger.warn("Dropping carried keys: fresh metadata no longer derives the keyset id", { id: a.id }), a.keys = {})), r[a.id] = a;
		}
		this.keysets = r, this.generation++;
	}
	getKeyset(e) {
		let t = e ? this.keysets[e] : this.getCheapestKeyset();
		return ce(t, `Keyset '${e}' not found`, this._logger, { keysetId: e }), t;
	}
	getCheapestKeyset() {
		this.assertInitialized();
		let e = Object.values(this.keysets).filter((e) => e.unit === this.unit && e.isActive && e.hasHexId), t = e.filter((e) => e.hasKeys && e.version <= 1);
		if (t.length === 0) {
			let t = e.filter((e) => e.version > 1);
			if (t.length > 0) {
				let e = Math.min(...t.map((e) => e.version));
				se(`No supported keyset for unit: ${this.unit}. The mint's active keysets are ${aa(e)} or later; this build of cashu-ts supports up to ${aa(1)}. Upgrade to spend on this mint.`, this._logger, {
					unit: this.unit,
					lowestVersionByte: e,
					supported: 1
				});
			}
			se(`No active keyset found for unit: ${this.unit}`, this._logger, { unit: this.unit });
		}
		let n = 2 ** 53 - 1;
		return t.sort((e, t) => t.version - e.version || e.fee - t.fee || (t.expiry ?? n) - (e.expiry ?? n))[0];
	}
	async ensureKeysetKeys(e) {
		let t = this.keysets[e];
		if (ce(t, `Keyset '${e}' not found`, this._logger, { keysetId: e }), W(t.id, this._logger), t.hasKeys) return t;
		let n = this.pendingKeyFetches.get(e);
		if (n) return await n;
		let r = (async () => {
			let n = this.generation, r = (await this.mint.getKeys(e)).keysets.find((t) => t.id === e);
			k(!r || !r.keys || Object.keys(r.keys).length === 0, `Mint returned no keys for keyset '${e}'`, this._logger, { keysetId: e });
			let i = t.toMintKeyset(), a = ko.fromMintApi(i, r);
			if (k(!a.verify(), `Keyset verification failed for ID ${e}`, this._logger, { keysetId: e }), this.generation !== n) {
				this._logger.debug("Keychain refreshed during key fetch; returning the live keyset", { id: e });
				let t = this.keysets[e];
				return ce(t, `Keyset '${e}' not found`, this._logger, { keysetId: e }), t;
			}
			return this.keysets[e] = a, a;
		})();
		this.pendingKeyFetches.set(e, r);
		try {
			return await r;
		} finally {
			this.pendingKeyFetches.delete(e);
		}
	}
	getKeysets() {
		this.assertInitialized();
		let e = Object.values(this.keysets).filter((e) => e.unit === this.unit);
		return k(e.length === 0, `No keysets found for unit: ${this.unit}`, this._logger, { unit: this.unit }), e;
	}
	isUnitKeyset(e) {
		if (!e) return !1;
		let t = this.keysets[e];
		return t !== void 0 && t.unit === this.unit;
	}
	hasKeyset(e) {
		return !!e && this.keysets[e] !== void 0;
	}
	getAllKeys() {
		return this.assertInitialized(), Object.values(this.keysets).map((e) => e.toMintKeys()).filter((e) => e !== null);
	}
	getAllKeysetIds() {
		return this.assertInitialized(), Object.keys(this.keysets);
	}
	get cache() {
		let t = Object.values(this.keysets), n = t.map((e) => e.toMintKeyset()), r = t.map((e) => e.toMintKeys()).filter((e) => e !== null), i = e.mintToCacheDTO(this.mint.mintUrl, n, r);
		return i.savedAt = this.savedAt, i;
	}
}, jo = class {
	constructor(e, t, n) {
		this.amountValue = j.from(e), this.B_ = t, this.id = n;
	}
	get amount() {
		return this.amountValue;
	}
	getSerializedBlindedMessage() {
		return {
			amount: this.amountValue,
			B_: this.B_.toHex(!0),
			id: this.id
		};
	}
}, Mo = new Set([
	"locktime",
	"pubkeys",
	"n_sigs",
	"refund",
	"n_sigs_refund",
	"sigflag"
]);
function No(e) {
	if (!e || typeof e != "string") throw new S("tag key must be a non empty string");
	if (Mo.has(e)) throw new S(`additionalTags must not use reserved key "${e}"`);
}
var $ = class e {
	constructor(e, t, n, r) {
		if (typeof t != "bigint" || t <= 0n) throw new S("OutputData: blindingFactor must be a positive bigint");
		if (!(n instanceof Uint8Array) || n.length === 0) throw new S("OutputData: secret must be a non-empty Uint8Array");
		if (e == null) throw new S("OutputData: blindedMessage is required");
		this.secret = n, this.blindingFactor = t, this.blindedMessage = e, this.ephemeralE = r;
	}
	toProof(e, t) {
		if (e == null) throw new S("Mint response is missing a signature for one of the outputs. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.");
		if (!this.blindedMessage.amount.isZero() && e.id !== this.blindedMessage.id) throw new S(`Mint signature keyset id ${e.id} does not match output ${this.blindedMessage.id}`);
		let r;
		e.dleq && (r = {
			s: i(e.dleq.s),
			e: i(e.dleq.e),
			r: this.blindingFactor
		});
		let a = e.amount.toString(), o = P(t.keys[a]);
		if (r) {
			let t = P(this.blindedMessage.B_), n = P(e.C_);
			if (!ur(r, t, n, o)) throw new S("DLEQ verification failed on mint response");
		}
		let s = Qt({
			id: e.id,
			C_: P(e.C_)
		}, this.blindingFactor, this.secret, o), c = {
			id: e.id,
			amount: e.amount,
			C: s.C.toHex(!0),
			secret: Te(s.secret),
			...r && { dleq: {
				s: n(r.s),
				e: n(r.e),
				r: Hi(this.blindingFactor)
			} }
		};
		return this.ephemeralE && (c.p2pk_e = this.ephemeralE), c;
	}
	static createP2PKData(e, t, n, r) {
		let i = H(t, n.keys, r), a = e.blindKeys && e.sigFlag === "SIG_ALL" ? qt() : void 0;
		return i.map((t) => this.createSingleP2PKData(e, t, n.id, a));
	}
	static createSingleP2PKData(t, r, i, o) {
		let s = j.from(r), c = In(t), l = Array.isArray(c.pubkey) ? c.pubkey : [c.pubkey], u = c.refundKeys ?? [], d = c.requiredSignatures ?? 1, f = c.requiredRefundSignatures ?? 1, p = c.hashlock, m = typeof p == "string" && p.length > 0, h = m ? p : l[0], g = m ? l : l.slice(1), _ = u, v;
		if (t.blindKeys) {
			let { blinded: e, Ehex: t } = En([...l, ...u], o, !m);
			m ? g = e.slice(0, l.length) : (h = e[0], g = e.slice(1, l.length)), _ = e.slice(l.length), v = t;
		}
		let y = [], b = c.locktime ?? NaN;
		if (Number.isSafeInteger(b) && b >= 0 && y.push(["locktime", String(b)]), g.length > 0 && (y.push(["pubkeys", ...g]), d > 1 && y.push(["n_sigs", String(d)])), _.length > 0 && (y.push(["refund", ..._]), f > 1 && y.push(["n_sigs_refund", String(f)])), c.sigFlag == "SIG_ALL" && y.push(["sigflag", "SIG_ALL"]), c.additionalTags?.length) {
			let e = c.additionalTags.map(([e, ...t]) => (No(e), [e, ...t.map(String)]));
			y.push(...e);
		}
		let x = [m ? "HTLC" : "P2PK", {
			nonce: n(a(32)),
			data: h,
			tags: y
		}], C = JSON.stringify(x), w = [...C].length;
		if (w > 1024) throw new S(`Secret too long (${w} characters), maximum is ${fe}`);
		let T = new TextEncoder().encode(C), { r: E, B_: D } = Xt(T);
		return new e(new jo(s, D, i).getSerializedBlindedMessage(), E, T, v);
	}
	static createRandomData(e, t, n) {
		return H(e, t.keys, n).map((e) => this.createSingleRandomData(e, t.id));
	}
	static createSingleRandomData(t, r) {
		let i = j.from(t), o = n(a(32)), s = new TextEncoder().encode(o), { r: c, B_: l } = Xt(s);
		return new e(new jo(i, l, r).getSerializedBlindedMessage(), c, s);
	}
	static createDeterministicData(e, t, n, r, i) {
		let a = H(e, r.keys, i), o = br(t, r.id);
		return a.map((e, t) => Po(e, r.id, o(n + t)));
	}
	static createSingleDeterministicData(e, t, n, r) {
		return Po(e, r, yr(t, r, n));
	}
	static sumOutputAmounts(e) {
		return j.sum(e.map((e) => e.blindedMessage.amount));
	}
	static serialize(e) {
		return {
			blindedMessage: {
				amount: e.blindedMessage.amount.toString(),
				B_: e.blindedMessage.B_,
				id: e.blindedMessage.id
			},
			blindingFactor: e.blindingFactor.toString(),
			secret: n(e.secret),
			...e.ephemeralE && { ephemeralE: e.ephemeralE }
		};
	}
	static deserialize(t) {
		try {
			if (!/^(0|[1-9]\d*)$/.test(t.blindingFactor)) throw new S("blindingFactor must be a canonical decimal integer");
			let n = i(t.secret);
			Te(n);
			let r = BigInt(t.blindingFactor), { B_: a } = Xt(n, r);
			if (a.toHex(!0) !== t.blindedMessage.B_.toLowerCase()) throw new S("stored output does not match its secret. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.");
			return new e({
				amount: j.from(t.blindedMessage.amount),
				B_: t.blindedMessage.B_,
				id: t.blindedMessage.id
			}, r, n, t.ephemeralE ? P(t.ephemeralE).toHex(!0) : void 0);
		} catch (e) {
			throw new S(`Invalid SerializedOutputData: ${e instanceof Error ? e.message : String(e)}`, { cause: e });
		}
	}
};
function Po(e, t, r) {
	let i = j.from(e), a = n(r.secret), o = new TextEncoder().encode(a), { r: s, B_: c } = Xt(o, g(r.blindingFactor));
	return new $(new jo(i, c, t).getSerializedBlindedMessage(), s, o);
}
//#endregion
//#region src/wallet/P2PKBuilder.ts
function Fo(e) {
	return e instanceof Date ? Math.floor(e.getTime() / 1e3) : Math.floor(e < 0xe8d4a51000 ? e : e / 1e3);
}
var Io = class e {
	constructor() {
		this.lockKeys = [], this.refundKeys = [], this.extraTags = [];
	}
	addMainPubkey(e) {
		let t = Array.isArray(e) ? e : [e];
		return this.lockKeys = R([...this.lockKeys, ...t]), this;
	}
	addLockPubkey(e) {
		return this.addMainPubkey(e);
	}
	addRefundPubkey(e) {
		let t = Array.isArray(e) ? e : [e];
		return this.refundKeys = R([...this.refundKeys, ...t]), this;
	}
	lockUntil(e) {
		return this.locktime = Fo(e), this;
	}
	requireMainSignatures(e) {
		if (!Number.isInteger(e) || e < 1) throw new S(`requiredSignatures (n_sigs) must be a positive integer, got ${e}`);
		return this.nSigs = e, this;
	}
	requireLockSignatures(e) {
		return this.requireMainSignatures(e);
	}
	requireRefundSignatures(e) {
		if (!Number.isInteger(e) || e < 1) throw new S(`requiredRefundSignatures (n_sigs_refund) must be a positive integer, got ${e}`);
		return this.nSigsRefund = e, this;
	}
	addTag(e, t) {
		No(e);
		let n = t === void 0 ? [] : Array.isArray(t) ? t : [t];
		return this.extraTags.push([e, ...n.map(String)]), this;
	}
	addTags(e) {
		for (let [t, ...n] of e) this.addTag(t, n);
		return this;
	}
	blindKeys() {
		return this._blindKeys = !0, this;
	}
	sigAll() {
		return this.sigFlag = "SIG_ALL", this;
	}
	addHashlock(e) {
		return this.hashlock = e, this;
	}
	toOptions() {
		let e = this.lockKeys, t = this.refundKeys;
		if (e.length === 0 && !this.hashlock) throw new S("At least one lock pubkey is required");
		let n = {
			pubkey: e.length === 1 ? e[0] : e,
			...this.locktime === void 0 ? {} : { locktime: this.locktime },
			...t.length ? { refundKeys: t } : {},
			...this.nSigs !== void 0 && (this.nSigs > 1 || e.length === 0) ? { requiredSignatures: this.nSigs } : {},
			...this.nSigsRefund !== void 0 && (this.nSigsRefund > 1 || t.length === 0) ? { requiredRefundSignatures: this.nSigsRefund } : {},
			...this.extraTags.length ? { additionalTags: this.extraTags.slice() } : {},
			...this._blindKeys ? { blindKeys: !0 } : {},
			...this.sigFlag == "SIG_ALL" ? { sigFlag: "SIG_ALL" } : {},
			...this.hashlock ? { hashlock: this.hashlock } : {}
		};
		return $.createSingleP2PKData(n, 1, "deedbeef"), n;
	}
	static fromOptions(t) {
		let n = new e();
		if (t.refundKeys !== void 0 && !Array.isArray(t.refundKeys)) throw new S("refundKeys must be an array of pubkeys");
		if (t.blindKeys !== void 0 && typeof t.blindKeys != "boolean") throw new S("blindKeys must be a boolean");
		let r = Array.isArray(t.pubkey) ? t.pubkey : [t.pubkey];
		return n.addMainPubkey(r), t.locktime !== void 0 && n.lockUntil(t.locktime), t.refundKeys?.length && n.addRefundPubkey(t.refundKeys), t.requiredSignatures !== void 0 && n.requireMainSignatures(t.requiredSignatures), t.requiredRefundSignatures !== void 0 && n.requireRefundSignatures(t.requiredRefundSignatures), t.additionalTags?.length && n.addTags(t.additionalTags), t.blindKeys && n.blindKeys(), t.sigFlag == "SIG_ALL" && n.sigAll(), t.hashlock && n.addHashlock(t.hashlock), n;
	}
}, Lo = Io;
//#endregion
//#region src/wallet/SelectProofs.ts
function Ro(e, t, n, r = !1, i = !1, a = O) {
	let o = Y(e), s = j.from(t), c = s.toBigInt(), l = Ce(), u = null, d = null, f = 0n, p = 0n, m = (e) => {
		let t;
		try {
			t = n.getKeyset(e.id).fee;
		} catch (t) {
			let r = `Could not get fee. No keyset found for keyset id: ${e.id}`;
			throw a.error(r, {
				error: t,
				keychain: n.getKeysets()
			}), new S(r, { cause: t });
		}
		return k(!n.isUnitKeyset(e.id), `Proof has unrecognised keyset. '${e.id}' is not a keyset for this wallet unit`, a, { id: e.id }), BigInt(t);
	}, h = (e, t) => e - (r ? (t + 999n) / 1000n : 0n), g = (e) => {
		let t = [...e];
		for (let e = t.length - 1; e > 0; e--) {
			let n = Math.floor(Math.random() * (e + 1));
			[t[e], t[n]] = [t[n], t[e]];
		}
		return t;
	}, _ = (e, t, n) => {
		let r = 0, i = e.length - 1, a = null;
		for (; r <= i;) {
			let o = Math.floor((r + i) / 2), s = e[o].exFee;
			(n ? s <= t : s >= t) ? (a = o, n ? r = o + 1 : i = o - 1) : n ? i = o - 1 : r = o + 1;
		}
		return n ? a : r < e.length ? r : null;
	}, v = (e, t) => {
		let n = t.exFee, r = 0, i = e.length;
		for (; r < i;) {
			let t = Math.floor((r + i) / 2);
			e[t].exFee < n ? r = t + 1 : i = t;
		}
		e.splice(r, 0, t);
	}, y = (e, t) => h(e, t) < c ? null : 1000n * e + t - 1000n * c, b = 0n, x = 0n, C = /* @__PURE__ */ new Set(), w = o.map((e, t) => {
		k(C.has(e.secret), `Duplicate proof at index ${t}: each proof may appear only once`, a, { index: t }), C.add(e.secret);
		let n = m(e), i = e.amount.toBigInt(), o = r ? i - n / 1000n : i, s = {
			proof: e,
			amountBig: i,
			exFee: o,
			ppkfee: n
		};
		return (!r || o > 0n) && (b += i, x += n), s;
	}), T = r ? w.filter((e) => e.exFee > 0n) : w;
	if (T.sort((e, t) => e.exFee < t.exFee ? -1 : +(e.exFee > t.exFee)), T.length > 0) {
		let e;
		if (i) {
			let t = _(T, c + 1n, !0);
			e = t === null ? 0 : t + 1;
		} else {
			let t = _(T, c + 1n, !1);
			if (t !== null) {
				let n = T[t].exFee, r = _(T, n, !0);
				ce(r, "Unexpected null rightIndex in binary search", a), e = r + 1;
			} else e = T.length;
		}
		for (let t = e; t < T.length; t++) b -= T[t].amountBig, x -= T[t].ppkfee;
		T = T.slice(0, e);
	}
	let E = h(b, x);
	if (s.isZero() || c > E) return {
		keep: o,
		send: []
	};
	let D = c < E ? c : E;
	for (let e = 0; e < 60; e++) {
		let t = [], n = 0n, r = 0n;
		for (let e of g(T)) {
			let a = n + e.amountBig, o = r + e.ppkfee, s = h(a, o);
			if (i && s > c || (t.push(e), n = a, r = o, s >= c)) break;
		}
		let o = new Set(t), s = T.filter((e) => !o.has(e)), m = g(Array.from({ length: t.length }, (e, t) => t)).slice(0, 5e3);
		for (let e of m) {
			let a = h(n, r);
			if (a === c || !i && a >= c && a <= D) break;
			let o = t[e], l = n - o.amountBig, u = r - o.ppkfee, d = c - h(l, u), f = _(s, d, i);
			if (f !== null) {
				let a = s[f];
				(!i || a.exFee > o.exFee) && (d >= 0n || a.exFee <= o.exFee) && (t[e] = a, n = l + a.amountBig, r = u + a.ppkfee, s.splice(f, 1), v(s, o));
			}
		}
		let b = y(n, r);
		if (b !== null && (d === null || b < d)) {
			a.debug(`selectProofsToSend: best solution found in trial #${e} - amount: ${n}, delta: ${b}`), u = [...t].sort((e, t) => t.exFee < e.exFee ? -1 : +(t.exFee > e.exFee)), d = b, f = n, p = r;
			let i = [...u];
			for (; i.length > 1 && d > 0n;) {
				let e = i.pop(), t = n - e.amountBig, a = r - e.ppkfee, o = y(t, a);
				if (o === null) break;
				o < d && (u = [...i], d = o, f = t, p = a, n = t, r = a);
			}
		}
		if (u && d !== null) {
			let e = h(f, p);
			if (e === c || !i && e >= c && e <= D) break;
		}
		if (l.elapsed() > 1e3) {
			k(i, "Proof selection took too long. Try again with a smaller proof set.", a), a.warn("Proof selection took too long. Returning best selection so far.");
			break;
		}
	}
	if (u && d !== null) {
		let e = u.map((e) => e.proof), t = new Set(e), n = o.filter((e) => !t.has(e));
		return a.info(`Proof selection took ${l.elapsed()}ms`), {
			keep: n,
			send: e
		};
	}
	return {
		keep: o,
		send: []
	};
}
function zo(e, t, n, r = !1, i = !1, a = O) {
	let o = Y(e), s = j.from(t), c = s.toBigInt(), l = (e, t) => e - (r ? (t + 999n) / 1000n : 0n), u = (e) => {
		let t = new Set(e.map((e) => e.secret));
		return {
			keep: o.filter((e) => !t.has(e.secret)),
			send: e
		};
	}, d = /* @__PURE__ */ new Set(), f = /* @__PURE__ */ new Map();
	for (let [e, t] of o.entries()) {
		k(d.has(t.secret), `Duplicate proof at index ${e}: each proof may appear only once`, a, { index: e }), d.add(t.secret);
		let r = n.getKeyset(t.id);
		k(!n.isUnitKeyset(t.id), `Proof has unrecognised keyset. '${t.id}' is not a keyset for this wallet unit`, a, { id: t.id });
		let i = (r.hasHexId ? parseInt(t.id.slice(0, 2), 16) + 1 : 0) * 2 + +!!r.isActive, o = f.get(i) ?? {
			proofs: [],
			gross: 0n,
			ppk: 0n
		};
		o.proofs.push(t), o.gross += t.amount.toBigInt(), o.ppk += BigInt(r.fee), f.set(i, o);
	}
	let p = [...f.keys()].sort((e, t) => e - t);
	if (s.isZero() || p.length <= 1) return Ro(o, s, n, r, i, a);
	let m = [], h = 0n, g = 0n, _ = 0;
	for (; _ < p.length; _++) {
		let e = f.get(p[_]), t = l(h + e.gross, g + e.ppk);
		if (t > c) break;
		for (let t of e.proofs) m.push(t);
		if (h += e.gross, g += e.ppk, t === c) return u(m);
	}
	if (_ < p.length) {
		let e = c - l(h, g), t = f.get(p[_]).proofs, o = p.slice(_ + 1).flatMap((e) => f.get(e).proofs), s = o.length ? [t, t.concat(o)] : [t];
		for (let t of s) {
			let o;
			try {
				o = Ro(t, e, n, r, i, a);
			} catch (e) {
				a.debug("selectProofsRotating: biased attempt failed", { error: e });
				continue;
			}
			if (o.send.length === 0) continue;
			let s = h, d = g;
			for (let e of o.send) s += e.amount.toBigInt(), d += BigInt(n.getKeyset(e.id).fee);
			let f = l(s, d);
			if (i ? f === c : f >= c) return u(m.concat(o.send));
		}
	}
	return Ro(o, s, n, r, i, a);
}
//#endregion
//#region src/wallet/SwapPreview.ts
function Bo(e) {
	return {
		...e,
		amount: e.amount.toString()
	};
}
function Vo(e) {
	return {
		amount: e.amount.toString(),
		fees: e.fees.toString(),
		keysetId: e.keysetId,
		inputs: e.inputs.map(Bo),
		...e.sendOutputs && { sendOutputs: e.sendOutputs.map((e) => $.serialize(e)) },
		...e.keepOutputs && { keepOutputs: e.keepOutputs.map((e) => $.serialize(e)) }
	};
}
function Ho(e) {
	try {
		return {
			amount: j.from(e.amount),
			fees: j.from(e.fees),
			keysetId: e.keysetId,
			inputs: Y(e.inputs),
			...e.sendOutputs && { sendOutputs: e.sendOutputs.map((e) => $.deserialize(e)) },
			...e.keepOutputs && { keepOutputs: e.keepOutputs.map((e) => $.deserialize(e)) }
		};
	} catch (e) {
		throw new S(`Invalid SerializedSwapPreview: ${e instanceof Error ? e.message : String(e)}`, { cause: e });
	}
}
//#endregion
//#region src/model/OutputDataCreator.ts
var Uo = class e {
	createP2PKData(t, n, r, i) {
		if (this.createSingleP2PKData === e.prototype.createSingleP2PKData) return $.createP2PKData(t, n, r, i);
		let a = H(n, r.keys, i), o = t.blindKeys && t.sigFlag === "SIG_ALL" ? qt() : void 0, s = a.map((e) => this.createSingleP2PKData(t, e, r.id, o));
		if (o && new Set(s.map((e) => e.ephemeralE)).size > 1) throw new S("createSingleP2PKData override must reuse the shared eBytes ephemeral key for a SIG_ALL split");
		return s;
	}
	createSingleP2PKData(e, t, n, r) {
		return $.createSingleP2PKData(e, t, n, r);
	}
	createRandomData(e, t, n) {
		return H(e, t.keys, n).map((e) => this.createSingleRandomData(e, t.id));
	}
	createSingleRandomData(e, t) {
		return $.createSingleRandomData(e, t);
	}
	createDeterministicData(t, n, r, i, a) {
		if (this.createSingleDeterministicData === e.prototype.createSingleDeterministicData) return $.createDeterministicData(t, n, r, i, a);
		let o = H(t, i.keys, a), s = r + (o.length - 1);
		if (!Number.isSafeInteger(r) || r < 0 || o.length > 0 && !Number.isSafeInteger(s)) throw new S("Counter must be an integer in the range 0 <= counter <= 2^53 - 1");
		return o.map((e, t) => this.createSingleDeterministicData(e, n, r + t, i.id));
	}
	createSingleDeterministicData(e, t, n, r) {
		return $.createSingleDeterministicData(e, t, n, r);
	}
};
//#endregion
//#region src/wallet/_internal.ts
function Wo(e) {
	return e <= 1n ? 0 : (e - 1n).toString(2).length;
}
function Go(e) {
	let t = Object.keys(e).map((e) => j.from(e));
	return t.sort((e, t) => e.compareTo(t)), t;
}
function Ko(e) {
	if (e > 8192) throw new S(`Cannot split amount: optimized split would exceed ${le} outputs`);
}
function qo(e, t, n, r) {
	let i = j.from(t), a = [], o = j.zero(), s = e.map((e) => e.amount);
	for (let e of Go(n)) {
		let t = s.filter((t) => e.equals(t)).length, n = Math.max(r - t, 0);
		for (let t = 0; t < n; ++t) {
			let t = o.add(e);
			if (t.greaterThan(i)) break;
			Ko(a.length + 1), a.push(e), o = t;
		}
	}
	let c = i.subtract(o);
	if (!c.isZero()) {
		let e = H(c, n);
		Ko(a.length + e.length);
		for (let t of e) a.push(t), o = o.add(t);
	}
	return a.sort((e, t) => e.compareTo(t));
}
function Jo(e) {
	switch (e.type) {
		case "custom": return JSON.stringify({
			type: "custom",
			outputs: e.data.length,
			amounts: e.data.map((e) => e.blindedMessage.amount.toString())
		});
		case "factory": return JSON.stringify({
			type: "factory",
			denominations: (e.denominations ?? []).map((e) => j.from(e).toString())
		});
		case "deterministic": return JSON.stringify({
			type: "deterministic",
			counter: e.counter,
			denominations: (e.denominations ?? []).map((e) => j.from(e).toString())
		});
		case "p2pk": {
			let t = e.options, n = t.blindKeys ? {
				...t,
				pubkey: Array.isArray(t.pubkey) ? t.pubkey.map(() => "[redacted]") : "[redacted]",
				refundKeys: t.refundKeys?.map(() => "[redacted]")
			} : t;
			return JSON.stringify({
				type: "p2pk",
				options: n,
				denominations: (e.denominations ?? []).map((e) => j.from(e).toString())
			});
		}
		case "random": return JSON.stringify({
			type: "random",
			denominations: (e.denominations ?? []).map((e) => j.from(e).toString())
		});
		default: return "Unknown";
	}
}
//#endregion
//#region src/wallet/CounterSource.ts
function Yo(e) {
	return e.length === 12 ? e : U(e) ? e.toLowerCase() : e;
}
function Xo(e, t) {
	if (!Number.isSafeInteger(t)) throw new S(`Reservation for counter key ${e} would take the cursor to ${t}, outside the safe integer range`);
}
var Zo = class {
	constructor(e) {
		if (this.next = /* @__PURE__ */ new Map(), this.locks = /* @__PURE__ */ new Map(), e) for (let [t, n] of Object.entries(e)) {
			let e = Yo(t);
			Xo(e, n), this.next.set(e, Math.max(this.next.get(e) ?? 0, n));
		}
	}
	async withLock(e, t) {
		let n = Yo(e), r = this.locks.get(n) ?? Promise.resolve(), i, a = new Promise((e) => i = e), o = r.then(() => a);
		this.locks.set(n, o);
		try {
			return await r, await t(n);
		} finally {
			i(), this.locks.get(n) === o && this.locks.delete(n);
		}
	}
	async reserve(e, t) {
		if (t < 0) throw new S("reserve called with negative count");
		return this.withLock(e, (e) => {
			let n = this.next.get(e) ?? 0;
			return t === 0 ? {
				start: n,
				count: 0
			} : (Xo(e, n + t), this.next.set(e, n + t), {
				start: n,
				count: t
			});
		});
	}
	async reserveAt(e, t, n) {
		if (t < 0 || n < 0) throw new S("reserveAt called with a negative start or count");
		return this.withLock(e, (e) => {
			let r = this.next.get(e) ?? 0;
			if (t < r) throw new S(`Counter ${t} for counter key ${e} was already issued (next is ${r})`);
			return Xo(e, t + n), this.next.set(e, t + n), {
				start: t,
				count: n
			};
		});
	}
	async advanceToAtLeast(e, t) {
		await this.withLock(e, (e) => {
			t > (this.next.get(e) ?? 0) && (Xo(e, t), this.next.set(e, t));
		});
	}
	async setNext(e, t) {
		await this.withLock(e, (e) => {
			if (t < 0) throw new S("setNext: negative next not allowed");
			Xo(e, t), this.next.set(e, t);
		});
	}
	snapshot() {
		return Promise.resolve(Object.fromEntries(this.next.entries()));
	}
};
function Qo(e) {
	return new Zo(e);
}
//#endregion
//#region src/wallet/WalletCounters.ts
var $o = class {
	constructor(e) {
		this.src = e;
	}
	async peekNext(e) {
		return (await this.src.reserve(e, 0)).start;
	}
	async advanceToAtLeast(e, t) {
		await this.src.advanceToAtLeast(e, t);
	}
	async setNext(e, t) {
		if (typeof this.src.setNext == "function") {
			await this.src.setNext(e, t);
			return;
		}
		throw new S("CounterSource does not support setNext()");
	}
	async snapshot() {
		if (typeof this.src.snapshot == "function") return await this.src.snapshot();
		throw new S("CounterSource does not support snapshot()");
	}
};
//#endregion
//#region src/wallet/WalletEvents.ts
function es() {}
function ts(e) {
	let t = /* @__PURE__ */ new WeakSet();
	try {
		return JSON.stringify(e, (e, n) => {
			if (typeof n == "object" && n) {
				if (t.has(n)) return "[Circular]";
				t.add(n);
			}
			return n;
		});
	} catch {
		return Object.prototype.toString.call(e);
	}
}
function ns(e) {
	return {
		...e,
		...e.amount != null && { amount: j.from(e.amount) },
		...e.amount_paid != null && { amount_paid: j.from(e.amount_paid) },
		...e.amount_issued != null && { amount_issued: j.from(e.amount_issued) }
	};
}
function rs(e, t) {
	let n = {
		...e,
		...e.amount != null && { amount: j.from(e.amount) },
		...e.fee_reserve != null && { fee_reserve: j.from(e.fee_reserve) },
		...e.change && { change: e.change.map((e) => ({
			...e,
			amount: j.from(e.amount)
		})) }
	};
	return Ia(n, "bolt11", t, "bolt11 melt quote update"), n;
}
function is(e) {
	return e instanceof Error ? e : new S(typeof e == "string" ? e : ts(e), { cause: e });
}
function as() {
	let e = /* @__PURE__ */ Error("Aborted");
	return Object.defineProperty(e, "name", { value: "AbortError" }), e;
}
function os(e) {
	e && Promise.resolve(e).then((e) => {
		try {
			e();
		} catch {}
	}).catch(() => {});
}
var ss = class {
	constructor(e) {
		this.wallet = e, this.countersReservedHandlers = /* @__PURE__ */ new Set(), this.keychainUpdatedHandlers = /* @__PURE__ */ new Set();
	}
	withAbort(e, t) {
		if (!e) return t;
		if (e.aborted) return t(), es;
		let n = () => t();
		return e.addEventListener("abort", n, { once: !0 }), () => {
			e.removeEventListener("abort", n), t();
		};
	}
	waitUntilPaid(e, t, n, r = "Timeout waiting for paid") {
		return new Promise((i, a) => {
			let o = null, s = null, c = !1, l = (e) => {
				c || (c = !0, os(o), s && (clearTimeout(s), s = null), n?.signal && n.signal.removeEventListener("abort", u), e && a(is(e)));
			}, u = () => l(as());
			if (n?.signal) {
				if (n.signal.aborted) return u();
				n.signal.addEventListener("abort", u, { once: !0 });
			}
			n?.timeoutMs && n.timeoutMs > 0 && (s = setTimeout(() => l(new S(r)), n.timeoutMs)), o = e(t, (e) => {
				l(), i(e);
			}, (e) => l(e), { signal: n?.signal }), o.catch((e) => l(e));
		});
	}
	countersReserved(e, t) {
		return this.countersReservedHandlers.add(e), this.withAbort(t?.signal, () => this.countersReservedHandlers.delete(e));
	}
	_emitCountersReserved(e) {
		for (let t of this.countersReservedHandlers) A(t, e, this.wallet.logger, { event: "countersReserved" });
	}
	keychainUpdated(e, t) {
		return this.keychainUpdatedHandlers.add(e), this.withAbort(t?.signal, () => this.keychainUpdatedHandlers.delete(e));
	}
	_emitKeychainUpdated() {
		if (this.keychainUpdatedHandlers.size === 0) return;
		let e = { cache: this.wallet.keyChain.cache };
		for (let t of this.keychainUpdatedHandlers) A(t, e, this.wallet.logger, { event: "keychainUpdated" });
	}
	async mintQuoteUpdates(e, t, n, r) {
		if (r?.signal?.aborted || (await this.wallet.mint.connectWebSocket(), r?.signal?.aborted)) return es;
		let i = this.wallet.mint.webSocketConnection;
		if (!i) throw new S("Failed to establish WebSocket connection.");
		let a = Array.from(new Set(e)), o = (e) => {
			let r;
			try {
				r = ns(e);
			} catch (e) {
				A(n, is(e), this.wallet.logger);
				return;
			}
			A(t, r, this.wallet.logger, { event: "bolt11_mint_quote" });
		}, s = i.createSubscription({
			kind: "bolt11_mint_quote",
			filters: a
		}, o, n);
		return this.withAbort(r?.signal, () => i.cancelSubscription(s, o));
	}
	async mintQuotePaid(e, t, n, r) {
		return this.mintQuoteUpdates([e], (e) => {
			e.state === Ha.PAID && A(t, e, this.wallet.logger);
		}, n, r);
	}
	async meltQuoteUpdates(e, t, n, r) {
		if (r?.signal?.aborted || (await this.wallet.mint.connectWebSocket(), r?.signal?.aborted)) return es;
		let i = this.wallet.mint.webSocketConnection;
		if (!i) throw new S("Failed to establish WebSocket connection.");
		let a = Array.from(new Set(e)), o = (e) => {
			let r;
			try {
				r = rs(e, this.wallet.logger);
			} catch (e) {
				A(n, is(e), this.wallet.logger);
				return;
			}
			A(t, r, this.wallet.logger, { event: "bolt11_melt_quote" });
		}, s = i.createSubscription({
			kind: "bolt11_melt_quote",
			filters: a
		}, o, n);
		return this.withAbort(r?.signal, () => i.cancelSubscription(s, o));
	}
	async meltQuotePaid(e, t, n, r) {
		return this.meltQuoteUpdates([e], (e) => {
			e.state === Ua.PAID && A(t, e, this.wallet.logger);
		}, n, r);
	}
	async proofStateUpdates(e, t, n, r) {
		if (r?.signal?.aborted) return es;
		for (let t of e) W(t.id, this.wallet.logger);
		if (await this.wallet.mint.connectWebSocket(), r?.signal?.aborted) return es;
		let i = this.wallet.mint.webSocketConnection;
		if (!i) throw new S("Failed to establish WebSocket connection.");
		let a = new TextEncoder(), o = Object.create(null);
		for (let t of e) {
			let e = Bt(a.encode(t.secret)).toHex(!0);
			if (o[e]) throw new S("Duplicate proof secret in proofStateUpdates input");
			o[e] = t;
		}
		let s = Object.keys(o), c = (e) => {
			let n = o[e.Y];
			n && A(t, {
				...e,
				proof: n
			}, this.wallet.logger, { event: "proof_state" });
		}, l = i.createSubscription({
			kind: "proof_state",
			filters: s
		}, c, n);
		return this.withAbort(r?.signal, () => i.cancelSubscription(l, c));
	}
	onceMintPaid(e, t) {
		return this.waitUntilPaid(this.mintQuotePaid.bind(this), e, t, "Timeout waiting for mint paid");
	}
	onceAnyMintPaid(e, t) {
		return new Promise((n, r) => {
			let i = Array.from(new Set(e)), a = /* @__PURE__ */ new Map(), o = null, s = null, c = !1, l = !1, u = (e) => {
				if (!l) {
					l = !0;
					for (let e of a.values()) os(e);
					a.clear(), o && (clearTimeout(o), o = null), t?.signal && t.signal.removeEventListener("abort", d), e && r(is(e));
				}
			}, d = () => u(as());
			if (t?.signal) {
				if (t.signal.aborted) return d();
				t.signal.addEventListener("abort", d, { once: !0 });
			}
			if (t?.timeoutMs && t.timeoutMs > 0 && (o = setTimeout(() => u(new S("Timeout waiting for any mint paid")), t.timeoutMs)), i.length === 0) return u(new S("No quote ids provided"));
			for (let e of i) {
				let r = this.mintQuotePaid(e, (t) => {
					u(), n({
						id: e,
						quote: t
					});
				}, (n) => {
					if (t?.failOnError) {
						u(n);
						return;
					}
					s = n;
					let r = a.get(e);
					r && (os(r), a.delete(e)), c && a.size === 0 && u(s ?? new S("No subscriptions remaining"));
				});
				a.set(e, r), r.catch((n) => {
					if (t?.failOnError) {
						u(n);
						return;
					}
					s = n;
					let r = a.get(e);
					r && (os(r), a.delete(e)), c && a.size === 0 && u(s ?? new S("No subscriptions remaining"));
				});
			}
			c = !0;
		});
	}
	onceMeltPaid(e, t) {
		return this.waitUntilPaid(this.meltQuotePaid.bind(this), e, t, "Timeout waiting for melt paid");
	}
	proofStatesStream(e, t) {
		return async function* () {
			let n = [], r = !1, i = null, a = t?.maxBuffer && t.maxBuffer > 0 ? t.maxBuffer : Infinity, o = t?.drop ?? "oldest", s = () => {
				let e = i;
				i = null, e && e();
			}, c = (e) => {
				if (n.length >= a) if (o === "oldest") {
					let r = n.shift();
					r !== void 0 && A(t?.onDrop, r, this.wallet.logger, { event: "onDrop" }), n.push(e);
				} else {
					A(t?.onDrop, e, this.wallet.logger, { event: "onDrop" });
					return;
				}
				else n.push(e);
				s();
			}, l = null, u = this.proofStateUpdates(e, c, () => {
				r = !0, s();
			}, { signal: t?.signal });
			u.catch((e) => {
				l = is(e), r = !0, s();
			});
			let d = () => {
				r = !0, s();
			};
			try {
				for (t?.signal && (t.signal.aborted ? d() : t.signal.addEventListener("abort", d, { once: !0 })); !r || n.length;) {
					for (; n.length;) yield n.shift();
					if (r) break;
					await new Promise((e) => i = e);
				}
				if (l) throw l;
			} finally {
				os(u), t?.signal && t.signal.removeEventListener("abort", d);
			}
		}.call(this);
	}
	group() {
		let e = [], t = !1, n = (() => {
			if (!t) for (t = !0; e.length;) os(e.pop());
		});
		return n.add = (n) => t ? (os(n), n) : (A(async () => {
			await n;
		}, void 0, this.wallet.logger, {
			event: "group.add",
			reason: "subscription setup failed"
		}), e.push(n), n), Object.defineProperty(n, "cancelled", {
			get: () => t,
			enumerable: !0
		}), n;
	}
}, cs = /* @__PURE__ */ new WeakSet();
function ls(e) {
	let { pubkey: t, refundKeys: n = [], hashlock: r } = In(e);
	for (let e of [t, n].flat()) try {
		P(e);
	} catch (e) {
		throw new S("payment request lock key is not a valid secp256k1 point", { cause: e });
	}
	if (r !== void 0 && !/^[0-9a-f]{64}$/.test(r)) throw new S("payment request hashlock must be a 64-character hex string");
}
var us = class {
	constructor(e) {
		this.wallet = e;
	}
	send(e, t) {
		return new ds(this.wallet, e, t);
	}
	sendToRequest(e, t, n) {
		let r = this.wallet;
		if (e.amount && n !== void 0) throw new S("the request specifies its amount; omit the amount argument");
		let i = e.amount ?? (n === void 0 ? void 0 : j.from(n));
		if (!i) throw new S("amountless payment request: pass the chosen amount");
		if (!e.unit && (e.amount !== void 0 || e.supportedMethods?.length)) throw new S("invalid payment request: unit (u) is required when an amount (a) or supported methods (sm) are set");
		if (e.unit && e.unit !== r.unit) throw new S(`request unit '${e.unit}' does not match wallet unit '${r.unit}'`);
		let a = e.includesMint(r.mint.mintUrl);
		if (e.isMintListStrict && !a) throw new S("this wallet's mint is not in the request's strict mint list");
		let o = j.zero();
		if (e.supportedMethods?.length) {
			let { disabled: t, params: n } = r.getMintInfo().isSupported(5), i = t ? [] : n.filter((e) => e.unit === r.unit).map((e) => e.method);
			if (!e.supportedMethods.some((e) => i.includes(e.method))) throw new S(`mint cannot melt ${r.unit} via any method the request accepts`);
			o = a ? j.zero() : e.feesFor(r.mint.mintUrl, i);
		}
		let s = e.toP2PKOptions();
		if (e.nut10 && !s) throw new S(`cannot honour the request's nut10 lock kind '${e.nut10.kind}'`);
		s && ls(s);
		let c = new ds(r, i.add(o), t);
		return cs.add(c), c.includeFees(!0), s ? c.asLocked(s) : c;
	}
	receive(e) {
		return new fs(this.wallet, e);
	}
	mintBolt11(e, t) {
		return new ps(this.wallet, "bolt11", e, t);
	}
	mintBolt12(e, t) {
		return new ps(this.wallet, "bolt12", e, t);
	}
	mintOnchain(e, t) {
		return new ps(this.wallet, "onchain", e, t);
	}
	meltBolt11(e, t) {
		return new ms(this.wallet, "bolt11", e, t);
	}
	meltBolt12(e, t) {
		return new ms(this.wallet, "bolt12", e, t);
	}
	meltOnchain(e, t) {
		return new hs(this.wallet, e, t);
	}
}, ds = class {
	constructor(e, t, n) {
		this.wallet = e, this.proofs = n, this.config = {}, this.amount = j.from(t);
	}
	assertSendOutputMutable() {
		if (cs.has(this) && this.sendOT) throw new S("the payment request mandates this send output; it cannot be replaced by chaining");
	}
	asRandom(e) {
		return this.assertSendOutputMutable(), this.sendOT = {
			type: "random",
			denominations: e
		}, this;
	}
	asDeterministic(e = 0, t) {
		return this.assertSendOutputMutable(), this.sendOT = {
			type: "deterministic",
			counter: e,
			denominations: t
		}, this;
	}
	asLocked(e, t) {
		this.assertSendOutputMutable();
		let n = e instanceof Io ? e.toOptions() : e;
		return this.sendOT = {
			type: "p2pk",
			options: n,
			denominations: t
		}, this;
	}
	asP2PK(e, t) {
		return this.asLocked(e, t);
	}
	asFactory(e, t) {
		return this.assertSendOutputMutable(), this.sendOT = {
			type: "factory",
			factory: e,
			denominations: t
		}, this;
	}
	asCustom(e) {
		return this.assertSendOutputMutable(), this.sendOT = {
			type: "custom",
			data: e
		}, this;
	}
	keepAsRandom(e) {
		return this.keepOT = {
			type: "random",
			denominations: e
		}, this;
	}
	keepAsDeterministic(e = 0, t) {
		return this.keepOT = {
			type: "deterministic",
			counter: e,
			denominations: t
		}, this;
	}
	keepAsLocked(e, t) {
		let n = e instanceof Io ? e.toOptions() : e;
		return this.keepOT = {
			type: "p2pk",
			options: n,
			denominations: t
		}, this;
	}
	keepAsP2PK(e, t) {
		return this.keepAsLocked(e, t);
	}
	keepAsFactory(e, t) {
		return this.keepOT = {
			type: "factory",
			factory: e,
			denominations: t
		}, this;
	}
	keepAsCustom(e) {
		return this.keepOT = {
			type: "custom",
			data: e
		}, this;
	}
	includeFees(e = !0) {
		if (cs.has(this) && !e) throw new S("a payment request is sent net of input fees; includeFees cannot be disabled");
		return this.config.includeFees = e, this;
	}
	keyset(e) {
		return this.config.keysetId = e, this;
	}
	privkey(e) {
		return this.config.privkey = e, this;
	}
	proofsWeHave(e) {
		return this.config.proofsWeHave = e, this;
	}
	onCountersReserved(e) {
		return this.config.onCountersReserved = e, this;
	}
	offlineExactOnly(e = !1) {
		return this.offlineExact = { requireDleq: e }, this;
	}
	offlineCloseMatch(e = !1) {
		return this.offlineClose = { requireDleq: e }, this;
	}
	async prepare() {
		if (this.offlineExact || this.offlineClose) throw new S("Offline selection has nothing to prepare; call run() instead, or drop the offline mode for an online swap.");
		let e = {
			send: this.sendOT ?? this.wallet.defaultOutputType(),
			...this.keepOT ? { keep: this.keepOT } : {}
		};
		return this.wallet.prepareSwapToSend(this.amount, this.proofs, this.config, e);
	}
	async run() {
		if ((this.offlineExact || this.offlineClose) && (this.sendOT || this.keepOT)) throw new S("Offline selection cannot be combined with custom output types. Remove send/keep output configuration, or use an online swap.");
		if (this.offlineExact) return this.config.privkey && (this.proofs = this.wallet.signP2PKProofs(this.proofs, this.config.privkey)), this.wallet.sendOffline(this.amount, this.proofs, {
			includeFees: this.config.includeFees,
			exactMatch: !0,
			requireDleq: this.offlineExact.requireDleq
		});
		if (this.offlineClose) return this.config.privkey && (this.proofs = this.wallet.signP2PKProofs(this.proofs, this.config.privkey)), this.wallet.sendOffline(this.amount, this.proofs, {
			includeFees: this.config.includeFees,
			exactMatch: !1,
			requireDleq: this.offlineClose.requireDleq
		});
		let e = {
			send: this.sendOT ?? this.wallet.defaultOutputType(),
			...this.keepOT ? { keep: this.keepOT } : {}
		};
		return this.wallet.send(this.amount, this.proofs, this.config, e);
	}
}, fs = class {
	constructor(e, t) {
		this.wallet = e, this.token = t, this.config = {};
	}
	asRandom(e) {
		return this.outputType = {
			type: "random",
			denominations: e
		}, this;
	}
	asDeterministic(e = 0, t) {
		return this.outputType = {
			type: "deterministic",
			counter: e,
			denominations: t
		}, this;
	}
	asLocked(e, t) {
		let n = e instanceof Io ? e.toOptions() : e;
		return this.outputType = {
			type: "p2pk",
			options: n,
			denominations: t
		}, this;
	}
	asP2PK(e, t) {
		return this.asLocked(e, t);
	}
	asFactory(e, t) {
		return this.outputType = {
			type: "factory",
			factory: e,
			denominations: t
		}, this;
	}
	asCustom(e) {
		return this.outputType = {
			type: "custom",
			data: e
		}, this;
	}
	keyset(e) {
		return this.config.keysetId = e, this;
	}
	requireDleq(e = !0) {
		return this.config.requireDleq = e, this;
	}
	privkey(e) {
		return this.config.privkey = e, this;
	}
	proofsWeHave(e) {
		return this.config.proofsWeHave = e, this;
	}
	onCountersReserved(e) {
		return this.config.onCountersReserved = e, this;
	}
	async prepare() {
		return this.wallet.prepareSwapToReceive(this.token, this.config, this.outputType);
	}
	async run() {
		return this.wallet.receive(this.token, this.config, this.outputType);
	}
}, ps = class {
	constructor(e, t, n, r) {
		this.wallet = e, this.method = t, this.quote = r, this.config = {}, this.amount = j.from(n), this._hasPrivkey;
	}
	asRandom(e) {
		return this.outputType = {
			type: "random",
			denominations: e
		}, this;
	}
	asDeterministic(e = 0, t) {
		return this.outputType = {
			type: "deterministic",
			counter: e,
			denominations: t
		}, this;
	}
	asLocked(e, t) {
		let n = e instanceof Io ? e.toOptions() : e;
		return this.outputType = {
			type: "p2pk",
			options: n,
			denominations: t
		}, this;
	}
	asP2PK(e, t) {
		return this.asLocked(e, t);
	}
	asFactory(e, t) {
		return this.outputType = {
			type: "factory",
			factory: e,
			denominations: t
		}, this;
	}
	asCustom(e) {
		return this.outputType = {
			type: "custom",
			data: e
		}, this;
	}
	keyset(e) {
		return this.config.keysetId = e, this;
	}
	privkey(e) {
		return this.config.privkey = e, this;
	}
	proofsWeHave(e) {
		return this.config.proofsWeHave = e, this;
	}
	onCountersReserved(e) {
		return this.config.onCountersReserved = e, this;
	}
	async prepare() {
		if (this.method === "bolt11") {
			let e = this.quote, t = typeof e == "string" ? await this.wallet.checkMintQuoteBolt11(e) : e;
			if (this.wallet.validateMintQuote(t), t.pubkey && !this.config.privkey) throw new S("privkey is required for locked BOLT11 mint quotes");
			return this.wallet.prepareMint(this.method, this.amount, t, this.config, this.outputType);
		}
		if (this.method === "bolt12") {
			let e = this.quote;
			if (this.wallet.validateMintQuote(e), !this.config.privkey) throw new S("privkey is required for BOLT12 mint quotes");
			return this.wallet.prepareMint(this.method, this.amount, e, this.config, this.outputType);
		}
		let e = this.quote;
		if (this.wallet.validateMintQuote(e), !this.config.privkey) throw new S("privkey is required for onchain mint quotes");
		return this.wallet.prepareMint(this.method, this.amount, e, this.config, this.outputType);
	}
	async run() {
		let e = await this.prepare();
		return this.wallet.completeMint(e);
	}
}, ms = class {
	constructor(e, t, n, r) {
		this.wallet = e, this.method = t, this.quote = n, this.proofs = r, this.config = {};
	}
	asRandom(e) {
		return this.outputType = {
			type: "random",
			denominations: e
		}, this;
	}
	asDeterministic(e = 0, t) {
		return this.outputType = {
			type: "deterministic",
			counter: e,
			denominations: t
		}, this;
	}
	asLocked(e, t) {
		let n = e instanceof Io ? e.toOptions() : e;
		return this.outputType = {
			type: "p2pk",
			options: n,
			denominations: t
		}, this;
	}
	asP2PK(e, t) {
		return this.asLocked(e, t);
	}
	asFactory(e, t) {
		return this.outputType = {
			type: "factory",
			factory: e,
			denominations: t
		}, this;
	}
	asCustom(e) {
		return this.outputType = {
			type: "custom",
			data: e
		}, this;
	}
	keyset(e) {
		return this.config.keysetId = e, this;
	}
	privkey(e) {
		return this.config.privkey = e, this;
	}
	onCountersReserved(e) {
		return this.config.onCountersReserved = e, this;
	}
	async prepare() {
		return await this.wallet.prepareMelt(this.method, this.quote, this.proofs, this.config, this.outputType);
	}
	async run() {
		let e = await this.wallet.prepareMelt(this.method, this.quote, this.proofs, this.config, this.outputType);
		return this.wallet.completeMelt(e, this.config.privkey);
	}
}, hs = class {
	constructor(e, t, n) {
		this.wallet = e, this.quote = t, this.proofs = n, this.config = {};
	}
	keyset(e) {
		return this.config.keysetId = e, this;
	}
	privkey(e) {
		return this.config.privkey = e, this;
	}
	feeIndex(e) {
		return this.selectedFeeIndex = e, this;
	}
	async run() {
		if (this.selectedFeeIndex === void 0 && this.quote.fee_options.length === 1 && (this.selectedFeeIndex = this.quote.fee_options[0].fee_index), this.selectedFeeIndex === void 0) throw new S("feeIndex is required when an onchain melt quote has multiple fee options");
		return this.wallet.meltProofsOnchain(this.quote, this.proofs, this.selectedFeeIndex, this.config);
	}
}, gs = "__PENDING__", _s = 20008;
function vs(e) {
	return !!(e.dleq?.r && e.dleq.s && e.dleq.e);
}
var ys = class e {
	constructor(e, t) {
		this._seed = void 0, this._unit = "sat", this._mintInfo = void 0, this._denominationTarget = 3, this._secretsPolicy = "auto", this._boundKeysetId = gs, this._pendingRepair = null, this._lastRepairAt = 0, this._explicitBind = !1, this._requireSigDleq = !1, this._strictCachedKeysets = !1, this.ops = new us(this), this.on = new ss(this), this._logger = t?.logger ?? O, this._selectProofs = t?.selectProofs ?? zo, this._outputDataCreator = t?.outputDataCreator ?? new Uo(), this.mint = typeof e == "string" ? new Oo(e, {
			authProvider: t?.authProvider,
			logger: this._logger
		}) : e, this._unit = t?.unit ?? this._unit, this._boundKeysetId = t?.keysetId ?? this._boundKeysetId, this._explicitBind = t?.keysetId !== void 0, t?.bip39seed && (this.failIf(!(t.bip39seed instanceof Uint8Array), "bip39seed must be a valid Uint8Array", { received: typeof t.bip39seed }), this.failIf(t.bip39seed.length < 16 || t.bip39seed.length > 64, "bip39seed must be 16 to 64 bytes", { length: t.bip39seed.length }), this._seed = t.bip39seed), this._secretsPolicy = t?.secretsPolicy ?? this._secretsPolicy, t?.counterSource ? this._counterSource = t.counterSource : this._counterSource = new Zo(t?.counterInit), this.counters = new $o(this._counterSource), this._keyChain = new Ao(this.mint, this._unit, this._logger), this._denominationTarget = t?.denominationTarget ?? this._denominationTarget, this._requireSigDleq = t?.requireSigDleq ?? this._requireSigDleq, this._strictCachedKeysets = t?.strictCachedKeysets ?? this._strictCachedKeysets;
	}
	fail(e, t) {
		return se(e, this._logger, t);
	}
	failIf(e, t, n) {
		return k(e, t, this._logger, n);
	}
	failIfNullish(e, t, n) {
		return ce(e, t, this._logger, n);
	}
	requireNut29(e, t, n) {
		let r = this._mintInfo?.isSupported(29);
		if (r === void 0) return;
		let i = r.params?.methods;
		return this.failIf(!r.supported || Array.isArray(i) && !i.includes(e), `${t}: mint does not advertise NUT-29 for ${e}; use ${n} per quote`), r.params;
	}
	requireSupport(e, t) {
		this.failIf(!this.getMintInfo().supportsMintMeltMethod(e, t, this._unit), `Mint does not support ${t} ${e} for unit '${this._unit}'`);
	}
	safeCallback(e, t, n) {
		A(e, t, this._logger, n);
	}
	parseAmount(e, t, n = !1) {
		try {
			let r = j.from(e);
			return n || this.failIf(r.isZero(), `Amount must be positive: ${r.toString()}`, {
				op: t,
				amount: e
			}), r;
		} catch (n) {
			let r = n instanceof Error ? n.message : String(n);
			throw this._logger.error(r, {
				op: t,
				amount: e
			}), new S(r, { cause: n });
		}
	}
	async loadMint(e) {
		let t = [];
		(!this._mintInfo || e) && t.push(this.mint.getInfo().then((e) => (this._mintInfo = new Z(e, this._logger), this.mint.setMintInfo(this._mintInfo), null))), t.push(this._keyChain.init(e)), await Promise.all(t), this.finishInit();
	}
	loadMintFromCache(e, t) {
		this._mintInfo = new Z(e, this._logger), this.mint.setMintInfo(this._mintInfo), this._keyChain.loadFromCache(t), this.finishInit();
	}
	finishInit() {
		let { keysets: e, mintUrl: t, savedAt: n } = this._keyChain.cache;
		if (this._logger.debug("KeyChain loaded", {
			mintUrl: t,
			savedAt: n,
			keysets: e.map((e) => ({
				id: e.id,
				unit: e.unit,
				active: e.active,
				fee: e.input_fee_ppk,
				keys: e.keys ? Object.keys(e.keys).length : 0
			}))
		}), this._boundKeysetId === gs) try {
			this._boundKeysetId = this._keyChain.getCheapestKeyset().id;
		} catch (e) {
			this._logger.warn("No active keyset available, wallet remains unbound", {
				unit: this._unit,
				err: e.message
			});
		}
		else if (this._explicitBind) {
			let e = this._keyChain.getKeyset(this._boundKeysetId);
			this.failIf(e.unit !== this._unit, "Keyset unit does not match wallet unit", {
				keyset: e.id,
				unit: e.unit,
				walletUnit: this._unit
			}), W(e.id, this._logger);
		} else {
			let e = this._keyChain.hasKeyset(this._boundKeysetId) ? this._keyChain.getKeyset(this._boundKeysetId) : void 0;
			try {
				let e = this._keyChain.getCheapestKeyset().id;
				e !== this._boundKeysetId && (this._boundKeysetId = e, this._logger.info("Wallet rebound to cheapest active keyset after refresh", { keysetId: e }));
			} catch (t) {
				e || (this._boundKeysetId = gs), this._logger.warn("No active keyset available after refresh", {
					unit: this._unit,
					err: t.message
				});
			}
		}
		this.getMintInfo();
	}
	get keyChain() {
		return this._keyChain;
	}
	get unit() {
		return this._unit;
	}
	getMintInfo() {
		return this.failIfNullish(this._mintInfo, "Mint info not initialized; call loadMint or loadMintFromCache first"), this._mintInfo;
	}
	get maxArrayLength() {
		return this._mintInfo?.maxArrayLength ?? 500;
	}
	get keysetId() {
		if (this._boundKeysetId === gs && this._mintInfo) try {
			this._keyChain.getCheapestKeyset();
		} catch (e) {
			this.fail(`Wallet has no bound keyset: ${e.message}`);
		}
		return this.failIf(this._boundKeysetId === gs, "Wallet has no bound keyset. The mint may have no active keysets, or wallet was not initialized via loadMint or loadMintFromCache"), this._boundKeysetId;
	}
	getKeyset(e) {
		let t = this._keyChain.getKeyset(e ?? this.keysetId);
		return this.failIf(t.unit !== this._unit, "Keyset unit does not match wallet unit", {
			keyset: t.id,
			unit: t.unit,
			walletUnit: this._unit
		}), W(t.id, this._logger), this.failIf(!t.hasKeys, "Keyset has no keys loaded", { keyset: t.id }), t;
	}
	getOutputKeyset(e) {
		let t = this.getKeyset(e);
		return this.failIf(!t.hasHexId, "Legacy keyset cannot be used to create new proofs", { keyset: t.id }), this.failIf(!t.isActive, "Inactive keyset cannot be used to create new proofs", { keyset: t.id }), t;
	}
	async ensureOperableKeysets(e) {
		return this.failIf(!Array.isArray(e), "ensureOperableKeysets: ids must be an array"), this.failIf(!this._mintInfo, "Mint info not initialized; call loadMint or loadMintFromCache first"), this._ensureOperableKeysets(e, { implicit: !1 });
	}
	startRepair(e) {
		if (this._pendingRepair) return this._pendingRepair;
		if (e) {
			if (Date.now() - this._lastRepairAt < 6e4) return null;
			this._lastRepairAt = Date.now();
		}
		return this._pendingRepair = this.loadMint(!0).finally(() => {
			this._pendingRepair = null;
		}), this._pendingRepair;
	}
	async _ensureOperableKeysets(e, t) {
		if (!this._mintInfo) return;
		let n = [...new Set(e.filter((e) => !!e))];
		if (t.implicit && this._strictCachedKeysets) {
			let e = n.filter((e) => !this._keyChain.hasKeyset(e));
			if (e.length > 0) throw new te(e[0]);
			return;
		}
		let r = n.filter((e) => !this._keyChain.hasKeyset(e)), i = !1;
		if (r.length > 0) {
			let e = this.startRepair(t.implicit);
			if (!e) throw new te(r[0]);
			try {
				await e;
			} catch (e) {
				throw new te(r[0], { cause: e });
			}
			i = !0;
			let n = r.filter((e) => !this._keyChain.hasKeyset(e));
			if (n.length > 0) throw t.implicit && this.on._emitKeychainUpdated(), new te(n[0], { refreshed: !0 });
		}
		let a = t.fetchKeys === !1 ? [] : n.filter((e) => this._keyChain.isUnitKeyset(e) && !this._keyChain.getKeyset(e).hasKeys);
		if (a.length > 0) {
			let e = await Promise.allSettled(a.map((e) => this._keyChain.ensureKeysetKeys(e))), n = e.filter((e) => e.status === "rejected");
			if (n.length < e.length && (i = !0), n.length > 0) throw i && t.implicit && this.on._emitKeychainUpdated(), n.length === 1 ? n[0].reason : new S(`Could not load keys for ${n.length} keysets`, { cause: n.map((e) => e.reason) });
		}
		i && t.implicit && this.on._emitKeychainUpdated();
	}
	async repairStaleSnapshot() {
		if (this._strictCachedKeysets) return !1;
		let e = this.startRepair(!0);
		if (!e) return !1;
		try {
			await e;
		} catch (e) {
			return this._logger.warn("Snapshot refresh after a mint keyset rejection failed", { err: e.message }), !1;
		}
		return this.on._emitKeychainUpdated(), !0;
	}
	async withStaleKeysetRepair(e) {
		try {
			return await e();
		} catch (e) {
			throw !oe(e) || !Number.isFinite(e.code) || e.code < 12e3 || e.code >= 13e3 ? e : new re(await this.repairStaleSnapshot(), { cause: e });
		}
	}
	requireMintableKeyset(e) {
		try {
			this._keyChain.getCheapestKeyset();
		} catch (t) {
			this.fail(`${e}: no active keyset for unit '${this._unit}' — a paid mint quote could not be redeemed. ${t.message}`, { reason: t.message });
		}
	}
	get logger() {
		return this._logger;
	}
	async reserveFor(e, t) {
		return t <= 0 ? {
			start: 0,
			count: 0
		} : this._counterSource.reserve(e, t);
	}
	countersNeeded(e) {
		return e.type !== "deterministic" || e.counter !== 0 ? 0 : (e.denominations ?? []).length;
	}
	async addCountersToOutputTypes(e, ...t) {
		let n = t.filter((e) => e.type === "deterministic" && e.counter > 0 && (e.denominations?.length ?? 0) > 0);
		if (n.length > 1) {
			let t = n.map((e) => ({
				start: e.counter,
				end: e.counter + e.denominations.length
			})).sort((e, t) => e.start - t.start);
			for (let n = 1; n < t.length; n++) this.failIf(t[n].start < t[n - 1].end, "Manual counter ranges overlap", {
				keysetId: e,
				prev: t[n - 1],
				cur: t[n]
			});
		}
		if (n.length > 0) {
			let t = Math.min(...n.map((e) => e.counter)), r = Math.max(...n.map((e) => e.counter + e.denominations.length));
			if (this._counterSource.reserveAt) try {
				await this._counterSource.reserveAt(e, t, r - t);
			} catch {
				this._logger.warn("Manual deterministic counter range was already issued", {
					keysetId: e,
					minManualStart: t,
					maxManualEnd: r
				}), await this._counterSource.advanceToAtLeast(e, r);
			}
			else {
				let { start: n } = await this._counterSource.reserve(e, 0);
				n > t && this._logger.warn("Manual deterministic counter range was already issued", {
					keysetId: e,
					minManualStart: t,
					maxManualEnd: r
				}), await this._counterSource.advanceToAtLeast(e, r);
			}
			this._logger.debug("Counter source advanced to respect manual deterministic counters", {
				keysetId: e,
				maxManualEnd: r
			});
		}
		let r = t.reduce((e, t) => e + this.countersNeeded(t), 0);
		if (r === 0) return { outputTypes: t };
		let i = await this.reserveFor(e, r), a = i.start, o = t.map((e) => {
			if (e.type === "deterministic" && e.counter === 0) {
				let t = e.denominations?.length ?? 0;
				if (t > 0) {
					let n = {
						...e,
						counter: a
					};
					return a += t, n;
				}
			}
			return e;
		}), s = {
			keysetId: e,
			start: i.start,
			count: i.count,
			next: i.start + i.count
		};
		return this.on._emitCountersReserved(s), {
			outputTypes: o,
			used: s
		};
	}
	bindKeyset(e) {
		let t = this._keyChain.getKeyset(e);
		this.failIf(t.unit !== this._unit, "Keyset unit does not match wallet unit", {
			keyset: t.id,
			unit: t.unit,
			walletUnit: this._unit
		}), W(t.id, this._logger), this.failIf(!t.hasKeys, "Keyset has no keys loaded", { keyset: t.id }), this._boundKeysetId = t.id, this._explicitBind = !0, this._logger.debug("Wallet bound to keyset", {
			keysetId: t.id,
			unit: t.unit,
			feePPK: t.fee
		});
	}
	withKeyset(t, n) {
		let r = new e(this.mint, {
			keysetId: t,
			bip39seed: this._seed,
			secretsPolicy: this._secretsPolicy,
			outputDataCreator: this._outputDataCreator,
			requireSigDleq: this._requireSigDleq,
			logger: this._logger,
			counterSource: n?.counterSource ?? this._counterSource,
			strictCachedKeysets: this._strictCachedKeysets
		});
		return r.loadMintFromCache(this.getMintInfo().cache, this._keyChain.cache), r;
	}
	defaultOutputType() {
		return this._secretsPolicy === "random" ? { type: "random" } : this._secretsPolicy === "deterministic" ? (this.failIfNullish(this._seed, "Deterministic policy requires a seed"), {
			type: "deterministic",
			counter: 0
		}) : this._seed ? {
			type: "deterministic",
			counter: 0
		} : { type: "random" };
	}
	configureOutputs(e, t, n, r = !1, i = []) {
		let a = this.parseAmount(e, "configureOutputs", !0), o = i.map((e) => ({ amount: j.from(e.amount) }));
		if (n.type === "custom") {
			this.failIf(r, "The custom OutputType does not support automatic fee inclusion");
			let e = this.parseAmount($.sumOutputAmounts(n.data), "configureOutputs.customTotal", !0);
			this.failIf(!e.equals(a), `Custom output data total (${e.toString()}) does not match amount (${a.toString()})`);
			for (let e of n.data) this.getOutputKeyset(e.blindedMessage.id);
			return n;
		}
		let s = n.denominations ?? [];
		if (s.length === 0 && o.length > 0 && (s = qo(o, a, t.keys, this._denominationTarget)), s = H(a, t.keys, s), r) {
			let e = this.receiveFeeAmounts(s.length, t);
			a = a.add(j.sum(e)), s = [...s, ...e];
		}
		return {
			...n,
			denominations: s
		};
	}
	receiveFeeAmounts(e, t) {
		let n = this.getFeesForKeyset(e, t.id), r = H(n, t.keys), i = 0;
		for (; this.getFeesForKeyset(e + r.length, t.id).greaterThan(n);) {
			if (i++ > 4096) throw new S(`Fee calculation for keyset ${t.id} did not converge (input_fee_ppk too high for its denominations)`);
			n = n.add(1), r = H(n, t.keys);
		}
		return r;
	}
	preparedTotal(e) {
		if (e.type === "custom") return $.sumOutputAmounts(e.data);
		let t = e.denominations ?? [];
		return j.sum(t);
	}
	createOutputData(e, t, n) {
		let r = this.parseAmount(e, "createOutputData", !0);
		if (n.type != "custom" && n.denominations && n.denominations.length > 0) {
			let e = j.sum(n.denominations);
			this.failIf(!e.equals(r), "Denominations do not sum to the expected amount", {
				splitSum: e.toString(),
				expected: r.toString()
			});
		}
		let i;
		switch (n.type) {
			case "random":
				i = this._outputDataCreator.createRandomData(r, t, n.denominations);
				break;
			case "deterministic":
				this.failIfNullish(this._seed, "Deterministic outputs require a seed configured in the wallet"), i = this._outputDataCreator.createDeterministicData(r, this._seed, n.counter, t, n.denominations);
				break;
			case "p2pk":
				{
					let e = this.getMintInfo();
					this.failIf(!e.isSupported(11).supported, "Mint does not support NUT-11: the lock would be spendable by anyone"), this.failIf(n.options.hashlock !== void 0 && !e.isSupported(14).supported, "Mint does not support NUT-14: the hashlock would be spendable by anyone");
				}
				n.options.locktime !== void 0 && n.options.locktime <= Math.floor(Date.now() / 1e3) && this._logger.warn("Lock locktime is not in the future", { locktime: n.options.locktime }), i = this._outputDataCreator.createP2PKData(n.options, r, t, n.denominations);
				break;
			case "factory":
				i = H(r, t.keys, n.denominations).map((e) => n.factory(e, t));
				break;
			case "custom": {
				i = n.data;
				let e = this.parseAmount($.sumOutputAmounts(i), "createOutputData.customTotal", !0);
				this.failIf(!e.equals(r), `Custom output data total (${e.toString()}) does not match amount (${r.toString()})`);
				break;
			}
			default: this.fail("Invalid OutputType");
		}
		return i;
	}
	createSwapTransaction(e, t, n = []) {
		e = this._prepareInputsForMint(e);
		let r = [...t, ...n], i = r.map((e, t) => t);
		Zn(e) || i.sort((e, t) => r[e].blindedMessage.amount.compareTo(r[t].blindedMessage.amount));
		let a = [...Array.from({ length: t.length }, () => !0), ...Array.from({ length: n.length }, () => !1)], o = i.map((e) => r[e]), s = i.map((e) => a[e]), c = o.map((e) => e.blindedMessage);
		return {
			payload: {
				inputs: e,
				outputs: c
			},
			outputData: o,
			keepVector: s,
			sortedIndices: i
		};
	}
	async receive(e, t, n) {
		let r = await this.prepareSwapToReceive(e, t, n), { keep: i } = await this.completeSwap(r, t?.privkey);
		return i;
	}
	async prepareSwapToReceive(e, t, n) {
		let { keysetId: r, requireDleq: i, proofsWeHave: a, onCountersReserved: o } = t || {};
		n = n ?? this.defaultOutputType();
		let s;
		if (Array.isArray(e)) s = Y(e);
		else {
			let t = typeof e == "string" ? this.decodeToken(e) : e, n = ua(t.mint);
			this.failIf(n !== this.mint.mintUrl, "Token belongs to a different mint", {
				token: n,
				wallet: this.mint.mintUrl
			}), this.failIf(t.unit !== this._unit, "Token is not in wallet unit", {
				token: t.unit,
				wallet: this._unit
			}), s = Y(t.proofs);
		}
		await this._ensureOperableKeysets(s.map((e) => e.id), { implicit: !0 }), this.assertProofsInWalletUnit(s);
		let c = this.parseAmount(J(s), "prepareSwapToReceive", !0);
		this.failIf(c.isZero(), "Token contains no proofs");
		for (let e of s) {
			let t = this._keyChain.getKeyset(e.id);
			i ? this.failIf(!ga(e, t), "Token contains proofs with invalid or missing DLEQ") : this.failIf(!_a(e, t), "Token contains a proof with an invalid DLEQ");
		}
		let l = this.getOutputKeyset(r), u = this.getFeesForProofs(s), d = c.subtract(u), f = this.configureOutputs(d, l, n, !1, a), p = await this.addCountersToOutputTypes(l.id, f);
		[f] = p.outputTypes, p.used && this.safeCallback(o, p.used, { op: "receive" }), this._logger.debug("receive counter", {
			counter: p.used,
			receiveOT: Jo(f)
		});
		let m = this.createOutputData(this.preparedTotal(f), l, f);
		return {
			amount: d,
			fees: u,
			keysetId: l.id,
			inputs: s,
			keepOutputs: m
		};
	}
	sendOffline(e, t, n) {
		let r = this.parseAmount(e, "sendOffline"), i = Y(t), { requireDleq: a = !1, includeFees: o = !1, exactMatch: s = !0 } = n || {};
		a && (i = i.filter((e) => vs(e))), this.failIf(J(i).lessThan(r), "Not enough funds available to send");
		let { keep: c, send: l } = this.selectProofsToSend(i, r, o, s);
		return {
			keep: c,
			send: this._prepareInputsForMint(l, !0, !0)
		};
	}
	async send(e, t, n, r) {
		let i = this.parseAmount(e, "send"), { keysetId: a, includeFees: o = !1 } = n || {};
		r = r ?? {
			send: this.defaultOutputType(),
			keep: this.defaultOutputType()
		};
		try {
			let e = this.defaultOutputType().type === "deterministic", n = (e) => !e || e.type === "random" && (!e.denominations || e.denominations.length === 0);
			if (a || e || !n(r.send) || r.keep && !n(r.keep)) {
				let t = [];
				throw a && t.push("keysetId override"), e && t.push("wallet default is deterministic"), n(r.send) || t.push("non-default send output type"), r.keep && !n(r.keep) && t.push("non-default keep output type"), new S(`Options require a swap: ${t.join(", ")}`);
			}
			let { keep: s, send: c } = this.sendOffline(i, t, {
				includeFees: o,
				exactMatch: !0,
				requireDleq: !1
			});
			for (let e of c) {
				try {
					F(e.secret);
				} catch (t) {
					if (e.secret.trimStart().startsWith("[")) throw t;
					continue;
				}
				throw new S("A locked legacy proof requires a swap");
			}
			let l = o ? this.getFeesForProofs(c) : j.zero();
			if (J(c).equals(i.add(l))) return this._logger.info("Successful exactMatch offline selection!"), {
				keep: s,
				send: c
			};
		} catch (e) {
			let t = e instanceof Error ? e.message : "Unknown error";
			this._logger.debug("ExactMatch offline selection failed.", { e: t });
		}
		let s = await this.prepareSwapToSend(i, t, n, r);
		return await this.completeSwap(s, n?.privkey);
	}
	async prepareSwapToSend(e, t, n, r) {
		let i = this.parseAmount(e, "prepareSwapToSend"), a = Y(t), { keysetId: o, includeFees: s = !1, onCountersReserved: c } = n || {};
		await this._ensureOperableKeysets(a.map((e) => e.id), {
			implicit: !0,
			fetchKeys: !1
		}), r = r ?? {
			send: this.defaultOutputType(),
			keep: this.defaultOutputType()
		};
		let l = this.getOutputKeyset(o), u = this.configureOutputs(i, l, r.send ?? this.defaultOutputType(), s), d = this.preparedTotal(u), { keep: f, send: p } = this.selectProofsToSend(a, d, !0);
		if (p.length === 0) {
			let e = J(a).toString(), t = this.getFeesForProofs(a).toString();
			this.fail(`Not enough funds available to send: ${e} available, ${d.toString()} required plus up to ${t} input fee`);
		}
		let m = J(p), h = this.getFeesForProofs(p), g = d.add(h);
		m.lessThan(g) && this.failIf(!0, "Not enough funds available for swap", {
			selectedSum: m.toString(),
			required: g.toString()
		});
		let _ = m.subtract(g), v = this.configureOutputs(_, l, r.keep ?? this.defaultOutputType(), !1, n?.proofsWeHave), y = this.preparedTotal(v), b = await this.addCountersToOutputTypes(l.id, u, v);
		[u, v] = b.outputTypes, b.used && this.safeCallback(c, b.used, { op: "send" }), this._logger.debug("send counters", {
			counter: b.used,
			sendOT: Jo(u),
			keepOT: Jo(v)
		});
		let x = this.createOutputData(d, l, u), S = this.createOutputData(y, l, v);
		return {
			amount: i,
			fees: h,
			keysetId: l.id,
			inputs: p,
			sendOutputs: x,
			keepOutputs: S,
			unselectedProofs: f
		};
	}
	async completeSwap(e, t) {
		let n = e?.keepOutputs ? e.keepOutputs : [], r = e.sendOutputs ? e.sendOutputs : [], i = e.unselectedProofs ? e.unselectedProofs : [];
		t && (e.inputs = this.signP2PKProofs(e.inputs, t, [...n, ...r]));
		let a = this.createSwapTransaction(e.inputs, n, r), { signatures: o } = await this.withStaleKeysetRepair(() => this.mint.swap(a.payload));
		this.failIf(o.length !== a.outputData.length, `Mint returned ${o.length} signatures, expected ${a.outputData.length}. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`), this.validateReturnedSignatures(o, a.outputData), await this._ensureKeysetsForSignatures(o);
		let s = a.outputData.map((e, t) => e.toProof(o[t], this.keysetForSignature(o[t].id))), c = Array(s.length), l = Array(a.keepVector.length);
		a.sortedIndices.forEach((e, t) => {
			l[e] = a.keepVector[t], c[e] = s[t];
		});
		let u = [], d = [];
		return c.forEach((e, t) => {
			l[t] ? u.push(e) : d.push(e);
		}), this._logger.debug("SEND COMPLETED", {
			unselectedProofs: i.map((e) => e.amount.toString()),
			keepProofs: u.map((e) => e.amount.toString()),
			sendProofs: d.map((e) => e.amount.toString())
		}), {
			keep: [...u, ...i],
			send: d
		};
	}
	selectProofsToSend(e, t, n = !1, r = !1) {
		let i = this.parseAmount(t, "selectProofsToSend"), { keep: a, send: o } = this._selectProofs(e, i, this._keyChain, n, r);
		return {
			keep: a,
			send: o
		};
	}
	signP2PKProofs(e, t, n, r) {
		let i = Y(e);
		if (!Zn(i)) return Hn(i, t, this._logger);
		this.failIfNullish(n, "OutputData is required for SIG_ALL proof signing."), Yn(i);
		let [a, ...o] = i, s = a, c = [Xn(i, n, r)];
		for (let e of c) s = Hn([s], t, this._logger, e)[0];
		return [s, ...o];
	}
	getFeesForProofs(e) {
		let t = j.sum(e.map((e) => this.getProofFeePPK(e))).toBigInt();
		return j.from((t + 999n) / 1000n);
	}
	isPaymentRequestSatisfied(e, t, n, r) {
		let i = e.amount ?? (n === void 0 ? void 0 : j.from(n));
		if (!i) throw new S("amountless payment request: pass the expected amount to check against");
		if (e.unit && e.unit !== this._unit) throw new S(`request unit '${e.unit}' does not match wallet unit '${this._unit}'`);
		if (this.assertProofsInWalletUnit(t), this.assertNoDuplicateProofs(t), e.nut10) {
			let n = e.toP2PKOptions();
			this.failIfNullish(n, `cannot check the request's nut10 lock kind '${e.nut10.kind}'`);
			for (let e of t) this.assertNut10Lock(n, e.secret);
		} else {
			let e = r?.privkeys === void 0 ? [] : [r.privkeys].flat();
			for (let n of t) this.failIf(!this.canPayeeSpend(n, e), "unlocked request: the payee cannot spend a proof");
		}
		let a = e.includesMint(this.mint.mintUrl);
		this.failIf(e.isMintListStrict === !0 && !a, "this wallet's mint is not in the request's strict mint list");
		let o = j.zero();
		if (e.supportedMethods?.length) {
			let { disabled: t, params: n } = this.getMintInfo().isSupported(5), r = t ? [] : n.filter((e) => e.unit === this._unit).map((e) => e.method);
			this.failIf(!e.supportedMethods.some((e) => r.includes(e.method)), `mint cannot melt ${this._unit} via any method the request accepts`), o = a ? j.zero() : e.feesFor(this.mint.mintUrl, r);
		}
		let s = i.add(o).add(this.getFeesForProofs(t));
		return J(t).compareTo(s) >= 0;
	}
	assertNut10Lock(e, t) {
		let n;
		try {
			Ln(t);
			let [e, r] = F(t), i = Ii({
				kind: e,
				data: r.data,
				tags: r.tags
			});
			n = i && JSON.stringify(In(i));
		} catch {
			n = void 0;
		}
		this.failIfNullish(n, "nut10 request: proof does not carry a valid NUT-10 lock of the requested kind"), this.failIf(n !== JSON.stringify(In(e)), "nut10 request: proof lock is not the requested spending condition");
	}
	canPayeeSpend(e, t) {
		try {
			F(e.secret);
		} catch (t) {
			if (typeof e.secret != "string" || e.secret.trimStart().startsWith("[")) throw t;
			return !0;
		}
		return Rn(e, t);
	}
	assertNoDuplicateProofs(e) {
		let t = /* @__PURE__ */ new Set();
		for (let [n, r] of e.entries()) this.failIf(t.has(r.secret), `Duplicate proof at index ${n}: each proof may appear only once`, { index: n }), t.add(r.secret);
	}
	assertProofsInWalletUnit(e) {
		let t = e.find((e) => !this._keyChain.isUnitKeyset(e.id));
		this.failIf(!!t, `Proof has unrecognised keyset. '${t?.id}' is not a ${this._unit} keyset from this mint`, { id: t?.id });
	}
	getProofFeePPK(e) {
		try {
			return this._keyChain.getKeyset(e.id).fee;
		} catch (t) {
			let n = `Could not get fee. No keyset found for keyset id: ${e.id}`;
			throw this._logger.error(n, {
				e: t,
				keychain: this._keyChain.getKeysets()
			}), new S(n, { cause: t });
		}
	}
	getFeesForKeyset(e, t) {
		let n;
		try {
			n = this._keyChain.getKeyset(t).fee;
		} catch (e) {
			let n = `No keyset found with ID ${t}`;
			throw this._logger.error(n, { e }), new S(n, { cause: e });
		}
		return j.from(e).ceilPercent(n, 1e3);
	}
	getFeesToInclude(e, t) {
		let n = this.getKeyset(t?.keysetId), r = this.parseAmount(e, "getFeesToInclude", !0), i = t?.nOutputs ?? H(r, n.keys).length;
		return j.sum(this.receiveFeeAmounts(i, n));
	}
	maxSpendableAfterFees(e, t = 0) {
		let n = J(e), r = this.getFeesForProofs(e).add(t);
		return r.greaterThanOrEqual(n) ? j.zero() : n.subtract(r);
	}
	_prepareInputsForMint(e, t = !1, n = !1) {
		return e.map((e) => {
			let r = this._normalizeWitness(e), { dleq: i, p2pk_e: a, ...o } = e, s = {
				...o,
				witness: r
			};
			return n && a && (s = {
				...s,
				p2pk_e: a
			}), t && vs(e) && (s = {
				...s,
				dleq: i
			}), s;
		});
	}
	_normalizeWitness(e) {
		if (e.witness) {
			try {
				F(e.secret);
			} catch {
				return;
			}
			return typeof e.witness == "string" ? e.witness : JSON.stringify(e.witness);
		}
	}
	decodeToken(e) {
		return ea(e, this._keyChain.getAllKeysetIds());
	}
	async batchRestore(e = 300, t, n = 0, r) {
		let i = t ?? this.maxArrayLength, a = Math.ceil(e / i), o = r ?? this.keysetId, s = [], c, l = 0;
		for (; l < a;) {
			let e = await this.restore(n, i, { keysetId: o });
			e.proofs.length > 0 || e.lastCounterWithSignature !== void 0 ? (l = 0, s.push(...e.proofs), c = e.lastCounterWithSignature) : l++, n += i;
		}
		return {
			proofs: s,
			lastCounterWithSignature: c
		};
	}
	async restore(e, t, n) {
		this.failIfNullish(this._seed, "Cashu Wallet must be initialized with a seed to use restore");
		let { keysetId: r } = n || {}, i = r ?? this.keysetId;
		this._strictCachedKeysets || await this._keyChain.ensureKeysetKeys(i);
		let a = this.getKeyset(i), o = Array(t).fill(0), s = this._outputDataCreator.createDeterministicData(0, this._seed, e, a, o), { outputs: c, signatures: l } = await this.mint.restore({ outputs: s.map((e) => e.blindedMessage) });
		await this._ensureKeysetsForSignatures(l);
		let u = {};
		c.forEach((e, t) => u[e.B_] = l[t]);
		let d = [], f;
		for (let t = 0; t < s.length; t++) {
			let n = u[s[t].blindedMessage.B_];
			n && (f = e + t, !n.amount.isZero() && d.push(s[t].toProof(n, this.keysetForSignature(n.id))));
		}
		return {
			proofs: d,
			lastCounterWithSignature: f
		};
	}
	async createMintQuote(e, t, n) {
		try {
			this._keyChain.getCheapestKeyset();
		} catch (e) {
			this._logger.warn(`createMintQuote: no active keyset for unit '${this._unit}' — a paid mint quote could not be redeemed. This will become an error in cashu-ts v5.`, { reason: e.message });
		}
		let r = {
			...t,
			unit: this._unit
		}, i = await this.mint.createMintQuote(e, r, { normalize: n?.normalize });
		return this.warnQuoteUnit(i, `createMintQuote: ${e}`), {
			...i,
			unit: i.unit || this._unit
		};
	}
	warnQuoteUnit(e, t) {
		typeof e.unit != "string" || e.unit === "" || e.unit === this._unit || this._logger.warn(`${t}: the mint quoted unit '${e.unit}' but this wallet uses '${this._unit}'. This will become an error in cashu-ts v5.`, {
			quoted: e.unit,
			wallet: this._unit
		});
	}
	warnQuoteAmount(e, t, n) {
		t !== n && this._logger.warn(`${e}: the mint quoted ${n ?? "no amount"} but ${t ?? "no amount"} was requested. This will become an error in cashu-ts v5.`, {
			expected: t,
			quoted: n
		});
	}
	assertBolt11MintQuoteAmount(e, t) {
		if (this.failIf(!e.amount.equals(t), "Mint quote amount does not match", {
			expected: t.toString(),
			quoted: e.amount.toString()
		}), this._unit !== "sat" && this._unit !== "msat") return;
		let n = null;
		try {
			n = wa(e.request);
		} catch {}
		let r = this._unit === "sat" ? t.multiplyBy(1e3) : t, i = n !== null && r.equals(n);
		this.failIf(!i, "Mint quote invoice amount does not match the quote", {
			expected: t.toString(),
			invoiceMsat: n
		});
	}
	async createMintQuoteBolt11(e, t) {
		this.requireSupport("mint", "bolt11"), this.requireMintableKeyset("createMintQuoteBolt11");
		let n = this.parseAmount(e, "createMintQuoteBolt11");
		t && (this.getMintInfo().supportsNut04Description("bolt11", this._unit) || this.fail("Mint does not support description for bolt11"));
		let r = {
			unit: this._unit,
			amount: n,
			description: t
		}, i = await this.mint.createMintQuoteBolt11(r);
		return this.warnQuoteUnit(i, "createMintQuoteBolt11"), this.assertBolt11MintQuoteAmount(i, n), {
			...i,
			unit: i.unit || this._unit
		};
	}
	async createLockedMintQuote(e, t, n) {
		this.requireSupport("mint", "bolt11"), this.requireMintableKeyset("createLockedMintQuote"), this.failIf(typeof t != "string" || t.length === 0, "A pubkey is required to lock the mint quote");
		let r = this.parseAmount(e, "createLockedMintQuote"), { supported: i } = this.getMintInfo().isSupported(20);
		this.failIf(!i, "Mint does not support NUT-20");
		let a = {
			unit: this._unit,
			amount: r,
			description: n,
			pubkey: t
		}, o = await this.mint.createMintQuoteBolt11(a);
		this.warnQuoteUnit(o, "createLockedMintQuote"), this.assertBolt11MintQuoteAmount(o, r), this.failIf(typeof o.pubkey != "string", "Mint returned unlocked mint quote");
		let s = o.pubkey;
		return this.failIf(s.toLowerCase() !== t.toLowerCase(), "Mint quote is not locked to the requested pubkey"), {
			...o,
			pubkey: s,
			unit: o.unit || this._unit
		};
	}
	async createMintQuoteBolt12(e, t) {
		this.requireSupport("mint", "bolt12"), this.requireMintableKeyset("createMintQuoteBolt12"), this.failIf(typeof e != "string" || e.length === 0, "A pubkey is required to lock the mint quote");
		let n = this.getMintInfo();
		t?.description && !n.supportsNut04Description("bolt12", this._unit) && this.fail("Mint does not support description for bolt12");
		let r = t?.amount === void 0 ? void 0 : this.parseAmount(t.amount, "createMintQuoteBolt12"), i = {
			pubkey: e,
			unit: this._unit,
			amount: r,
			description: t?.description
		}, a = await this.mint.createMintQuoteBolt12(i);
		return this.warnQuoteUnit(a, "createMintQuoteBolt12"), this.warnQuoteAmount("createMintQuoteBolt12", r?.toString() ?? null, a.amount?.toString() ?? null), this.failIf(typeof a.pubkey != "string" || a.pubkey.toLowerCase() !== e.toLowerCase(), "Mint quote is not locked to the requested pubkey"), a;
	}
	async createMintQuoteOnchain(e) {
		this.requireSupport("mint", "onchain"), this.requireMintableKeyset("createMintQuoteOnchain"), this.failIf(typeof e != "string" || e.length === 0, "A pubkey is required to lock the mint quote");
		let t = await this.mint.createMintQuoteOnchain({
			unit: this._unit,
			pubkey: e
		});
		return this.warnQuoteUnit(t, "createMintQuoteOnchain"), this.failIf(typeof t.pubkey != "string" || t.pubkey.toLowerCase() !== e.toLowerCase(), "Mint quote is not locked to the requested pubkey"), {
			...t,
			unit: t.unit || this._unit
		};
	}
	async checkMintQuote(e, t, n) {
		let r = typeof t == "string" ? t : t.quote;
		return this.mint.checkMintQuote(e, r, { normalize: n?.normalize });
	}
	async checkMintQuoteBolt11(e) {
		let t = typeof e == "string" ? e : e.quote, n = await this.mint.checkMintQuoteBolt11(t);
		return this.assertBolt11MintQuoteAmount(n, n.amount), n;
	}
	async checkMintQuoteBolt12(e) {
		return this.mint.checkMintQuoteBolt12(e);
	}
	async checkMintQuoteOnchain(e) {
		return this.mint.checkMintQuoteOnchain(e);
	}
	async checkMintQuoteBatch(e, t, n) {
		this.requireNut29(e, "checkMintQuoteBatch", "checkMintQuote");
		let r = t.map((e) => typeof e == "string" ? e : e.quote);
		return this.mint.checkMintQuoteBatch(e, r, { normalize: n?.normalize });
	}
	async checkMintQuoteBatchBolt11(e) {
		this.requireNut29("bolt11", "checkMintQuoteBatchBolt11", "checkMintQuoteBolt11");
		let t = e.map((e) => typeof e == "string" ? e : e.quote), n = await this.mint.checkMintQuoteBatchBolt11(t);
		for (let e of n) this.assertBolt11MintQuoteAmount(e, e.amount);
		return n;
	}
	async checkMintQuoteBatchBolt12(e) {
		this.requireNut29("bolt12", "checkMintQuoteBatchBolt12", "checkMintQuoteBolt12");
		let t = e.map((e) => typeof e == "string" ? e : e.quote);
		return this.mint.checkMintQuoteBatchBolt12(t);
	}
	validateReturnedSignatures(e, t) {
		let n = this._requireSigDleq && (this._mintInfo?.isSupported(12).supported ?? !1);
		for (let r = 0; r < e.length; r++) {
			this.failIf(e[r] == null, `Mint response is missing a signature at index ${r}. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`);
			let i = t[r].blindedMessage.amount.isZero();
			i && e[r].amount.isZero() || (this.failIf(!i && e[r].id !== t[r].blindedMessage.id, `Mint signature keyset id at index ${r} does not match output: expected ${t[r].blindedMessage.id}, got ${e[r].id}. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`), this.failIf(!i && !e[r].amount.equals(t[r].blindedMessage.amount), `Mint returned signature with wrong amount at index ${r}: expected ${t[r].blindedMessage.amount.toString()}, got ${e[r].amount.toString()}. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`), this.failIf(n && !e[r].dleq, `Mint supports NUT-12, but returned a signature without DLEQ proof at index ${r}. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`));
		}
	}
	validateMintQuoteAvailableAmount(e, t, n) {
		if (e !== "bolt12" && e !== "onchain" || !("amount_paid" in t) || !("amount_issued" in t)) return;
		let r = j.from(t.amount_paid), i = j.from(t.amount_issued), a = r.subtract(i);
		this.failIf(n.greaterThan(a), `Mint quote has only ${a.toString()} available to mint; requested ${n.toString()}`, {
			method: e,
			amount_paid: r.toString(),
			amount_issued: i.toString(),
			requestedAmount: n.toString()
		});
	}
	validateMintQuote(e) {
		this.failIf("unit" in e && typeof e.unit == "string" && e.unit !== this.unit, `Quote unit '${e.unit}' does not match wallet unit '${this.unit}'`);
	}
	validateMeltQuote(e) {
		this.failIf("unit" in e && typeof e.unit == "string" && e.unit !== this.unit, `Quote unit '${e.unit}' does not match wallet unit '${this.unit}'`);
	}
	async mintProofs(e, t, n, r, i) {
		let a = await this.prepareMint(e, t, n, r, i);
		return this.completeMint(a);
	}
	async mintProofsBolt11(e, t, n, r) {
		if (this.requireSupport("mint", "bolt11"), typeof t == "string") {
			let i = { quote: t }, a = await this.prepareMint("bolt11", e, i, n, r);
			return this.completeMint(a);
		}
		this.validateMintQuote(t);
		let i = await this.prepareMint("bolt11", e, t, n, r);
		return this.completeMint(i);
	}
	async mintProofsBolt12(e, t, n, r, i) {
		this.requireSupport("mint", "bolt12");
		let a = await this.prepareMint("bolt12", e, t, {
			...r,
			privkey: n
		}, i);
		return this.completeMint(a);
	}
	async mintProofsOnchain(e, t, n, r, i) {
		this.requireSupport("mint", "onchain");
		let a = await this.prepareMint("onchain", e, t, {
			...r,
			privkey: n
		}, i);
		return this.completeMint(a);
	}
	async prepareMint(e, t, n, r, i) {
		this.failIf(typeof n == "string", "prepareMint: expected a quote object, not a string ID. Use mintBolt11() which accepts string quote IDs."), this.validateMintQuote(n);
		let a = this.parseAmount(t, `prepareMint: ${e}`);
		this.validateMintQuoteAvailableAmount(e, n, a), i = i ?? this.defaultOutputType();
		let { privkey: o, keysetId: s, proofsWeHave: c, onCountersReserved: l } = r ?? {}, u = this.getOutputKeyset(s), d = this.configureOutputs(a, u, i, !1, c), f = this.preparedTotal(d), p = await this.addCountersToOutputTypes(u.id, d);
		[d] = p.outputTypes, p.used && this.safeCallback(l, p.used, { op: "mintProofs" }), this._logger.debug("mint counter", {
			counter: p.used,
			mintOT: Jo(d)
		});
		let m = this.createOutputData(f, u, d), h = m.map((e) => e.blindedMessage), g = {
			outputs: h,
			quote: n.quote
		};
		"pubkey" in n && n.pubkey && this.failIf(!o, "Can not sign locked quote without private key");
		let _;
		if (o) {
			let e = "pubkey" in n ? n.pubkey : void 0;
			this.failIf(!e && Array.isArray(o), "prepareMint: multiple privkeys supplied for a quote without pubkey");
			let t = e ? on(e, o) : Array.isArray(o) ? o[0] : o;
			this.failIf(!t, "prepareMint: privkey is empty or correct privkey not provided"), g.signature = Br(t, n.quote, h), _ = Rr(t, n.quote, h);
		}
		return {
			method: e,
			payload: g,
			outputData: m,
			keysetId: u.id,
			quote: n,
			legacySignature: _
		};
	}
	async withLegacyQuoteSigFallback(e, t, n) {
		try {
			return await t();
		} catch (t) {
			if (e && oe(t) && t.code === _s) return this._logger.warn("Mint rejected the amended NUT-20 quote signature (20008); retrying with the legacy message."), n();
			throw t;
		}
	}
	async completeMint(e) {
		let { payload: t, outputData: n, method: r, legacySignature: i } = e, a = {
			...t,
			outputs: n.map((e) => e.blindedMessage)
		}, { signatures: o } = await this.withStaleKeysetRepair(() => this.withLegacyQuoteSigFallback(i !== void 0, () => this.mint.mint(r, a), () => this.mint.mint(r, {
			...a,
			signature: i
		})));
		return this.failIf(o.length !== n.length, `Mint returned ${o.length} signatures, expected ${n.length}. The mint quote may already be marked issued; if the wallet is seeded, try restoring (NUT-09) to recover.`), this.validateReturnedSignatures(o, n), await this._ensureKeysetsForSignatures(o), this._logger.debug("MINT COMPLETED", { amounts: n.map((e) => e.blindedMessage.amount.toString()) }), n.map((e, t) => e.toProof(o[t], this.keysetForSignature(o[t].id)));
	}
	async prepareBatchMint(e, t, n, r) {
		this.failIf(t.length === 0, "prepareBatchMint: no entries provided");
		let i = this.requireNut29(e, "prepareBatchMint", "prepareMint"), a = i?.max_batch_size ?? 100;
		if (t.length > a) {
			let e = i?.max_batch_size == null ? "cashu-ts internal cap" : "mint's advertised limit";
			this.failIf(!0, `prepareBatchMint: batch size ${t.length} exceeds ${e} of ${a}`);
		}
		let { privkey: o, keysetId: s, proofsWeHave: c, onCountersReserved: l } = n ?? {};
		for (let e of t) this.failIf(typeof e.quote == "string", "prepareBatchMint: expected a quote object, not a string ID"), this.validateMintQuote(e.quote);
		t.some((e) => "pubkey" in e.quote && e.quote.pubkey) && this.failIf(!o, "Can not sign locked quotes without private key");
		let u = this.getOutputKeyset(s), d = t.map((t) => this.parseAmount(t.amount, `prepareBatchMint: ${e}`)), f = j.sum(d);
		r = r ?? this.defaultOutputType();
		let p = this.configureOutputs(f, u, r, !1, c), m = await this.addCountersToOutputTypes(u.id, p);
		[p] = m.outputTypes, m.used && this.safeCallback(l, m.used, { op: "mintProofs" });
		let h = this.createOutputData(f, u, p), g = h.map((e) => e.blindedMessage), _ = [], v = [], y = !1;
		for (let [e, n] of t.entries()) {
			let t = "pubkey" in n.quote ? n.quote.pubkey : void 0;
			if (t && o) {
				let e = on(t, o);
				_.push(Br(e, n.quote.quote, g)), v.push(Rr(e, n.quote.quote, g)), y = !0;
			} else o && !t && this._logger.warn(`prepareBatchMint: privkey supplied but quote #${e + 1} has no pubkey; treating as unlocked`), _.push(null), v.push(null);
		}
		return {
			method: e,
			payload: {
				quotes: t.map((e) => e.quote.quote),
				quote_amounts: d,
				outputs: g,
				...y ? { signatures: _ } : {}
			},
			outputData: h,
			keysetId: u.id,
			quotes: t.map((e) => e.quote),
			...y ? { legacySignatures: v } : {}
		};
	}
	async completeBatchMint(e) {
		let { method: t, payload: n, outputData: r, legacySignatures: i } = e, a = {
			...n,
			outputs: r.map((e) => e.blindedMessage)
		}, { signatures: o } = await this.withStaleKeysetRepair(() => this.withLegacyQuoteSigFallback(i !== void 0, () => this.mint.mintBatch(t, a), () => this.mint.mintBatch(t, {
			...a,
			signatures: i
		})));
		return this.failIf(o.length !== r.length, `Mint returned ${o.length} signatures, expected ${r.length}. The mint quote may already be marked issued; if the wallet is seeded, try restoring (NUT-09) to recover.`), this.validateReturnedSignatures(o, r), await this._ensureKeysetsForSignatures(o), this._logger.debug("BATCH MINT COMPLETED", {
			quotes: n.quotes.length,
			amounts: r.map((e) => e.blindedMessage.amount.toString())
		}), r.map((e, t) => e.toProof(o[t], this.keysetForSignature(o[t].id)));
	}
	async createMeltQuote(e, t, n) {
		let r = {
			...t,
			unit: this._unit
		}, i = await this.mint.createMeltQuote(e, r, { normalize: n?.normalize });
		return this.warnQuoteUnit(i, `createMeltQuote: ${e}`), {
			...i,
			unit: i.unit || this._unit
		};
	}
	assertBolt11MeltQuoteAmount(e, t) {
		this.meltQuoteExceeds(e, t) && this.fail("Melt quote amount exceeds the invoice amount", {
			quoted: e.amount.toString(),
			invoiceMsat: j.from(t).toString()
		});
	}
	meltQuoteExceeds(e, t) {
		if (this._unit !== "sat" && this._unit !== "msat" || t === null) return !1;
		let n = j.from(t), r = this._unit === "sat" ? n.ceilPercent(1, 1e3) : n;
		return e.amount.greaterThan(r);
	}
	async createMeltQuoteBolt11(e, t) {
		this.requireSupport("melt", "bolt11");
		let n = t === void 0 ? void 0 : this.parseAmount(t, "createMeltQuoteBolt11");
		t !== void 0 && this.failIf(xa(e), "amountMsat supplied but invoice already contains an amount. Leave amountMsat undefined for non-zero invoices.");
		let r = this._mintInfo?.supportsAmountless?.("bolt11", this._unit) ?? !1, i = {
			unit: this._unit,
			request: e,
			...r && n !== void 0 ? { options: { amountless: { amount_msat: n } } } : {}
		}, a = await this.mint.createMeltQuoteBolt11(i), o = null;
		try {
			o = wa(e);
		} catch {}
		return o === null && n !== void 0 && (o = n), this.warnQuoteUnit(a, "createMeltQuoteBolt11"), this.assertBolt11MeltQuoteAmount(a, o), {
			...a,
			unit: a.unit || this._unit,
			request: a.request || e
		};
	}
	async createMeltQuoteBolt12(e, t) {
		this.requireSupport("melt", "bolt12");
		let n = t === void 0 ? void 0 : this.parseAmount(t, "createMeltQuoteBolt12"), r = await this.mint.createMeltQuoteBolt12({
			unit: this._unit,
			request: e,
			options: n ? { amountless: { amount_msat: n } } : void 0
		});
		return this.warnQuoteUnit(r, "createMeltQuoteBolt12"), this.meltQuoteExceeds(r, n ?? null) && this.warnQuoteAmount("createMeltQuoteBolt12", `at most ${n.toString()} msat`, r.amount.toString()), r;
	}
	async createMeltQuoteOnchain(e, t) {
		this.requireSupport("melt", "onchain");
		let n = this.parseAmount(t, "createMeltQuoteOnchain"), r = await this.mint.createMeltQuoteOnchain({
			unit: this._unit,
			request: e,
			amount: n
		});
		return this.warnQuoteUnit(r, "createMeltQuoteOnchain"), this.warnQuoteAmount("createMeltQuoteOnchain", n.toString(), r.amount.toString()), {
			...r,
			unit: r.unit || this._unit
		};
	}
	async createMultiPathMeltQuote(e, t) {
		let n = this.parseAmount(t, "createMultiPathMeltQuote"), { supported: r, params: i } = this.getMintInfo().isSupported(15);
		this.failIf(!r, "Mint does not support NUT-15"), this.failIf(!i?.some((e) => e.method === "bolt11" && e.unit === this._unit), `Mint does not support MPP for bolt11 and ${this._unit}`);
		let a = {
			unit: this._unit,
			request: e,
			options: { mpp: { amount: n } }
		}, o = await this.mint.createMeltQuoteBolt11(a);
		return this.warnQuoteUnit(o, "createMultiPathMeltQuote"), this.assertBolt11MeltQuoteAmount(o, n), {
			...o,
			request: e,
			unit: this._unit
		};
	}
	async checkMeltQuote(e, t, n) {
		let r = typeof t == "string" ? t : t.quote;
		return this.mint.checkMeltQuote(e, r, { normalize: n?.normalize });
	}
	async checkMeltQuoteBolt11(e) {
		let t = typeof e == "string" ? e : e.quote, n = await this.mint.checkMeltQuoteBolt11(t);
		if (this._unit === "sat" || this._unit === "msat") {
			let e = null;
			try {
				e = wa(n.request);
			} catch {
				this.fail("Melt quote request is not a valid bolt11 invoice");
			}
			this.assertBolt11MeltQuoteAmount(n, e);
		}
		return n;
	}
	async checkMeltQuoteBolt12(e) {
		return this.mint.checkMeltQuoteBolt12(e);
	}
	async checkMeltQuoteOnchain(e) {
		return this.mint.checkMeltQuoteOnchain(e);
	}
	async meltProofs(e, t, n, r, i) {
		let a = await this.prepareMelt(e, t, n, r, i);
		return this.completeMelt(a, r?.privkey);
	}
	async meltProofsBolt11(e, t, n, r) {
		this.requireSupport("melt", "bolt11");
		let i = await this.prepareMelt("bolt11", e, t, n, r);
		return this.completeMelt(i, n?.privkey);
	}
	async meltProofsBolt12(e, t, n, r) {
		this.requireSupport("melt", "bolt12");
		let i = await this.prepareMelt("bolt12", e, t, n, r);
		return this.completeMelt(i, n?.privkey);
	}
	async meltProofsOnchain(e, t, n, r) {
		this.requireSupport("melt", "onchain"), this.validateMeltQuote(e);
		let i = e.fee_options.find((e) => e.fee_index === n);
		this.failIfNullish(i, "feeIndex must match an onchain melt quote fee option", {
			feeIndex: n,
			feeOptions: e.fee_options.map((e) => e.fee_index)
		});
		let a = Y(t);
		await this._ensureOperableKeysets(a.map((e) => e.id), {
			implicit: !0,
			fetchKeys: !1
		});
		let o = this.getFeesForProofs(a), s = J(a), c = e.amount.add(i.fee_reserve).add(o);
		this.failIf(s.lessThan(c), "Not enough proofs to cover amount + fee", {
			sendAmount: s.toString(),
			totalRequired: c.toString(),
			amount: e.amount.toString(),
			fee_reserve: i.fee_reserve.toString(),
			inputFee: o.toString()
		});
		let l = await this.prepareMelt("onchain", e, a, r);
		return await this.completeMelt(l, r?.privkey, { extraPayload: { fee_index: n } });
	}
	async prepareMelt(e, t, n, r, i) {
		this.validateMeltQuote(t), i = i ?? this.defaultOutputType();
		let { keysetId: a, onCountersReserved: o, nut08Change: s = !0 } = r || {};
		try {
			await this._ensureOperableKeysets(n.map((e) => e.id), {
				implicit: !0,
				fetchKeys: !1
			});
		} catch (e) {
			if (this._strictCachedKeysets || !(e instanceof te)) throw e;
			this._logger.warn("Melt input keyset is not listed by the mint; proceeding anyway", { keyset: e.keysetId });
		}
		let c = this.getKeyset(a), l = Y(n), u = J(l), d = [];
		this.failIf(u.lessThan(t.amount), "Not enough proofs to cover amount + fee reserve", {
			sendAmount: u.toString(),
			quoteAmount: t.amount.toString()
		});
		let f = u.subtract(t.amount);
		if (i.type === "custom") d = i.data;
		else if (s && f.greaterThan(0)) {
			this.failIf(!c.isActive, "Melt change keyset is inactive. Use an active keyset or set nut08Change: false to forfeit it", { keyset: c.id }), c = this.getOutputKeyset(a);
			let e = Math.max(Wo(f.toBigInt()), 1), t = Array(e).fill(0);
			this._logger.debug("Creating NUT-08 blanks for fee reserve", {
				feeReserve: f,
				denominations: t
			});
			let n = {
				...i,
				denominations: t
			}, r = await this.addCountersToOutputTypes(c.id, n);
			[n] = r.outputTypes, r.used && this.safeCallback(o, r.used, { op: "meltProofs" }), this._logger.debug("melt counter", {
				counter: r.used,
				meltOT: Jo(n)
			}), d = this.createOutputData(0, c, n);
		}
		return {
			method: e,
			inputs: l,
			outputData: d,
			keysetId: c.id,
			quote: t
		};
	}
	async completeMelt(e, t, n) {
		let r = typeof n == "boolean" ? { preferAsync: n } : n ?? {}, i = e.inputs, a = e.outputData.map((e) => e.blindedMessage), o = e.quote.quote;
		t && (i = this.signP2PKProofs(i, t, e.outputData, o)), i = this._prepareInputsForMint(i);
		let s = r.extraPayload;
		if (s) {
			let e = [
				"quote",
				"inputs",
				"outputs",
				"prefer_async"
			], t = Object.keys(s).filter((t) => e.includes(t));
			this.failIf(t.length > 0, "extraPayload cannot override reserved melt fields", { reserved: t });
		}
		let c = {
			quote: o,
			inputs: i,
			outputs: a,
			...r.preferAsync ? { prefer_async: !0 } : {},
			...s
		}, l = await this.withStaleKeysetRepair(() => this.mint.melt(e.method, c)), u = {
			...e.quote,
			...l
		}, d = l.change ?? [], f;
		try {
			d.length > 0 && await this._ensureOperableKeysets(d.filter((e) => !e.amount.isZero()).map((e) => e.id), { implicit: !0 }), f = this.createMeltChangeProofs(e.outputData, d);
		} catch (t) {
			throw new ne(e.outputData, u, { cause: t });
		}
		let p = f.map((e) => e.amount.toString());
		return r.preferAsync ? this._logger.debug("ASYNC MELT REQUESTED", {
			state: l.state,
			changeAmounts: p
		}) : this._logger.debug("MELT COMPLETED", { changeAmounts: p }), {
			quote: u,
			change: f,
			outputData: f.length > 0 ? [] : e.outputData
		};
	}
	createMeltChangeProofs(e, t) {
		this.failIf(t.length > e.length, `Mint returned ${t.length} signatures, but only ${e.length} blanks were provided. Inputs may already be spent; if the wallet is seeded, try restoring (NUT-09) to recover.`);
		let n = t.map((e) => e && {
			...e,
			amount: j.from(e.amount)
		});
		this.validateReturnedSignatures(n, e);
		let r = n.filter((e) => e.amount.isZero()).length;
		r > 0 && this._logger.warn("Mint returned zero-value change signatures, which NUT-08 requires it to omit", {
			mintUrl: this.mint.mintUrl,
			count: r
		});
		let i = [];
		return n.forEach((t, n) => {
			t.amount.isZero() || i.push(e[n].toProof(t, this.keysetForSignature(t.id)));
		}), i;
	}
	_ensureKeysetsForSignatures(e) {
		return this._ensureOperableKeysets(e.map((e) => e?.amount.isZero() ? void 0 : e?.id), { implicit: !0 });
	}
	keysetForSignature(e) {
		try {
			return this.getKeyset(e);
		} catch (t) {
			throw new S(`Cannot reconstruct proof: keyset ${e} is not loaded in this wallet (may be inactive after rotation). If the wallet is seeded, try restoring (NUT-09) to recover.`, { cause: t });
		}
	}
	async checkProofsStates(e) {
		let t = new TextEncoder(), n = e.map((e) => (e.id !== void 0 && W(e.id, this._logger), Bt(t.encode(e.secret)).toHex(!0))), r = this.maxArrayLength, i = [];
		for (let e = 0; e < n.length; e += r) {
			let t = n.slice(e, e + r), { states: a } = await this.mint.check({ Ys: t }), o = {};
			a.forEach((e) => {
				o[e.Y] = e;
			});
			for (let e = 0; e < t.length; e++) {
				let n = o[t[e]];
				this.failIfNullish(n, "Could not find state for proof with Y: " + t[e]), i.push(n);
			}
		}
		return i;
	}
	async verifyMintSignatures(e, t) {
		if (t?.signal?.aborted) throw new T("Operation aborted by caller");
		if (this.failIf(e.length > ue, `Token contains too many proofs: ${e.length}, maximum is ${ue}`), !this._strictCachedKeysets) for (let n of new Set(e.map((e) => e.id))) {
			if (t?.signal?.aborted) throw new T("Operation aborted by caller");
			try {
				await this._keyChain.ensureKeysetKeys(n);
			} catch {}
		}
		return Pa(e, (e) => this._keyChain.getKeyset(e), t);
	}
	async groupProofsByState(e) {
		let t = await this.checkProofsStates(e), n = {
			unspent: [],
			pending: [],
			spent: []
		};
		for (let r = 0; r < t.length; r++) {
			let i = e[r];
			switch (t[r].state) {
				case Wa.UNSPENT:
					n.unspent.push(i);
					break;
				case Wa.PENDING:
					n.pending.push(i);
					break;
				case Wa.SPENT:
					n.spent.push(i);
					break;
			}
		}
		return n;
	}
}, bs, xs = class e {
	constructor(e, t) {
		this.tokenGeneration = 0, this.sessionGeneration = 0, this.tokens = {}, this.pool = [], this.desiredPoolSize = 100, this.maxPerMint = 100, this.mintUrl = e, this.req = t?.request ?? yo, this.logger = t?.logger ?? O;
		let n = Math.max(1, t?.desiredPoolSize ?? this.desiredPoolSize), r = Math.max(1, t?.maxPerMint ?? this.maxPerMint);
		this.desiredPoolSize = Math.min(n, 100), this.maxPerMint = Math.min(r, 100), this.desiredPoolSize !== n && this.logger.warn("AuthManager: desiredPoolSize exceeds internal cap and was clamped", {
			configured: n,
			clampedTo: this.desiredPoolSize
		}), this.maxPerMint !== r && this.logger.warn("AuthManager: maxPerMint exceeds internal cap and was clamped", {
			configured: r,
			clampedTo: this.maxPerMint
		});
	}
	attachOIDC(e) {
		return this.oidc && this.oidcListener && this.oidc.removeTokenListener(this.oidcListener), this.oidc && this.oidc !== e && (this.tokens = {}), this.tokenGeneration++, this.sessionGeneration++, this.oidc = e, this.bindOIDC(), this;
	}
	bindOIDC() {
		let e = this.oidc;
		if (!e) return;
		this.oidcListener && e.removeTokenListener(this.oidcListener);
		let t = (n, r) => {
			if (this.oidc !== e || r === "refresh" && this.oidcListener !== t) {
				this.logger.warn("AuthManager: ignoring tokens from an earlier session");
				return;
			}
			this.updateFromOIDC(n, r);
		};
		this.oidcListener = t, e.addTokenListener(t);
	}
	get poolSize() {
		return this.pool.length;
	}
	get poolTarget() {
		return this.desiredPoolSize;
	}
	get activeAuthKeysetId() {
		try {
			return this.keychain?.getCheapestKeyset().id;
		} catch {
			return;
		}
	}
	get hasCAT() {
		return !!this.tokens.accessToken;
	}
	getCAT() {
		return this.tokens.accessToken;
	}
	setCAT(e) {
		this.tokenGeneration++, this.sessionGeneration++, this.tokens = { accessToken: e }, this.bindOIDC();
	}
	async ensureCAT(e) {
		if (this.validForAtLeast(e) || !this.oidc || !this.tokens.refreshToken) return this.tokens.accessToken;
		if (this.inflightRefresh?.session !== this.sessionGeneration) {
			let e = this.tokenGeneration, t = this.sessionGeneration, n = (async () => {
				try {
					let t = await this.oidc.refresh(this.tokens.refreshToken);
					this.tokenGeneration === e && this.updateFromOIDC(t, "refresh");
				} catch (e) {
					this.logger.warn("AuthManager: CAT refresh failed", { err: e });
				}
			})();
			this.inflightRefresh = {
				session: t,
				promise: n
			};
		}
		let t = this.inflightRefresh;
		try {
			await t.promise;
		} finally {
			this.inflightRefresh === t && (this.inflightRefresh = void 0);
		}
		return this.validForAtLeast(0) ? this.tokens.accessToken : void 0;
	}
	validForAtLeast(t = e.MIN_VALID_SECS) {
		let { accessToken: n, expiresAt: r } = this.tokens;
		return n ? r ? Date.now() + t * 1e3 < r : !0 : !1;
	}
	updateFromOIDC(e, t) {
		if (!e.access_token) return;
		this.tokenGeneration++, t === "signin" && this.sessionGeneration++;
		let n = Date.now();
		if (this.tokens.accessToken = e.access_token, this.tokens.refreshToken = e.refresh_token || (t === "refresh" ? this.tokens.refreshToken : void 0), typeof e.expires_in == "number" && e.expires_in > 0) this.tokens.expiresAt = n + e.expires_in * 1e3;
		else {
			let t = this.parseJwtExpSec(e.access_token);
			this.tokens.expiresAt = t ? t * 1e3 : void 0;
		}
		this.bindOIDC(), this.logger.debug("AuthManager: OIDC tokens updated", { expiresAt: this.tokens.expiresAt });
	}
	async ensure(e) {
		if (await this.init(), this.pool.length >= e) return;
		let t = Math.max(this.desiredPoolSize, e), n = this.getBatMaxMint(), r = Math.min(t - this.pool.length, n);
		r <= 0 || await this.topUp(r);
	}
	async getBlindAuthToken({ method: e, path: t }) {
		return this.info && !this.info.requiresBlindAuthToken(e, t) && this.logger.warn("Endpoint is not marked as protected by NUT-22; still issuing BAT", { method: e }), this.withLock(async () => {
			if (await this.ensure(1), this.pool.length === 0) throw new S("AuthManager: no BATs available and minting failed");
			let t = this.pool.pop();
			return this.logger.debug("AuthManager: BAT requested", {
				method: e,
				remaining: this.pool.length
			}), Ss(t);
		});
	}
	importPool(e, t = "replace") {
		t === "replace" && (this.pool = []);
		let n = new Map(this.pool.map((e) => [e.secret, e]));
		for (let t of e) !t || !t.secret || !t.C || !t.id || n.has(t.secret) || (this.pool.push(t), n.set(t.secret, t));
	}
	exportPool() {
		return this.pool.map((e) => ({
			...e,
			dleq: e.dleq ? { ...e.dleq } : void 0
		}));
	}
	parseJwtExpSec(e) {
		if (!e) return;
		let t = e.split(".");
		if (t.length === 3) try {
			let e = De(tt(t[1])), n = JSON.parse(e), r = typeof n.exp == "number" ? n.exp : Number(n.exp);
			if (Number.isFinite(r) && r > 0) return r;
		} catch {
			this.logger.warn("JWT access token was malformed.");
		}
	}
	async withLock(e) {
		let t = this.lockChain ?? Promise.resolve(), n, r = new Promise((e) => {
			n = e;
		}), i = t.then(() => r);
		this.lockChain = i;
		try {
			return await t, await e();
		} finally {
			n(), this.lockChain === i && (this.lockChain = void 0);
		}
	}
	async init() {
		if (!this.info) {
			let e = await this.req({
				endpoint: q(this.mintUrl, "/v1/info"),
				method: "GET",
				logger: this.logger
			});
			this.info = new Z(e, this.logger);
		}
		if (!this.keychain) {
			let [e, t] = await Promise.all([this.req({
				endpoint: q(this.mintUrl, "/v1/auth/blind/keysets"),
				method: "GET",
				logger: this.logger
			}), this.req({
				endpoint: q(this.mintUrl, "/v1/auth/blind/keys"),
				method: "GET",
				logger: this.logger
			})]);
			if (!Array.isArray(e.keysets) || !Array.isArray(t.keysets)) throw new S("AuthManager: mint returned malformed auth keysets");
			if (e.keysets.length > 1e4 || t.keysets.length > 1e4) throw new S("AuthManager: mint returned more auth keysets than can be processed");
			let n = e.keysets.map((e) => Ra(e)), r = t.keysets.map((e) => za(e)), i = Ao.mintToCacheDTO(this.mintUrl, n, r);
			this.keychain = Ao.fromCache(this.mintUrl, "auth", i), this.keychain.getCheapestKeyset();
		}
	}
	getBatMaxMint() {
		if (!this.info) throw new S("AuthManager: mint info not loaded");
		let e = this.info.nuts[22], t = X(e?.bat_max_mint, "nuts.22.bat_max_mint", this.maxPerMint);
		return Math.max(1, Math.min(this.maxPerMint, t));
	}
	getActiveKeys() {
		if (!this.keychain) throw new S("AuthManager: keyset not loaded for active keyset");
		return this.keychain.getCheapestKeyset();
	}
	async topUp(e) {
		if (!this.info) throw new S("AuthManager: mint info not loaded");
		let t = this.info.requiresClearAuthToken("POST", "/v1/auth/blind/mint"), n = this.sessionGeneration, r;
		if (t) {
			if (r = await this.ensureCAT(), this.sessionGeneration !== n) throw new S("AuthManager: session changed while obtaining BATs");
			if (!r) throw new S("AuthManager: Clear-auth token required for /v1/auth/blind/mint but not available. Authenticate with the mint to obtain a CAT first.");
		}
		let i = this.getActiveKeys(), a = $.createRandomData(e, i), o = { outputs: a.map((e) => e.blindedMessage) }, s = {};
		r && (s["Clear-auth"] = r);
		let c = await this.req({
			endpoint: q(this.mintUrl, "/v1/auth/blind/mint"),
			method: "POST",
			headers: s,
			...r ? { redirect: "error" } : {},
			requestBody: o,
			logger: this.logger
		});
		if (!Array.isArray(c?.signatures) || c.signatures.length !== a.length) throw new S("AuthManager: bad BAT mint response");
		let l = c.signatures.map((e) => ({
			...e,
			amount: j.from(e.amount)
		})), u = a.map((e, t) => e.toProof(l[t], i));
		for (let e of u) if (!ga(e, i)) throw new S("AuthManager: mint returned BAT with invalid DLEQ");
		if (t && this.sessionGeneration !== n) throw new S("AuthManager: session changed while obtaining BATs");
		this.pool.push(...u), this.logger.debug("AuthManager: performed topUp", {
			minted: u.length,
			pool: this.pool.length
		});
	}
};
bs = xs, bs.MIN_VALID_SECS = 30;
function Ss(e) {
	return `authA${et(o(JSON.stringify({
		id: e.id,
		secret: e.secret,
		C: e.C
	})))}`;
}
//#endregion
//#region src/auth/createAuthWallet.ts
async function Cs(e, t) {
	let n = new xs(e, {
		desiredPoolSize: t?.authPool ?? 10,
		maxPerMint: t?.authPool ?? 10,
		logger: t?.logger
	}), r = new Oo(e, {
		authProvider: n,
		logger: t?.logger
	}), i = await r.oidcAuth({
		...t?.oidc,
		logger: t?.logger
	});
	n.attachOIDC(i);
	let a = new ys(r, {
		authProvider: n,
		logger: t?.logger
	});
	return await a.loadMint(), {
		mint: r,
		auth: n,
		oidc: i,
		wallet: a
	};
}
//#endregion
//#region src/model/SigAll.ts
var ws = "sigallA";
function Ts(e, t, n) {
	return { v0: $t(Xn(e, t.map((e) => ({ blindedMessage: e })), n), !0) };
}
function Es(e) {
	let t = {
		version: e.version,
		type: e.type
	};
	return e.quote && (t.quote = e.quote), t.inputs = e.inputs, t.outputs = e.outputs, e.witness && (t.witness = e.witness), `${ws}${$e(o(M.stringify(t) ?? "{}"))}`;
}
function Ds(e) {
	if (!e.startsWith(ws)) throw new S(`Invalid signing package: must start with "${ws}"`);
	if (e.length - 7 > 1048576) throw new S(`Signing package exceeds ${ge} characters`);
	let t = e.slice(7), n;
	try {
		n = De(tt(t));
	} catch (e) {
		throw new S(`Failed to parse signing package: ${e instanceof Error ? e.message : String(e)}`, { cause: e });
	}
	let r;
	try {
		r = M.parse(n, void 0, { strict: !0 });
	} catch (e) {
		throw new S(`Failed to parse signing package JSON: ${e instanceof Error ? e.message : String(e)}`, { cause: e });
	}
	if (!r || typeof r != "object") throw new S("Signing package must be a JSON object");
	let i = r, a = i.version;
	if (a !== ws) throw new S(`Invalid signing package version: ${a}`);
	let o = i.type;
	if (o !== "swap" && o !== "melt") throw new S(`Invalid signing package type: ${o}`);
	if (o === "melt" && (typeof i.quote != "string" || i.quote.length === 0)) throw new S("Melt signing package requires a quote");
	if (!Array.isArray(i.inputs)) throw new S("Signing package inputs must be an array");
	for (let e = 0; e < i.inputs.length; e++) {
		let t = i.inputs[e];
		if (!t || typeof t != "object") throw new S(`Invalid input at index ${e}`);
		if (typeof t.secret != "string") throw new S(`Input ${e}: secret must be string`);
		if (typeof t.C != "string") throw new S(`Input ${e}: C must be string`);
	}
	if (!Array.isArray(i.outputs)) throw new S("Signing package outputs must be an array");
	for (let e = 0; e < i.outputs.length; e++) {
		let t = i.outputs[e];
		if (!t || typeof t != "object") throw new S(`Invalid output at index ${e}`);
		if (typeof t.amount != "number" && typeof t.amount != "bigint") throw new S(`Output ${e}: amount must be a number or bigint`);
		if (!t.B_ || typeof t.B_ != "string") throw new S(`Output ${e}: B_ invalid`);
		if (!t.id || typeof t.id != "string") throw new S(`Output ${e}: id invalid`);
		t.amount = j.from(t.amount);
	}
	if (i.witness !== void 0) {
		let e = i.witness;
		if (!e || typeof e != "object" || !Array.isArray(e.signatures) || e.signatures.some((e) => typeof e != "string")) throw new S("Signing package witness.signatures must be a string array");
	}
	return {
		version: ws,
		type: o,
		...o === "melt" ? { quote: i.quote } : {},
		inputs: i.inputs.map((e) => ({
			secret: e.secret,
			C: e.C
		})),
		outputs: i.outputs.map((e) => ({
			amount: e.amount,
			id: e.id,
			B_: e.B_
		})),
		...i.witness ? { witness: { signatures: i.witness.signatures } } : {}
	};
}
var Os = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i, ks = /^[0-9a-f]{64}$/i, As = /^0[23][0-9a-f]{64}$/i;
function js(e) {
	if (!As.test(e)) return !1;
	try {
		return P(e), !0;
	} catch {
		return !1;
	}
}
function Ms(e, t) {
	if (!ks.test(t)) throw new S("Private key must be 64 hex characters");
	if ((e.witness?.signatures.length ?? 0) >= 64) throw new S("Witness already at the 64-signature limit");
	if (Yn(e.inputs), e.inputs.forEach((e, t) => {
		if (!js(e.C)) throw new S(`Input ${t}: C must be a compressed secp256k1 point`);
	}), e.outputs.forEach((e, t) => {
		if (!js(e.B_)) throw new S(`Output ${t}: B_ must be a compressed secp256k1 point`);
	}), e.quote !== void 0 && !Os.test(e.quote)) throw new S("Melt quote id must be a UUID");
	Un(e.inputs[0].secret, t);
}
function Ns(e, t) {
	Ms(e, t);
	let n = [en(Ts(e.inputs, e.outputs, e.quote).v0, t)];
	return {
		...e,
		witness: { signatures: [...e.witness?.signatures || [], ...n] }
	};
}
function Ps(e) {
	let t = [...e.keepOutputs || [], ...e.sendOutputs || []];
	return Is("swap", e.inputs, t.map((e) => e.blindedMessage));
}
function Fs(e) {
	return Is("melt", e.inputs, e.outputData.map((e) => e.blindedMessage), e.quote.quote);
}
function Is(e, t, n, r) {
	return {
		version: ws,
		type: e,
		...r ? { quote: r } : {},
		inputs: t.map((e) => ({
			secret: e.secret,
			C: e.C
		})),
		outputs: n
	};
}
function Ls(e, t) {
	let n = zs(t.inputs, e);
	return {
		...t,
		inputs: n
	};
}
function Rs(e, t) {
	let n = zs(t.inputs, e);
	return {
		...t,
		inputs: n
	};
}
function zs(e, t) {
	if (!t.witness?.signatures.length) throw new S("No signatures to merge");
	if (e.length === 0) return e;
	let [n, ...r] = e, i = Vn(n.witness), a = i?.signatures ?? [];
	return [{
		...n,
		witness: {
			...i ?? {},
			signatures: [...a, ...t.witness.signatures]
		}
	}, ...r];
}
var Bs = {
	computeDigests: Ts,
	extractSwapPackage: Ps,
	extractMeltPackage: Fs,
	serializePackage: Es,
	deserializePackage: Ds,
	signPackage: Ns,
	signDigest: en,
	mergeSwapPackage: Ls,
	mergeMeltPackage: Rs
};
//#endregion
export { j as Amount, D as AmountError, Le as AmountWithUnit, ee as AmountWithUnitError, xs as AuthManager, S as CTSError, T as CallerAbortError, Wa as CheckStateEnum, Se as ConsoleLogger, C as HttpResponseError, M as JSONInt, Ao as KeyChain, ko as Keyset, Wt as LEGACY_KEYSET_ID_LENGTH, Io as LockBuilder, ia as MAX_SUPPORTED_KEYSET_VERSION_BYTE, ms as MeltBuilder, ne as MeltChangeError, hs as MeltOnchainBuilder, Ua as MeltQuoteState, Oo as Mint, ps as MintBuilder, Z as MintInfo, ae as MintOperationError, Ha as MintQuoteState, w as NetworkError, Va as OIDCAuth, $ as OutputData, Tn as P2BK_DST, Lo as P2PKBuilder, Fi as PaymentRequest, Vr as PaymentRequestTransportType, ie as RateLimitError, fs as ReceiveBuilder, ds as SendBuilder, Bs as SigAll, An as SigFlags, re as StaleKeysetError, E as UncancellableReadError, te as UnknownKeysetError, Eo as WSConnection, ys as Wallet, $o as WalletCounters, ss as WalletEvents, us as WalletOps, Ut as assertSecpPoint, gn as assertSecretKind, Yn as assertSigAllInputs, Un as assertSignerAuthorised, W as assertSpendableVersion, Wi as bigIntStringify, Xt as blindMessage, wa as bolt11AmountMsat, Ta as bolt11PaymentHash, Ea as bolt11PreimageMatches, Xn as buildP2PKSigAllMessageV0, Rn as canSpendP2PKWithKeys, $t as computeMessageDigest, Qt as constructUnblindedSignature, Cs as createAuthWallet, Jt as createBlindSignature, fr as createDLEQProof, Qo as createEphemeralCounterSource, Or as createHTLCHash, Er as createHTLCsecret, pn as createNewMintKeys, Pn as createP2PKsecret, Yt as createRandomRawBlindedMessage, qt as createRandomSecretKey, hn as createSecret, br as createSecretAndBlindingFactorDeriver, pa as decodePaymentRequest, R as dedupeP2PKPubkeys, vr as deriveBlindingFactor, oa as deriveKeysetId, En as deriveP2BKBlindedPubkeys, On as deriveP2BKSecretKey, Dn as deriveP2BKSecretKeys, _r as deriveSecret, yr as deriveSecretAndBlindingFactor, dn as deserializeMintKeys, fa as deserializeProofs, Ho as deserializeSwapPreview, on as findSigningKey, yn as getDataField, ea as getDecodedToken, ya as getDecodedTokenBinary, Ki as getEncodedToken, va as getEncodedTokenBinary, Mr as getHTLCWitnessPreimage, Li as getKeysetAmounts, Kt as getKeysetIdInt, Ln as getP2PKExpectedWitnessPubkeys, zn as getP2PKSigFlag, Bn as getP2PKWitnessSignatures, fn as getPubKeyFromPrivKey, vn as getSecretData, _n as getSecretKind, Sn as getTag, I as getTagInt, Cn as getTagScalar, bn as getTags, ta as getTokenMetadata, sn as getValidSigners, zi as hasCorrespondingKey, Gn as hasP2PKSignedProof, xn as hasTag, ga as hasValidDleq, Bt as hashToCurve, Vt as hash_e, Vi as hexToNumber, xo as injectWebSocketImpl, xa as invoiceHasAmountInHRP, jr as isHTLCSpendAuthorised, Gt as isLegacyKeysetId, oe as isMintOperationError, G as isObj, Nn as isP2PKKnownTagKey, Zn as isP2PKSigAll, qn as isP2PKSpendAuthorised, K as isRecord, U as isValidHex, q as joinUrls, Jn as maybeDeriveP2BKPrivateKeys, cn as meetsSignerThreshold, Rt as mnemonicToSeedSync, ua as normalizeMintUrl, In as normalizeP2PKOptions, Y as normalizeProofAmounts, la as nullIfUndefined, Hi as numberToHexPadded64, Dr as parseHTLCSecret, L as parseP2PKSecret, F as parseSecret, Vn as parseWitnessData, Ht as pointFromBytes, P as pointFromHex, en as schnorrSignDigest, tn as schnorrSignMessage, rn as schnorrVerifyDigest, nn as schnorrVerifyMessage, Ro as selectProofsRGLI, zo as selectProofsRotating, un as serializeMintKeys, da as serializeProofs, Vo as serializeSwapPreview, ro as setGlobalRequestOptions, Rr as signMintQuote, Wn as signP2PKProof, Hn as signP2PKProofs, ca as sortProofsById, H as splitAmount, ma as stripDleq, J as sumProofs, Zt as unblindSignature, ur as verifyDLEQProof, dr as verifyDLEQProof_reblind, _a as verifyDleqIfPresent, kr as verifyHTLCHash, Ar as verifyHTLCSpendingConditions, zr as verifyMintQuoteSignature, Pa as verifyMintSignatures, Kn as verifyP2PKSpendingConditions, Fa as verifyReceivedProofs, mn as verifyUnblindedSignature, aa as versionName };

//# sourceMappingURL=cashu-ts.es.js.map