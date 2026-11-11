// Exercise the actual vendored install flow with simulated serial sessions.
// Run: node --test tools/test_installer_speed.mjs
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const vendor = new URL("../docs/installer/vendor/esp-web-tools/", import.meta.url);
const helper = await readFile(new URL("flash-speed.js", vendor), "utf8");
const { initializeFlasher } = await import(
  `data:text/javascript;base64,${Buffer.from(helper).toString("base64")}`
);
const bundle = await readFile(new URL("install-dialog-C5LjR_e6.js", vendor), "utf8");
const start = bundle.indexOf("(async(e,t,i,r,o)=>{let a,s;");
const end = bundle.indexOf("})((e=>{this._installState=", start);
assert.ok(start >= 0 && end > start, "locate the bundled installation flow");

async function install({ vendorId = 0x1a86, failure, erase = false } = {}) {
  const events = [], states = [], loaders = [];
  const port = { readable: null, writable: null, getInfo: () => ({ usbVendorId: vendorId }) };
  class Transport {
    constructor(device) { this.device = device; }
    async disconnect() {
      events.push("disconnect");
      port.readable = port.writable = null;
    }
  }
  class Loader {
    constructor(options) {
      Object.assign(this, options);
      this.chip = { CHIP_NAME: "ESP32-P4" };
      loaders.push(this);
    }
    async main() {
      assert.equal(this.romBaudrate, 115200);
      assert.equal(this.baudrate, 115200);
      assert.equal(port.readable, null, "close the previous session before reconnecting");
      events.push("connect");
      port.readable = port.writable = {};
      if (failure === "initial" || (failure === "fallback" && loaders.length === 2)) {
        throw new Error("connection failed");
      }
    }
    async flashId() {
      events.push(`probe:${this.baudrate}`);
      if (this.baudrate === 460800 && ["probe", "fallback"].includes(failure)) {
        throw new Error("fast probe failed");
      }
    }
    async changeBaud() {
      events.push(`speed:${this.baudrate}`);
      if (failure === "change-open") throw new Error("baud command failed");
      if (failure === "change-closed") {
        port.readable = port.writable = null;
        throw new Error("cannot reopen fast port");
      }
    }
    async eraseFlash() { events.push(`erase:${this.baudrate}`); }
    async writeFlash(options) {
      events.push(`write:${this.baudrate}`);
      assert.equal(options.compress, true);
      assert.equal(options.eraseAll, false);
      assert.deepEqual(options.fileArray, [{ data: "firmware", address: 0 }]);
      if (failure === "write") throw new Error("write failed");
      options.reportProgress(0, 8, 8);
    }
  }
  class Reader {
    addEventListener(_, callback) { this.callback = callback; }
    readAsBinaryString(data) { this.result = data; this.callback(); }
  }
  const createFlow = new Function(
    "Br", "jr", "initializeFlasher", "Ua", "window", "location", "fetch", "FileReader", "console",
    `return ${bundle.slice(start + 1, end + 1)};`
  );
  const flow = createFlow(
    Transport, Loader, initializeFlasher, async () => {}, {}, "https://installer.example/",
    async () => ({ ok: true, blob: async () => "firmware" }), Reader,
    { log() {}, error() {} }
  );
  await flow(state => states.push(state), port, "manifest.json",
    { builds: [{ chipFamily: "ESP32-P4", parts: [{ path: "firmware.bin", offset: 0 }] }] }, erase);
  return { events, states, loaders };
}

test("UART flashes at 460800 after probing; erase remains opt-in", async () => {
  const { events, states } = await install();
  assert.deepEqual(events, ["connect", "probe:115200", "speed:460800", "probe:460800", "write:460800", "disconnect"]);
  assert.equal(states.at(-1).state, "finished");
});

test("native USB keeps its existing connection without baud negotiation", async () => {
  const { events } = await install({ vendorId: 0x303a });
  assert.deepEqual(events, ["connect", "probe:115200", "write:115200", "disconnect"]);
});

for (const failure of ["probe", "change-open", "change-closed"]) {
  test(`${failure}: reconnect at 115200 before any erase or write`, async () => {
    const { events, states, loaders } = await install({ failure, erase: true });
    assert.equal(loaders.length, 2);
    assert.ok(states.some(state => state.message === "Reconnecting at a slower speed..."));
    assert.deepEqual(events.slice(-4), ["probe:115200", "erase:115200", "write:115200", "disconnect"]);
    assert.equal(events.filter(event => event.startsWith("write:")).length, 1);
    assert.equal(events.filter(event => event.startsWith("erase:")).length, 1);
    assert.equal(states.at(-1).state, "finished");
  });
}

for (const failure of ["initial", "fallback"]) {
  test(`${failure} connection failure never erases or writes`, async () => {
    const { events, states } = await install({ failure, erase: true });
    assert.ok(!events.some(event => /^(write|erase):/.test(event)));
    assert.equal(states.at(-1).details.error, "failed_initialize");
  });
}

test("write failure is reported without automatically retrying the flash", async () => {
  const { events, states, loaders } = await install({ failure: "write" });
  assert.equal(loaders.length, 1);
  assert.equal(events.filter(event => event.startsWith("write:")).length, 1);
  assert.equal(states.at(-1).details.error, "write_failed");
});
