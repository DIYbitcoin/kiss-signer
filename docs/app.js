const captions = {
  "wallet-home.png": "Wallet home: receive, sign, export, settings.",
  "sign-verify.png": "Sign flow: every output must be visible before approval.",
  "export-descriptor.png": "Descriptor export: watch-only pairing without leaking keys.",
  "game-menu.png": "Decoy surface: FRUIT ISLAND stays first impression."
};

const heroShot = document.querySelector("#hero-shot");
const caption = document.querySelector("#shot-caption");

document.querySelectorAll("[data-shot]").forEach((button) => {
  button.addEventListener("click", () => {
    const file = button.dataset.shot;
    document.querySelectorAll("[data-shot]").forEach((tab) => {
      const active = tab === button;
      tab.classList.toggle("is-active", active);
      tab.setAttribute("aria-selected", String(active));
    });
    heroShot.src = `media/${file}`;
    heroShot.alt = captions[file] || "KISS Wallet simulator screenshot";
    caption.textContent = captions[file] || "";
  });
});

document.querySelectorAll("[data-copy]").forEach((button) => {
  button.addEventListener("click", async () => {
    const target = document.querySelector(button.dataset.copy);
    if (!target) return;

    const original = button.textContent;
    try {
      await navigator.clipboard.writeText(target.textContent.trim());
      button.textContent = "copied";
    } catch {
      button.textContent = "select";
    }

    window.setTimeout(() => {
      button.textContent = original;
    }, 1200);
  });
});
