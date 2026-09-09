/* The device's own accent selector, on the site.
 *
 * The four accents and their two hexes each are lifted from ACC_HEX and
 * ACC_BG_HEX in main/kiss_theme.c, so SETTINGS -> THEME on the device and the
 * swatches in the header are the same four choices in the same order. Do not
 * invent a fifth here: the firmware's WT_ACC_N is the source of truth.
 *
 * There is deliberately no font choice here. The site wears the firmware's
 * mono everywhere; that typeface is the identity, not a preference.
 *
 * The attribute is set during parse, before first paint, so the accent never
 * flashes the default on the way in.
 */
(function () {
  var ACCENTS = [
    { id: "mono", name: "MONO", sw: "#9fb6d4" },
    { id: "green", name: "GREEN", sw: "#35d07f" },
    { id: "pink", name: "CYPHERPINK", sw: "#e85ab8" },
    { id: "orange", name: "ORANGE", sw: "#ff8a3d" }
  ];

  var root = document.documentElement;

  function read(key, fallback) {
    try {
      return localStorage.getItem(key) || fallback;
    } catch (e) {
      return fallback; // private mode, or storage blocked
    }
  }

  function write(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch (e) {
      /* the choice still applies for this page view */
    }
  }

  var accent = read("kiss-accent", "mono");
  if (!ACCENTS.some(function (a) { return a.id === accent; })) accent = "mono";
  root.setAttribute("data-accent", accent);

  function build() {
    var bar = document.querySelector(".topbar");
    if (!bar || bar.querySelector(".theme-pick")) return;

    var wrap = document.createElement("div");
    wrap.className = "theme-pick";
    wrap.setAttribute("role", "group");
    wrap.setAttribute("aria-label", "Appearance");

    ACCENTS.forEach(function (a) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "swatch";
      b.title = a.name;
      b.setAttribute("aria-label", "Accent: " + a.name);
      b.setAttribute("aria-pressed", String(a.id === accent));
      b.style.setProperty("--sw", a.sw);
      b.addEventListener("click", function () {
        accent = a.id;
        root.setAttribute("data-accent", accent);
        write("kiss-accent", accent);
        Array.prototype.forEach.call(
          wrap.querySelectorAll(".swatch"),
          function (x) {
            x.setAttribute("aria-pressed", String(x.dataset.accent === accent));
          }
        );
      });
      b.dataset.accent = a.id;
      wrap.appendChild(b);
    });


    bar.appendChild(wrap);
  }

  /* The full stops, in the accent.
   *
   * No selector can reach a single character, so the sentence-ending periods
   * are wrapped once, here, and the colour is left to CSS. Only a period that
   * ENDS a sentence is wrapped -- one followed by whitespace or by the end of
   * the text -- so "0.1.0-beta10", "kiss-signer.bin" and an ellipsis keep
   * their dots and stay one unbroken string to read or copy.
   *
   * Scoped to prose that nothing else writes to. In particular it never walks
   * the install card's status line: app.js owns that text and rewrites it as
   * the binary is checked. */
  var PROSE = [
    ".doc-card p",
    ".doc-card li",
    ".lede",
    ".steps p",
    ".feats p",
    ".install-steps p",
    ".power-note"
  ].join(",");

  var SKIP = { CODE: 1, PRE: 1, KBD: 1, SAMP: 1, SCRIPT: 1, STYLE: 1 };

  function tintStops(el) {
    var walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT, null);
    var texts = [];
    var n;
    while ((n = walker.nextNode())) {
      if (n.nodeValue.indexOf(".") === -1) continue;
      var p = n.parentNode,
        skip = false;
      while (p && p !== el.parentNode) {
        if (SKIP[p.nodeName]) { skip = true; break; }
        p = p.parentNode;
      }
      if (!skip) texts.push(n);
    }

    texts.forEach(function (node) {
      // a period that ends a sentence: followed by space, or last in the node
      var parts = node.nodeValue.split(/\.(?=\s|$)/);
      if (parts.length < 2) return;
      var frag = document.createDocumentFragment();
      parts.forEach(function (chunk, i) {
        if (chunk) frag.appendChild(document.createTextNode(chunk));
        if (i < parts.length - 1) {
          var s = document.createElement("span");
          s.className = "fullstop";
          s.textContent = ".";
          frag.appendChild(s);
        }
      });
      node.parentNode.replaceChild(frag, node);
    });
  }

  function stops() {
    Array.prototype.forEach.call(document.querySelectorAll(PROSE), tintStops);
  }

  function start() {
    build();
    stops();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }
})();
