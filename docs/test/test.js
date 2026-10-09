// The test builds page: pick a board, connect, and the page installs the
// test build that fits the chip it finds.
//
// esptool-js is used directly rather than through esp-web-tools, which the
// release installer uses, for one reason: the release installer's copy bundles
// esptool-js 0.5, which cannot power the flash on ESP32-P4 v3.1 and v3.2, and
// a test build is the first place a v3 image will ever be flashed.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.js";
import { checkImage, revLabel, sha256 } from "./image.js";

// Changing the baud rate reopens the port, and on P4 v3.x that resets the
// board into a session without the flash powered. The ROM rate is slower on a
// USB bridge and costs nothing on the boards' native USB.
const BAUD = 115200;

const $ = (id) => document.getElementById(id);
const boardSel = $("board");
const goBtn = $("go");

let builds = null;

function log(line) {
  $("log").textContent += line + "\n";
  $("log").scrollTop = $("log").scrollHeight;
}

function status(text, kind = "") {
  $("status").textContent = text;
  $("status").className = kind;
  log(text);
}

function progress(pct) {
  $("bar").classList.toggle("on", pct !== null);
  $("fill").style.width = `${pct ?? 0}%`;
}

async function fetchImage(image) {
  if (typeof image.path !== "string" || !/^firmware\/[\w.-]+\.bin$/.test(image.path)) {
    throw new Error("The test build list names a bad file. Nothing was erased.");
  }
  const r = await fetch(image.path, { cache: "no-store", redirect: "error" });
  if (!r.ok) throw new Error("Could not download the test build. Nothing was erased.");
  const bytes = new Uint8Array(await r.arrayBuffer());
  if (bytes.length !== image.size || (await sha256(bytes)) !== image.sha256) {
    throw new Error("The download does not match the test build list. Reload the page and try again. Nothing was erased.");
  }
  return bytes;
}

async function install() {
  const board = builds.boards[boardSel.value];
  goBtn.disabled = boardSel.disabled = true;
  $("log").textContent = "";
  progress(null);
  let transport = null;
  let writing = false;
  try {
    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      status("No board was picked. Nothing was changed.");
      return;
    }
    status("Connecting…");
    transport = new Transport(port, true);
    const loader = new ESPLoader({
      transport,
      baudrate: BAUD,
      terminal: { clean() {}, writeLine: log, write: log },
    });
    try {
      await loader.main();
    } catch (e) {
      throw new Error(`Could not reach the board (${e.message || e}). Close any other program using it, then hold the BOOT button while you plug it in and try again. Nothing was erased.`);
    }
    if (loader.chip.CHIP_NAME !== "ESP32-P4") {
      throw new Error(`That is an ${loader.chip.CHIP_NAME}, not an ESP32-P4. Nothing was erased.`);
    }
    const rev = await loader.chip.getChipRevision(loader);
    const image = board.images.find((i) => rev >= i.minChipRevision && rev <= i.maxChipRevision);
    if (!image) {
      throw new Error(`Your chip is ESP32-P4 ${revLabel(rev)}, and there is no test build for it yet. Nothing was erased.`);
    }
    status(`Your chip is ESP32-P4 ${revLabel(rev)}. Downloading its test build…`);
    const bytes = await fetchImage(image);
    checkImage(bytes, board, rev);
    // One merged image from 0 to the end of the app. It covers the seed and
    // settings area too, which is why the page says installing replaces them.
    status("Installing. This takes a few minutes; keep the board plugged in.");
    writing = true;
    progress(0);
    await loader.writeFlash({
      fileArray: [{ data: bytes, address: 0 }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_i, written, total) => progress(Math.round((written / total) * 100)),
    });
    await loader.after("hard_reset");
    progress(100);
    status("Done. The board restarts by itself; if the screen stays dark, unplug it and plug it back in.", "ok");
  } catch (e) {
    progress(null);
    const msg = e.message || String(e);
    status(writing && !/Nothing was erased/.test(msg)
      ? `${msg} The install did not finish: press the button and run it again.`
      : msg, "bad");
  } finally {
    if (transport) {
      try { await transport.disconnect(); } catch { /* the port is gone already */ }
    }
    goBtn.disabled = boardSel.disabled = false;
  }
}

function showNote() {
  const board = builds.boards[boardSel.value];
  $("buildnote").textContent = board
    ? `Test build ${board.commit}, made ${board.built}. ${board.note || ""}`.trim()
    : "";
}

async function start() {
  if (!("serial" in navigator)) $("nobrowser").style.display = "block";
  try {
    const r = await fetch("builds.json", { cache: "no-store" });
    if (!r.ok) throw new Error(String(r.status));
    builds = await r.json();
  } catch {
    boardSel.innerHTML = "<option>Could not load the test builds</option>";
    return;
  }
  boardSel.innerHTML = "";
  if (!Array.isArray(builds.boards) || builds.boards.length === 0) {
    boardSel.innerHTML = "<option>No test builds right now</option>";
    return;
  }
  builds.boards.forEach((b, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    o.textContent = b.name;
    boardSel.appendChild(o);
  });
  boardSel.disabled = false;
  boardSel.addEventListener("change", showNote);
  showNote();
  goBtn.disabled = !("serial" in navigator);
  goBtn.addEventListener("click", install);
}

start();
