const base = "installer/";
const releaseUrl = `${base}release.json`;
const boardPick = document.querySelector("#board-pick");
const lockedButton = document.querySelector("#locked-button");
const installButton = document.querySelector("#install-button");
const ack = document.querySelector("#ack");
const verifyLight = document.querySelector("#verify-light");
const verifyTitle = document.querySelector("#verify-title");
const releaseTitle = document.querySelector("#release-title");
const hashCheck = document.querySelector("#hash-check");
const signatureCheck = document.querySelector("#signature-check");
const keyCheck = document.querySelector("#key-check");
const browserNote = document.querySelector("#browser-note");
let verified = false;
// The board the reader picked, by id, and whether the release has an image for
// it. Nothing is hashed and nothing is offered until a board is picked: both
// boards are ESP32-P4, so nothing the browser can see would stop one board's
// image being flashed onto the other.
let pickedBoard = null;
let noImage = false;
// Every pick starts a new check, and a check still hashing the board picked
// before it must not be the one that opens the gate. Each check compares its
// own number against this one after every wait.
let verifyRun = 0;
let releaseRead = null;

const hex = (buffer) =>
  [...new Uint8Array(buffer)].map((byte) => byte.toString(16).padStart(2, "0")).join("");

function setVerifyState(state, title) {
  verifyLight.classList.remove("ready", "stop");
  if (state !== "pending") verifyLight.classList.add(state);
  verifyTitle.textContent = title;
}

function updateFlashGate() {
  const supported = "serial" in navigator && window.isSecureContext;
  const ready = verified && ack.checked && supported;

  installButton.classList.toggle("is-hidden", !ready);
  /* On a browser with no Web Serial there is nothing to press, so the dead
     button goes rather than repeating what the note underneath already
     says. */
  lockedButton.classList.toggle("is-hidden", ready || !supported);

  /* The unsupported slot lives inside installButton, which is hidden whenever
     the gate is shut -- and a browser with no Web Serial can never open it. So
     the one reader who most needs an explanation was the one who never saw it.
     This note is outside the gate. */
  if (browserNote) browserNote.hidden = supported;

  if (!pickedBoard) {
    lockedButton.textContent = "Connect and install (pick your board first)";
  } else if (noImage) {
    lockedButton.textContent = "Connect and install (no image for this board)";
  } else if (!verified) {
    lockedButton.textContent = "Connect and install (verifying...)";
  } else if (!ack.checked) {
    lockedButton.textContent = "Connect and install (tick the box first)";
  }
}

function setReceipt(el, text, state) {
  if (!el) return;
  el.textContent = text;
  el.classList.remove("ok", "warn", "stop");
  if (state) el.classList.add(state);
}

function renderAuthenticity(release) {
  const auth = release.authenticity || {};
  // A signature FILE being present is not the same as this browser having
  // verified it. We do not do in-browser GPG verification, so the honest state
  // is "amber: signature present, verify yourself", never a green "verified".
  const hasSig = auth.signed === true ||
    (typeof auth.signatureStatus === "string" &&
     auth.signatureStatus.length > 0 &&
     !auth.signatureStatus.startsWith("unsigned"));
  setReceipt(signatureCheck,
    hasSig ? (auth.signatureLabel || "signed") + " (verify yourself)" : "unsigned",
    hasSig ? "warn" : "stop");
  // key receipt: prefer the GPG fingerprint (that's the trust anchor users
  // cross-check), fall back to a minisign public key, else say not published
  const keyId = auth.gpgFingerprint || auth.publicKey;
  if (keyId) {
    setReceipt(keyCheck, shortHex(keyId), hasSig ? "ok" : "warn");
    if (keyCheck) {
      keyCheck.title = keyId + " (tap to copy, cross-check via a second channel)";
      keyCheck.style.cursor = "copy";
      keyCheck.onclick = () => navigator.clipboard?.writeText(keyId);
    }
  } else {
    setReceipt(keyCheck, auth.keyLabel || "not published", "warn");
  }
}

function shortHex(s) {
  return s.length > 20 ? s.slice(0, 8) + "…" + s.slice(-8) : s;
}

// A file that is missing and a file that has been changed are two different
// accusations, and only one of them is about trust. This page also ships inside
// the offline installer zip, where the likeliest faults by a wide margin are a
// half unzipped folder or a server started one directory too high. Telling that
// person their download looks tampered with is both wrong and frightening, so
// anything that failed to arrive is tagged, and only real size or hash
// disagreements keep the tampering wording.
function readError(message) {
  const error = new Error(message);
  error.kind = "read";
  return error;
}

// The disagreements that ARE about trust: the file is not the size or the
// hash the release published, or the manifest points somewhere else. Tagged
// for the same reason read failures are, and it matters more here: anything
// the catch below cannot place used to be reported as tampering, so one
// unrelated bug in this file accused the release of being corrupt on every
// visit. It did exactly that, live, after the receipt rows were removed from
// the page and three lines here went on writing to them.
function mismatchError(message) {
  const error = new Error(message);
  error.kind = "mismatch";
  return error;
}

async function readFile(url, label) {
  let response;
  try {
    response = await fetch(url, { cache: "no-store" });
  } catch (cause) {
    throw readError(`${label} could not be fetched`);
  }
  if (!response.ok) throw readError(`${label} HTTP ${response.status}`);
  return response;
}

async function readJson(url, label) {
  const response = await readFile(url, label);
  try {
    return await response.json();
  } catch (cause) {
    throw readError(`${label} is not readable JSON`);
  }
}

function renderRelease(release) {
  releaseTitle.textContent = release.version + " (" + release.commit + ")";
  renderAuthenticity(release);
}

// One image per board. A release made before there was a second board has no
// "boards" list, and everything it describes is the Guition's: its top-level
// browserFirmware and manifest.json.
function releaseBoards(release) {
  if (Array.isArray(release.boards)) return release.boards;
  return [{ id: "guition", manifest: "manifest.json", browserFirmware: release.browserFirmware }];
}

async function verifyFirmware(boardId) {
  const run = ++verifyRun;
  const current = () => run === verifyRun;
  verified = false;
  noImage = false;
  updateFlashGate();
  setVerifyState("pending", "Verifying the binary before install");
  setReceipt(hashCheck, "checking", "warn");
  // The receipt copies the digest it last showed; a digest from the board
  // picked before this one is not a receipt for this one.
  if (hashCheck) {
    hashCheck.title = "";
    hashCheck.style.cursor = "";
    hashCheck.onclick = null;
  }
  try {
    // release.json is read once; a failed read is retried on the next pick.
    releaseRead = releaseRead || readJson(releaseUrl, "release.json");
    let release;
    try {
      release = await releaseRead;
    } catch (error) {
      releaseRead = null;
      throw error;
    }
    if (!current()) return;
    renderRelease(release);

    const entry = releaseBoards(release).find((b) => b && b.id === boardId);
    if (!entry) {
      // Not a fault in anything that was downloaded, so not red: the release
      // simply carries no image for this board, and there is nothing to flash.
      noImage = true;
      setVerifyState("pending", "This release has no image for that board");
      setReceipt(hashCheck, "no image", "warn");
      return;
    }
    // The manifest name comes out of release.json and ends up in a URL, so it
    // has to be a plain file beside release.json and nothing else.
    if (typeof entry.manifest !== "string" || !/^manifest[\w-]*\.json$/.test(entry.manifest)) {
      throw mismatchError("release metadata names no manifest for this board");
    }
    const manifestUrl = `${base}${entry.manifest}`;
    const manifest = await readJson(manifestUrl, entry.manifest);
    if (!current()) return;
    const image = entry.browserFirmware || {};

    // Check the WHOLE flash list, not just the first entry. esp-web-tools
    // writes every part at its own declared offset and resolves each path
    // against the manifest URL, so a path may be absolute and cross origin.
    // Validating parts[0] alone let an appended part carry arbitrary bytes to
    // an arbitrary offset while this page still went green: manifest.json is
    // not listed in SHA256SUMS, so the PGP signature covers none of it.
    if (!Array.isArray(manifest.builds) || manifest.builds.length !== 1) {
      throw mismatchError("manifest declares more than one build");
    }
    const parts = manifest.builds[0].parts;
    if (!Array.isArray(parts) || parts.length !== 1) {
      throw mismatchError("manifest declares more than one flash part");
    }
    const part = parts[0];
    if (part.offset !== image.offset || part.path !== image.path) {
      throw mismatchError("manifest does not match release metadata");
    }
    // An absolute path in a part silently overrides `base`. Resolve it the way
    // esp-web-tools will and require it to stay on this origin.
    // Both arguments have to be absolute: manifestUrl is a relative string,
    // and `new URL(relative, relative)` throws rather than resolving. It threw
    // on every load, and the catch below called that a hash mismatch, so the
    // page told every visitor the firmware was bad and hid the button.
    const partUrl = new URL(part.path, new URL(manifestUrl, location.href));
    if (partUrl.origin !== location.origin) {
      throw mismatchError("manifest part is not same origin");
    }

    setVerifyState("pending", "Hashing firmware");
    setReceipt(hashCheck, "checking", "warn");
    const firmwareResponse = await readFile(`${base}${image.path}`, "the firmware");
    const firmware = await firmwareResponse.arrayBuffer();
    if (!current()) return;
    if (firmware.byteLength !== image.size) {
      throw mismatchError(`size mismatch: got ${firmware.byteLength}`);
    }

    const digest = hex(await crypto.subtle.digest("SHA-256", firmware));
    if (!current()) return;
    if (digest !== image.sha256) {
      throw mismatchError(`hash mismatch: ${digest}`);
    }

    // The button flashes whatever manifest it names when it is pressed, so it
    // is pointed at this board's only once this board's image has checked out.
    installButton.setAttribute("manifest", manifestUrl);
    verified = true;
    setVerifyState("ready", "This file matches the published release");
    // show the actual hash, not just a verdict; tap to copy the full digest
    setReceipt(hashCheck, shortHex(digest), "ok");
    if (hashCheck) {
      hashCheck.title = digest;
      hashCheck.style.cursor = "copy";
      hashCheck.onclick = () => navigator.clipboard?.writeText(digest);
    }
  } catch (error) {
    if (!current()) return;
    verified = false;
    // A silent catch is why the page spent a release telling everyone the
    // firmware was bad without saying which line decided that.
    console.error("verifyFirmware:", error);
    if (error && error.kind === "read") {
      setVerifyState("stop", "Could not read the install files in this folder");
      setReceipt(hashCheck, "not read", "warn");
    } else if (error && error.kind === "mismatch") {
      setVerifyState("stop", "This file does not match the release, do not flash it");
      setReceipt(hashCheck, "failed", "stop");
    } else {
      setVerifyState("stop", "The check did not finish, so this page will not offer the button");
      setReceipt(hashCheck, "not checked", "warn");
    }
    // release.json is what fills these in, so a failure before it lands leaves
    // both rows saying "checking" forever, which reads as a check still running
    [signatureCheck, keyCheck].forEach((el) => {
      if (el && el.textContent === "checking") setReceipt(el, "not read", "warn");
    });
  } finally {
    if (current()) updateFlashGate();
  }
}

document.querySelectorAll(".motion-link, button").forEach((el) => {
  el.addEventListener("click", () => {
    el.classList.remove("is-clicked");
    void el.offsetWidth;
    el.classList.add("is-clicked");
    window.setTimeout(() => el.classList.remove("is-clicked"), 240);
  });
});

const revealEls = document.querySelectorAll(".reveal");
if ("IntersectionObserver" in window) {
  const observer = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-visible");
          observer.unobserve(entry.target);
        }
      });
    },
    { threshold: 0.14 }
  );
  revealEls.forEach((el) => observer.observe(el));
} else {
  revealEls.forEach((el) => el.classList.add("is-visible"));
}

// The install card is pulled whenever the staged installer artifacts do not
// match VERSION, so every element below this point is optional. The reveal and
// click behaviour above is not, which is why the bail comes here rather than at
// the top of the file. Restoring the button is then a pure HTML change: put the
// card back and this block starts running again on its own.
if (ack && installButton && lockedButton && verifyLight && verifyTitle) {
  ack.addEventListener("change", updateFlashGate);
  if (!boardPick) {
    // A card from before the board question, a cached copy of the page for
    // one, only ever offered the Guition's image, so that is what it checks.
    pickedBoard = "guition";
    verifyFirmware(pickedBoard);
  } else {
    boardPick.addEventListener("change", (event) => {
      if (!event.target || event.target.name !== "board" || !event.target.checked) return;
      pickedBoard = event.target.value;
      verifyFirmware(pickedBoard);
    });
    // A browser restoring the form on back or reload can bring a pick with it.
    const restored = boardPick.querySelector('input[name="board"]:checked');
    if (restored) {
      pickedBoard = restored.value;
      verifyFirmware(pickedBoard);
    } else {
      setVerifyState("pending", "Pick your board to check its firmware");
      updateFlashGate();
    }
  }
}
