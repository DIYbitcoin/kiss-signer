// The install card's progress marks.
//
// Lights BOARD, FILE CHECKED and BOX TICKED as each becomes true. It only
// reads what docs/app.js has already put on the page (the picked board, the
// verify light, the checkbox) and changes nothing app.js reads, so the gate on
// the install button is exactly as it was. The page cannot see a cable, so
// there is no mark for one.
(function () {
  "use strict";

  var marks = document.querySelector(".card-progress");
  var light = document.getElementById("verify-light");
  var ack = document.getElementById("ack");
  if (!marks || !light || !ack) return;

  function set(name, on) {
    var m = marks.querySelector('[data-done="' + name + '"]');
    if (m) m.classList.toggle("on", !!on);
  }

  function update() {
    set("board", document.querySelector('input[name="board"]:checked'));
    set("file", light.classList.contains("ready"));
    set("box", ack.checked);
  }

  document.addEventListener("change", update);
  new MutationObserver(update).observe(light, { attributes: true, attributeFilter: ["class"] });
  update();
})();
