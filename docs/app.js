const base = "installer/";
const releaseUrl = `${base}release.json`;
const manifestUrl = `${base}manifest.json`;
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
  lockedButton.classList.toggle("is-hidden", ready);

  /* The unsupported slot lives inside installButton, which is hidden whenever
     the gate is shut -- and a browser with no Web Serial can never open it. So
     the one reader who most needs an explanation was the one who never saw it.
     This note is outside the gate. */
  if (browserNote) browserNote.hidden = supported;

  if (!supported) {
    lockedButton.textContent = "This browser can't talk to the device";
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

async function verifyFirmware() {
  try {
    const [release, manifest] = await Promise.all([
      readJson(releaseUrl, "release.json"),
      readJson(manifestUrl, "manifest.json")
    ]);
    renderRelease(release);

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
    if (part.offset !== release.browserFirmware.offset || part.path !== release.browserFirmware.path) {
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
    const firmwareResponse = await readFile(`${base}${release.browserFirmware.path}`, "the firmware");
    const firmware = await firmwareResponse.arrayBuffer();
    if (firmware.byteLength !== release.browserFirmware.size) {
      throw mismatchError(`size mismatch: got ${firmware.byteLength}`);
    }

    const digest = hex(await crypto.subtle.digest("SHA-256", firmware));
    if (digest !== release.browserFirmware.sha256) {
      throw mismatchError(`hash mismatch: ${digest}`);
    }

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
  }
  updateFlashGate();
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
  updateFlashGate();
  verifyFirmware();
}
