// The theatres on the Docs page: a live device, and the demos that play on it.
//
// The device is the simulator build in docs/sim, the same firmware the
// Simulator page runs. Nothing is fetched until the reader taps a demo or the
// device; before that the frame shows a true picture the simulator rendered.
//
// The demos are data, in docs/tours.js. This file is the device, the page's
// finger, and the tabs.
(function () {
  "use strict";

  var ALL = window.KISS_TOURS || [];
  if (!ALL.length) return;

  var W = 800, H = 480, PX = W * H * 4;
  var SIM = "sim/";
  // The six faces on the dice screen, in the screen's own points.
  var FACE_X = [118, 230, 342, 455, 567, 680], FACE_Y = 141;
  var STOP = new Error("stopped");

  var still = window.matchMedia("(prefers-reduced-motion: reduce)");
  var query = new URLSearchParams(location.search);

  // ---------- the page side ---------------------------------------------------

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  // The firmware's loader script is fetched once, whichever theatre asks first.
  var glue = null;

  // One theatre per topic that has demos. Each has its own device; only the
  // one in the open topic ever runs.
  function Theatre(host, TOURS) {

  var theatre = el("div", "theatre framed");
  var tabs = el("div", "th-tabs");
  tabs.setAttribute("role", "tablist");
  tabs.setAttribute("aria-label", "Demos on the live device");

  var device = el("div", "th-device");
  var screen = el("div", "th-screen");
  var poster = el("img", "th-poster");
  poster.alt = ""; poster.width = W; poster.height = H;
  var finger = el("span", "th-finger");
  finger.setAttribute("aria-hidden", "true");
  var tag = el("button", "th-tag");
  tag.type = "button";
  var tagText = el("span", "th-tag-text");
  tag.append(el("span", "th-dot"), tagText);
  screen.append(poster, finger);
  device.append(screen, tag);

  var side = el("div", "th-side");
  // Not a heading element: the guide builds its "on this page" list and its
  // search index from the headings in a topic, and this line changes with
  // the tab.
  var title = el("p", "th-title");
  var lead = el("p", "th-lead");
  var stepList = el("ol", "th-steps");
  var extra = el("div", "th-extra");
  var note = el("p", "th-note");
  note.setAttribute("role", "status");
  var safe = el("p", "th-safe");
  safe.append(el("strong", null, "A browser tab is not a signer."),
    " Make the keys that matter on the device.");

  // Save a demo as a short video, straight from the firmware on the page, for
  // a post or the README. Only where the browser can record a canvas.
  var rec = el("button", "th-rec", "Save this demo as a video");
  rec.type = "button";
  var canRecord = !!(window.MediaRecorder && HTMLCanvasElement.prototype.captureStream);
  rec.hidden = !canRecord;
  side.append(title, lead, stepList, extra, note, rec, safe);

  // A demo that asks for the game's own sunset gets it behind the device. It
  // hangs off the device, not the window, so it can never reach the text.
  var scene = TOURS.some(function (t) { return t.scene; });
  if (scene) {
    var stage = el("div", "th-stage");
    var sky = el("div", "th-scene");
    sky.setAttribute("aria-hidden", "true");
    sky.append(el("div", "th-glow"), el("div", "th-sun"), el("div", "th-sea"));
    stage.append(sky, device);
    theatre.classList.add("is-hero");
    theatre.append(tabs, stage, side);
  } else {
    theatre.append(tabs, device, side);
  }

  // Above the feature cards in Features; in How it works, after the page's own
  // four steps, so the reader reads them and then watches them.
  var before = host.querySelector(".feats"), after = host.querySelector(":scope > ol.steps");
  if (before) host.insertBefore(theatre, before);
  else if (after) after.insertAdjacentElement("afterend", theatre);
  else host.appendChild(theatre);
  if (TOURS.length < 2) tabs.hidden = true;

  var current = null;      // the demo whose tab is selected
  var stepEls = [];

  function setTag(state) {
    tag.className = "th-tag is-" + state;
    device.classList.toggle("is-live", state === "live");
    tagText.textContent =
      state === "loading" ? "Loading the firmware" :
      state === "live" ? "Live firmware" :
      "Tap to run the real firmware";
  }

  function mark(at) {
    stepEls.forEach(function (li, i) {
      li.classList.toggle("on", i === at);
      li.classList.toggle("done", i < at);
    });
  }

  function showPoster(src) {
    if (src) poster.src = src;
    if (scene && src) theatre.classList.toggle("in-signer", src.indexOf("game-menu") < 0);
    if (canvas) canvas.hidden = true;
    poster.hidden = false;
  }

  function showCanvas() {
    canvas.hidden = false;
    poster.hidden = true;
  }

  function select(tour, byReader) {
    stop();
    current = tour;
    Array.prototype.forEach.call(tabs.children, function (b) {
      var on = b.dataset.id === tour.id;
      b.setAttribute("aria-selected", on ? "true" : "false");
      b.tabIndex = on ? 0 : -1;
    });
    title.textContent = tour.title;
    lead.textContent = tour.lead || "";
    stepList.textContent = "";
    extra.textContent = "";
    note.textContent = "";
    stepEls = [];
    theatre.classList.toggle("is-plain", !!tour.plain);
    rec.hidden = !canRecord || !!tour.plain;

    (tour.steps || []).forEach(function (step) {
      var li = el("li");
      li.append(el("strong", null, step.h), el("span", null, step.p));
      // Before the firmware runs, reading a step shows that step's own frame.
      li.addEventListener("click", function () {
        if (playing || !step.frame) return;
        mark(stepEls.indexOf(li));
        showPoster(step.frame);
      });
      stepList.appendChild(li);
      stepEls.push(li);
    });

    if (tour.plain) {
      var ul = el("ul", "th-limits");
      tour.plain.forEach(function (row) {
        var li = el("li");
        li.append(el("strong", null, row[0]), el("span", null, row[1]));
        ul.appendChild(li);
      });
      extra.appendChild(ul);
    }

    showPoster(tour.poster);
    setTag(mod ? "live" : "idle");

    // One address per demo, so a question can be answered with a link. Only
    // a choice the reader made is written into the address bar.
    if (byReader) try {
      var u = new URL(location.href);
      u.searchParams.set("demo", tour.id);
      history.replaceState(null, "", u);
    } catch (e) {}
  }

  TOURS.forEach(function (tour) {
    var b = el("button", "th-tab", tour.tab);
    b.type = "button";
    b.dataset.id = tour.id;
    b.setAttribute("role", "tab");
    // Choosing a demo is asking to see it, so it plays.
    b.addEventListener("click", function () {
      select(tour, true);
      if (!tour.plain) play(tour);
    });
    tabs.appendChild(b);
  });

  tabs.addEventListener("keydown", function (e) {
    if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return;
    var list = Array.prototype.slice.call(tabs.children);
    var i = list.indexOf(document.activeElement);
    if (i < 0) return;
    var next = list[(i + (e.key === "ArrowRight" ? 1 : list.length - 1)) % list.length];
    next.focus();
    e.preventDefault();
  });

  // ---------- the device ------------------------------------------------------

  var mod = null, ptr = 0;
  var canvas = null, ctx = null;
  var run = 0, playing = false;
  var sleepers = [], exacts = [];
  var looping = false, last = 0;
  var topicOpen = !host.hidden, inView = true;

  function loadGlue() {
    if (glue) return glue;
    glue = new Promise(function (resolve, reject) {
      var s = document.createElement("script");
      s.src = SIM + "kiss-sim.js";
      s.onload = resolve;
      s.onerror = function () { glue = null; reject(new Error("the firmware did not load")); };
      document.head.appendChild(s);
    });
    return glue;
  }

  // A fresh firmware every time. Booting again is how a demo starts from a
  // known screen: the page never guesses where the reader left the device.
  function boot(lang) {
    mod = null; ptr = 0;
    return loadGlue().then(function () {
      return window.KissSim({
        locateFile: function (f) { return SIM + f; },
        print: function () {}, printErr: function () {}
      });
    }).then(function (m) {
      // Language first: every screen reads its words as it is built.
      m.ccall("kiss_sim_set_lang", null, ["string"], [lang || "en"]);
      m.ccall("kiss_sim_boot", null, [], []);
      mod = m;
      window.kissSim = m;        // one handle, for anyone poking at a console
      ensureCanvas();
      if (!looping) { looping = true; last = 0; requestAnimationFrame(frame); }
      return m;
    });
  }

  function ensureCanvas() {
    if (canvas) return;
    canvas = el("canvas");
    canvas.width = W; canvas.height = H;
    canvas.hidden = true;
    canvas.setAttribute("aria-label", "The live KISS Signer firmware");
    ctx = canvas.getContext("2d");
    screen.insertBefore(canvas, finger);
    canvas.addEventListener("pointerdown", function (e) {
      takeOver();
      canvas.setPointerCapture(e.pointerId); fromPointer(e, 1); e.preventDefault();
    });
    canvas.addEventListener("pointermove", function (e) { if (e.buttons) fromPointer(e, 1); });
    canvas.addEventListener("pointerup", function (e) { fromPointer(e, 0); });
    canvas.addEventListener("pointercancel", function (e) { fromPointer(e, 0); });
  }

  function tickOnce(dt) {
    ptr = mod.ccall("kiss_sim_tick", "number", ["number"], [dt]);
  }

  function paint() {
    if (!ctx || !ptr) return;
    // Viewed fresh each time: memory growth can move the buffer.
    ctx.putImageData(new ImageData(new Uint8ClampedArray(mod.HEAPU8.buffer, ptr, PX), W, H), 0, 0);
  }

  function touch(x, y, down) {
    if (mod) mod.ccall("kiss_sim_touch", null, ["number", "number", "number"], [x, y, down]);
  }

  function fromPointer(e, down) {
    if (!mod) return;
    var r = canvas.getBoundingClientRect();
    touch(Math.round((e.clientX - r.left) * W / r.width),
          Math.round((e.clientY - r.top) * H / r.height), down);
  }

  // The device runs only while it can be seen: the topic open, the frame on
  // screen, the tab in front. Anything waiting on its clock waits with it.
  function frame(now) {
    requestAnimationFrame(frame);
    if (!mod || !topicOpen || !inView) { last = 0; return; }
    if (exacts.length) {
      // A stroke owns the clock: one 16 ms sample a frame, whatever the
      // screen's own rate, which is how the desktop walk draws it.
      var x = exacts[0];
      tickOnce(16);
      x.left -= 16;
      if (x.left <= 0) { exacts.shift(); settle(x); }
      last = now;
      paint();
      return;
    }
    var dt = last ? Math.min(now - last, 100) : 16;
    last = now;
    tickOnce(Math.max(1, Math.round(dt)));
    paint();
    if (scene && ++frames % 15 === 0) followWorld();
    for (var i = sleepers.length - 1; i >= 0; i--) {
      sleepers[i].left -= dt;
      if (sleepers[i].left <= 0) settle(sleepers.splice(i, 1)[0]);
    }
  }

  function settle(w) {
    if (w.id !== run) w.reject(STOP); else w.resolve();
  }

  // Which world the device is in, read off the screen it just drew: the game
  // is full of colour, every signer screen is near black. The sunset follows,
  // whoever is driving.
  var frames = 0;
  function followWorld() {
    var px = new Uint8ClampedArray(mod.HEAPU8.buffer, ptr, PX), sum = 0, n = 0;
    for (var y = 24; y < H; y += 56) {
      for (var x = 24; x < W; x += 64) {
        var o = (y * W + x) * 4;
        sum += Math.max(px[o], px[o + 1], px[o + 2]) - Math.min(px[o], px[o + 1], px[o + 2]);
        n++;
      }
    }
    theatre.classList.toggle("in-signer", sum / n < 28);
  }

  // Two clocks with one shape. The live one waits for the device as the
  // reader sees it; the fast one runs the device ahead without drawing, for
  // reaching a demo's first screen and for the self test.
  var liveClock = {
    live: true,
    sleep: function (ms, id) {
      return new Promise(function (resolve, reject) {
        sleepers.push({ left: ms, id: id, resolve: resolve, reject: reject });
      });
    },
    exact: function (ms, id) {
      return new Promise(function (resolve, reject) {
        exacts.push({ left: ms, id: id, resolve: resolve, reject: reject });
      });
    }
  };

  var fastClock = {
    live: false,
    sleep: function (ms, id) {
      if (id !== run) return Promise.reject(STOP);
      for (var t = 0; t < ms; t += 16) tickOnce(16);
      return Promise.resolve();
    },
    exact: function (ms, id) { return fastClock.sleep(ms, id); }
  };

  // ---------- the page's finger ------------------------------------------------

  function seq(list, fn) {
    return list.reduce(function (p, item, i) {
      return p.then(function () { return fn(item, i); });
    }, Promise.resolve());
  }

  function point(x, y, drawing, clock) {
    if (!clock.live) return;
    finger.style.left = (x / W * 100) + "%";
    finger.style.top = (y / H * 100) + "%";
    finger.classList.remove("tap", "draw");
    void finger.offsetWidth;                 // restart the ring
    finger.classList.add(drawing ? "draw" : "tap");
  }

  function tap(x, y, clock, id) {
    point(x, y, false, clock);
    touch(x, y, 1);
    return clock.sleep(112, id).then(function () {
      touch(x, y, 0);
      return clock.sleep(64, id);
    });
  }

  function drag(x0, y, x1, ms, clock, id) {
    var n = 14, i = 0;
    point(x0, y, true, clock);
    touch(x0, y, 1);
    function next() {
      if (i >= n) {
        return clock.sleep(160, id).then(function () {
          touch(x1, y, 0);
          finger.classList.remove("draw");
        });
      }
      i++;
      var x = Math.round(x0 + (x1 - x0) * i / n);
      point(x, y, true, clock);
      touch(x, y, 1);
      return clock.sleep(ms / n, id).then(next);
    }
    return clock.sleep(140, id).then(next);
  }

  // K, I, S, S, in the points sim/sim_main.c's draw_cover_word uses.
  function line(x0, y0, dx, dy, n) {
    var a = [];
    for (var i = 0; i <= n; i++) a.push([x0 + i * dx, y0 + i * dy]);
    return a;
  }
  var KISS = [
    line(140, 120, 0, 20, 9),
    line(140, 210, 15, -13, 6),
    line(140, 210, 15, 15, 6),
    line(285, 130, 0, 21, 8),
    [[420, 140], [360, 152], [345, 188], [400, 212], [422, 250], [362, 286], [342, 272]],
    [[540, 140], [480, 152], [465, 188], [520, 212], [542, 250], [482, 286], [462, 272]]
  ];

  function kiss(clock, id) {
    return seq(KISS, function (s) { return stroke(s, clock, id); });
  }

  function scanning() {
    return !!(mod && mod.ccall("kiss_sim_scanning", "number", [], []));
  }

  function until(what, ms, clock, id) {
    function poll(left) {
      if (scanning()) return Promise.resolve();
      if (left <= 0) return Promise.reject(new Error("the " + what + " screen did not open"));
      return clock.sleep(80, id).then(function () { return poll(left - 80); });
    }
    return poll(ms);
  }

  // The rolls, and the same sum worked out on this side of the glass.
  var sha = null;
  function shaPanel() {
    extra.textContent = "";
    sha = { rolls: el("b"), sum: el("b") };
    var box = el("div", "th-sha");
    var a = el("p"), b = el("p");
    a.append("rolls so far: ", sha.rolls);
    b.append("SHA256, worked out by this page: ", sha.sum);
    box.append(a, b);
    extra.appendChild(box);
  }

  function shaUpdate(text) {
    if (!sha || !window.crypto || !crypto.subtle) return;
    sha.rolls.textContent = text;
    crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)).then(function (buf) {
      var hex = Array.prototype.map.call(new Uint8Array(buf), function (v) {
        return (v < 16 ? "0" : "") + v.toString(16);
      }).join("");
      // Only if no later roll has landed since this one was sent off.
      if (sha && sha.rolls.textContent === text) sha.sum.textContent = hex.slice(0, 32);
    });
  }

  function rolls(digits, clock, id) {
    if (clock.live) shaPanel();
    var done = "";
    return seq(digits.split(""), function (d) {
      return tap(FACE_X[+d - 1], FACE_Y, clock, id).then(function () {
        done += d;
        if (clock.live) shaUpdate(done);
      });
    });
  }

  // The device's own keyboard, in the screen's points. Two layers share the
  // same key positions: letters, and 123 for digits and symbols. Shift is
  // one tap for one capital; 123 stays on until abc is pressed.
  var ABC = {}, NUM = {};
  function rows(map, list, xs, steps) {
    list.forEach(function (row, r) {
      var y = [205, 283, 360][r];
      row.split("").forEach(function (ch, i) { map[ch] = [Math.round(xs[r] + i * steps[r]), y]; });
    });
  }
  rows(ABC, ["qwertyuiop", "asdfghjkl", "zxcvbnm"], [50, 53, 141], [79.1, 88, 88]);
  rows(NUM, ["1234567890", "!@#$%&()?", "-_=+.,/"], [50, 53, 141], [79.1, 88, 88]);
  var SPACE = [472, 437], SHIFT = [53, 360], LAYER = [50, 437], DEL = [757, 360];

  function type(text, clock, id) {
    var onNum = false;
    function press(k) {
      return tap(k[0], k[1], clock, id).then(function () { return clock.sleep(60, id); });
    }
    return seq(text.split(""), function (ch) {
      if (ch === " ") return press(SPACE);
      var lower = ch.toLowerCase(), want = NUM[ch] ? "num" : ABC[lower] ? "abc" : null;
      if (!want) return Promise.reject(new Error("no key for " + ch));
      var p = Promise.resolve();
      if ((want === "num") !== onNum) { onNum = want === "num"; p = p.then(function () { return press(LAYER); }); }
      if (want === "abc" && ch !== lower) p = p.then(function () { return press(SHIFT); });
      return p.then(function () { return press(want === "num" ? NUM[ch] : ABC[lower]); });
    }).then(function () { if (onNum) return press(LAYER); });
  }

  // Taps for CAMERA AND TAPS: anywhere in its box, never on an even beat.
  function taps(n, clock, id) {
    var list = [];
    for (var i = 0; i < n; i++) list.push(i);
    return seq(list, function () {
      var x = 160 + Math.round(Math.random() * 480), y = 160 + Math.round(Math.random() * 220);
      return tap(x, y, clock, id).then(function () {
        return clock.sleep(40 + Math.round(Math.random() * 160), id);
      });
    });
  }

  function clear(n, clock, id) {
    var list = [];
    for (var i = 0; i < n; i++) list.push(i);
    return seq(list, function () { return tap(DEL[0], DEL[1], clock, id); });
  }

  function stroke(points, clock, id) {
    return seq(points, function (p) {
      point(p[0], p[1], true, clock);
      touch(p[0], p[1], 1);
      return clock.exact(16, id);
    }).then(function () {
      var end = points[points.length - 1];
      touch(end[0], end[1], 0);
      finger.classList.remove("draw");
      return clock.exact(32, id);
    });
  }

  function act(a, clock, id, tour) {
    if (id !== run) return Promise.reject(STOP);
    switch (a[0]) {
      case "wait": return clock.sleep(a[1], id);
      case "tap": return tap(a[1], a[2], clock, id);
      case "drag": return drag(a[1], a[2], a[3], a[4], clock, id);
      case "kiss": return kiss(clock, id);
      case "signer": mod.ccall("kiss_sim_test_signer", null, [], []); return Promise.resolve();
      case "feed": mod.ccall("kiss_sim_feed_qr", null, ["string"], [tour.data[a[1]]]); return Promise.resolve();
      case "until": return until(a[1], a[2], clock, id);
      case "rolls": return rolls(a[1], clock, id);
      case "type": return type(a[1], clock, id);
      case "clear": return clear(a[1], clock, id);
      case "stroke": return stroke(a[1], clock, id);
      case "taps": return taps(a[1], clock, id);
    }
    return Promise.reject(new Error("unknown step " + a[0]));
  }

  // ---------- playing a demo -----------------------------------------------------

  // ---------- recording ---------------------------------------------------------

  var wantRec = false, recorder = null;

  function startRec(tour) {
    var types = ["video/webm;codecs=vp9", "video/webm;codecs=vp8", "video/webm"];
    var type = types.filter(function (t) { return MediaRecorder.isTypeSupported(t); })[0];
    var chunks = [];
    recorder = new MediaRecorder(canvas.captureStream(30), type ? { mimeType: type } : undefined);
    recorder.ondataavailable = function (e) { if (e.data.size) chunks.push(e.data); };
    recorder.onstop = function () {
      if (!recorder || !recorder.keep) { recorder = null; return; }
      var a = el("a");
      a.href = URL.createObjectURL(new Blob(chunks, { type: "video/webm" }));
      a.download = "kiss-signer-" + tour.id + ".webm";
      a.click();
      setTimeout(function () { URL.revokeObjectURL(a.href); }, 4000);
      recorder = null;
    };
    recorder.start();
    rec.textContent = "Recording. It saves when the demo ends";
    rec.classList.add("is-on");
  }

  function stopRec(keep) {
    wantRec = false;
    rec.textContent = "Save this demo as a video";
    rec.classList.remove("is-on");
    if (!recorder) return;
    var r = recorder;
    r.keep = keep;
    // A short tail, so the last screen is held rather than cut.
    setTimeout(function () { if (r.state !== "inactive") r.stop(); }, keep ? 1500 : 0);
  }

  rec.addEventListener("click", function () {
    if (!current || current.plain || recorder) return;
    wantRec = true;
    play(current);
  });

  function stop() {
    if (recorder && playing) stopRec(false);
    run++;
    playing = false;
    finger.classList.remove("tap", "draw");
    // Whatever was waiting on the old run is told so, and lets go.
    sleepers.splice(0).concat(exacts.splice(0)).forEach(settle);
  }

  function play(tour) {
    stop();
    var id = run;
    playing = true;
    mark(-1);
    extra.textContent = "";
    sha = null;
    note.textContent = "";
    showPoster(tour.poster);
    setTag("loading");

    boot().then(function () {
      if (id !== run) throw STOP;
      return seq(tour.prep || [], function (a) { return act(a, fastClock, id, tour); });
    }).then(function () {
      if (id !== run) throw STOP;
      paint();
      showCanvas();
      setTag("live");
      if (wantRec) startRec(tour);
      return seq(tour.steps, function (step, i) {
        mark(i);
        return seq(step.run || [], function (a) { return act(a, liveClock, id, tour); });
      });
    }).then(function () {
      playing = false;
      mark(tour.steps.length);
      note.textContent = tour.end || "Your turn: tap it.";
      if (recorder) stopRec(true);
    }).catch(function (err) {
      if (err === STOP) return;
      playing = false;
      if (recorder) stopRec(false);
      setTag(mod ? "live" : "idle");
      note.textContent = mod
        ? "The demo lost its place (" + err.message + "). The device is still live: tap it yourself."
        : "The firmware did not load here. The picture above is a real frame from it.";
    });
  }

  // The reader touching the device ends the demo: it is their device now.
  function takeOver() {
    if (!playing) return;
    stop();
    stepEls.forEach(function (li) { li.classList.remove("on"); });
    note.textContent = "Your turn. It is the real firmware: tap anything.";
  }

  function wake() {
    if (!current || current.plain) return;
    if (mod && !canvas.hidden) return;        // already live and showing
    play(current);
  }
  tag.addEventListener("click", wake);
  screen.addEventListener("click", function (e) { if (e.target !== canvas) wake(); });

  // ---------- when the device may run ----------------------------------------------

  if ("IntersectionObserver" in window) {
    new IntersectionObserver(function (entries) {
      inView = entries[0].isIntersecting;
    }, { threshold: 0.05 }).observe(device);
  }

  // Called by the guide when it opens or leaves a topic, the same way it
  // starts and stops the other moving pictures on the page.
  function onTopic(isOpen, opened) {
    topicOpen = isOpen;
    if (!isOpen) return;
    // Arriving here with the firmware already running plays the chosen demo,
    // unless the reader asked for no motion.
    if (opened && mod && !playing && current && !current.plain && !still.matches) play(current);
  }

  // ---------- the self test ----------------------------------------------------------

  // guide.html?selftest#tour plays every demo without drawing and shows
  // where each one ended. Run it whenever the firmware in docs/sim is rebuilt:
  // the demos tap fixed points, and a moved screen shows up here first.
  function near(px, want) {
    return Math.abs(px[0] - want[0]) < 60 && Math.abs(px[1] - want[1]) < 60 && Math.abs(px[2] - want[2]) < 60;
  }

  function selfTest() {
    var out = el("div", "th-selftest");
    theatre.appendChild(out);
    return seq(TOURS.filter(function (t) { return !t.plain; }), function (tour) {
      stop();
      var id = run;
      return boot().then(function () {
        return seq(tour.prep || [], function (a) { return act(a, fastClock, id, tour); });
      }).then(function () {
        return seq(tour.steps, function (step) {
          return seq(step.run || [], function (a) { return act(a, fastClock, id, tour); });
        });
      }).then(function () { return ""; }, function (err) { return err.message; })
        .then(function (failure) {
          var px = new Uint8ClampedArray(mod.HEAPU8.buffer, ptr, PX);
          var verdict = "LOOK";
          if (failure) verdict = "FAIL: " + failure;
          else if (tour.probe) {
            var o = (tour.probe.y * W + tour.probe.x) * 4;
            var got = [px[o], px[o + 1], px[o + 2]];
            var ok = tour.probe.is ? near(got, tour.probe.is) : !near(got, tour.probe.not);
            verdict = ok ? "PASS" : "FAIL: the last screen is not the expected one";
          }
          var fig = el("figure");
          var c = el("canvas");
          c.width = W; c.height = H;
          c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(px), W, H), 0, 0);
          fig.append(c, el("figcaption", null, tour.id + ": " + verdict));
          out.appendChild(fig);
        });
    }).then(function () {
      stop();
      note.textContent = "Self test done. PASS and FAIL are checked by the page; LOOK means compare the picture by eye.";
    });
  }

  // ---------- start --------------------------------------------------------------------

  var first = TOURS[0];
  TOURS.forEach(function (t) { if (t.id === query.get("demo")) first = t; });
  select(first);
  setTag("idle");

  // Open one demo by name and play it, for the "Watch it" links on the
  // other topics.
  function pick(name) {
    TOURS.forEach(function (t) {
      if (t.id !== name) return;
      select(t, true);
      if (!t.plain && !still.matches) play(t);
    });
  }

  return { host: host, topic: onTopic, selfTest: selfTest, pick: pick };
  }

  // A browser holding an older copy of styles.css than this script gets the
  // theatre unstyled. If that is what happened, fetch the stylesheet again.
  function styleCheck() {
    var t = document.querySelector(".theatre");
    if (!t || getComputedStyle(t).display === "grid") return;
    var old = document.querySelector('link[rel="stylesheet"][href$="styles.css"]');
    if (!old) return;
    var fresh = document.createElement("link");
    fresh.rel = "stylesheet";
    fresh.href = old.getAttribute("href") + "?fresh=" + Date.now();
    old.after(fresh);
  }

  var byHost = {}, theatres = [];
  ALL.forEach(function (t) {
    var h = t.host || "tour";
    (byHost[h] = byHost[h] || []).push(t);
  });
  Object.keys(byHost).forEach(function (id) {
    var host = document.getElementById(id);
    if (host) theatres.push(Theatre(host, byHost[id]));
  });

  styleCheck();

  // "Watch it" links on the Docs topics: go to See it work and play that demo.
  document.addEventListener("click", function (e) {
    var a = e.target.closest && e.target.closest("a[data-demo]");
    if (!a) return;
    theatres.forEach(function (t) {
      if (t.host.id === "tour") setTimeout(function () { t.pick(a.dataset.demo); }, 60);
    });
  });

  // Called by the guide with the topic it is showing.
  window.__live = function (id, opened) {
    // See it work needs the width; its "on this page" column has one entry.
    document.body.classList.toggle("on-tour", id === "tour");
    theatres.forEach(function (t) { t.topic(t.host.id === id, opened); });
  };

  if (query.has("selftest")) {
    var want = location.hash.slice(1) || "tour";
    theatres.forEach(function (t) { if (t.host.id === want) t.selfTest(); });
  }
})();
