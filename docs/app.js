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

  if (!supported) {
    lockedButton.textContent = "Connect/install needs Chrome, Brave, or Edge";
  } else if (!verified) {
    lockedButton.textContent = "Connect and install firmware (checking...)";
  } else if (!ack.checked) {
    lockedButton.textContent = "Connect and install firmware (check box first)";
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
  const isSigned = auth.signatureStatus === "signed" || auth.signatureStatus === "signature-verified";
  setReceipt(signatureCheck, auth.signatureLabel || "missing", isSigned ? "ok" : "warn");
  setReceipt(keyCheck, auth.keyLabel || "not published", isSigned ? "ok" : "warn");
}

function renderRelease(release) {
  releaseTitle.textContent = release.version + " (" + release.commit + ")";
  renderAuthenticity(release);
}

async function verifyFirmware() {
  try {
    const [releaseResponse, manifestResponse] = await Promise.all([
      fetch(releaseUrl, { cache: "no-store" }),
      fetch(manifestUrl, { cache: "no-store" })
    ]);
    if (!releaseResponse.ok) throw new Error(`release.json HTTP ${releaseResponse.status}`);
    if (!manifestResponse.ok) throw new Error(`manifest.json HTTP ${manifestResponse.status}`);

    const release = await releaseResponse.json();
    const manifest = await manifestResponse.json();
    renderRelease(release);

    const part = manifest.builds?.[0]?.parts?.[0];
    if (!part || part.offset !== release.browserFirmware.offset || part.path !== release.browserFirmware.path) {
      throw new Error("manifest does not match release metadata");
    }

    setVerifyState("pending", "Hashing firmware");
    setReceipt(hashCheck, "checking", "warn");
    const firmwareResponse = await fetch(`${base}${release.browserFirmware.path}`, { cache: "no-store" });
    if (!firmwareResponse.ok) throw new Error(`firmware HTTP ${firmwareResponse.status}`);
    const firmware = await firmwareResponse.arrayBuffer();
    if (firmware.byteLength !== release.browserFirmware.size) {
      throw new Error(`size mismatch: got ${firmware.byteLength}`);
    }

    const digest = hex(await crypto.subtle.digest("SHA-256", firmware));
    if (digest !== release.browserFirmware.sha256) {
      throw new Error(`hash mismatch: ${digest}`);
    }

    verified = true;
    setVerifyState("ready", "Firmware verified");
    setReceipt(hashCheck, "verified", "ok");
  } catch (error) {
    verified = false;
    setVerifyState("stop", "Verification failed");
    setReceipt(hashCheck, "failed", "stop");
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

ack.addEventListener("change", updateFlashGate);
updateFlashGate();
verifyFirmware();
