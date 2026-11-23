// Short topics on the Docs page.
//
// Each topic keeps its title, its first line, and its pictures, steps and
// demos in view; the long explanation after that folds under "More". Nothing is
// removed: search, the "on this page" links and printing open the folds, and
// without scripts the page shows everything as it always has.
//
// It runs after the guide's own script, so the search index is built from the
// page as written.
(function () {
  "use strict";

  // What stays in view: pictures, commands are not (they sit behind More),
  // the try-it lines, warnings, and the live device.
  var VISUAL = "figure, .feats, ol.steps, .theatre, .first-run, .shots";
  var ALWAYS = ".try, .danger";

  // The demos on See it work that show what a topic explains.
  var WATCH = {
    firstboot: [["seed", "Seed words"], ["swipe", "Your swipe"]],
    storage: [["storage", "Storage"], ["backup", "Backup"]],
    roundtrip: [["pair", "Pair"], ["receive", "Receive"], ["sign", "Sign"]],
    cautions: [["checks", "Checks"], ["sign", "Sign"]]
  };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  Array.prototype.forEach.call(document.querySelectorAll(".docs-body .doc-card"), function (card) {
    var kids = Array.prototype.slice.call(card.children);
    var h2 = card.querySelector(":scope > h2");
    if (!h2) return;
    var i = kids.indexOf(h2) + 1;
    // The first line: the lede, or the first paragraph when there is none.
    var lead = kids[i] && kids[i].tagName === "P" && !kids[i].matches(ALWAYS) ? kids[i] : null;
    if (lead) i++;

    if (WATCH[card.id]) {
      var w = el("p", "watch");
      w.appendChild(el("span", null, "Watch it"));
      WATCH[card.id].forEach(function (d) {
        var a = el("a", null, d[1]);
        a.href = "#tour";
        a.dataset.demo = d[0];
        w.appendChild(a);
      });
      (lead || h2).insertAdjacentElement("afterend", w);
    }

    while (i < kids.length && kids[i].matches(VISUAL + ", " + ALWAYS)) i++;
    var rest = kids.slice(i).filter(function (k) { return !k.matches(ALWAYS) && !k.classList.contains("watch"); });
    var keep = kids.slice(i).filter(function (k) { return k.matches(ALWAYS); });
    if (rest.length < 1) return;

    var more = el("details", "more");
    more.appendChild(el("summary", null, "More"));
    rest[0].insertAdjacentElement("beforebegin", more);
    rest.forEach(function (k) { more.appendChild(k); });
    keep.forEach(function (k) { card.appendChild(k); });
  });

  function openAll() {
    Array.prototype.forEach.call(document.querySelectorAll("details.more"), function (d) { d.open = true; });
  }

  // A search result or an "on this page" link may point inside a fold.
  document.addEventListener("click", function (e) {
    if (e.target.closest(".docs-results")) { openAll(); return; }
    var a = e.target.closest('a[href^="#"]');
    if (!a || a.dataset.demo) return;
    var t = document.getElementById(a.getAttribute("href").slice(1));
    var d = t && t.closest("details.more");
    if (d) d.open = true;
  }, true);
  document.addEventListener("keydown", function (e) {
    if (e.key === "Enter" && e.target.closest && e.target.closest(".docs-search")) openAll();
  }, true);
  window.addEventListener("beforeprint", openAll);
})();
