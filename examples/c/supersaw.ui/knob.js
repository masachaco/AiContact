// SuperSaw panel: SVG knobs bound to the instrument's params through the host's `aci` object.
(function () {
  var SVG = "http://www.w3.org/2000/svg";
  var START = -135, END = 135;  // degrees, 0 = up
  var readoutName = document.getElementById("readout-name");
  var readoutValue = document.getElementById("readout-value");

  function el(name, attrs, parent) {
    var e = document.createElementNS(SVG, name);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }

  function polar(cx, cy, r, deg) {
    var a = (deg - 90) * Math.PI / 180;
    return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
  }

  function arc(cx, cy, r, from, to) {
    var a = polar(cx, cy, r, from), b = polar(cx, cy, r, to);
    return "M" + a[0] + " " + a[1] + " A" + r + " " + r + " 0 " + (to - from > 180 ? 1 : 0) + " 1 " + b[0] + " " + b[1];
  }

  function show(p) {
    readoutName.textContent = p.name.toUpperCase();
    readoutValue.textContent = aci.format(p);
  }

  function knob(host) {
    var p = aci.param(host.dataset.param);
    if (!p) return;
    var size = host.classList.contains("big") ? 76 : 54, c = size / 2, r = c - 5;
    var svg = el("svg", { width: size, height: size, viewBox: "0 0 " + size + " " + size }, host);
    var defs = el("defs", {}, svg);
    var grad = el("radialGradient", { id: "cap" + p.index, cx: "40%", cy: "30%", r: "75%" }, defs);
    el("stop", { offset: "0%", "stop-color": "#5a5f69" }, grad);
    el("stop", { offset: "100%", "stop-color": "#1c1e23" }, grad);
    el("path", { d: arc(c, c, r, START, END), stroke: "#0b0c0f", "stroke-width": 5, fill: "none", "stroke-linecap": "round" }, svg);
    var value = el("path", { stroke: "#ff8a3d", "stroke-width": 3.5, fill: "none", "stroke-linecap": "round",
                             style: "filter: drop-shadow(0 0 3px rgba(255,138,61,0.7))" }, svg);
    el("circle", { cx: c, cy: c, r: r - 7, fill: "url(#cap" + p.index + ")", stroke: "#0c0d10", "stroke-width": 1 }, svg);
    var pointer = el("line", { x1: c, y1: c - (r - 10), x2: c, y2: c - (r - 10) * 0.45, stroke: "#f2f3f5",
                               "stroke-width": 2.5, "stroke-linecap": "round" }, svg);
    var name = document.createElement("div");
    name.className = "name";
    name.textContent = host.dataset.label || p.name;
    var val = document.createElement("div");
    val.className = "value";
    host.appendChild(name);
    host.appendChild(val);

    function render() {
      var n = aci.getNormalized(p), deg = START + n * (END - START);
      value.setAttribute("d", n > 0.002 ? arc(c, c, r, START, deg) : "");
      pointer.setAttribute("transform", "rotate(" + deg + " " + c + " " + c + ")");
      val.textContent = aci.format(p);
    }

    var dragY = 0, dragN = 0;
    host.addEventListener("pointerdown", function (e) {
      host.setPointerCapture(e.pointerId);
      host.classList.add("active");
      dragY = e.clientY;
      dragN = aci.getNormalized(p);
      show(p);
    });
    host.addEventListener("pointermove", function (e) {
      if (!host.hasPointerCapture(e.pointerId)) return;
      var range = e.shiftKey ? 800 : 180;  // pixels for the full range; shift = fine
      aci.setNormalized(p, dragN + (dragY - e.clientY) / range);
    });
    host.addEventListener("pointerup", function () { host.classList.remove("active"); });
    host.addEventListener("dblclick", function () { aci.set(p, p.default); });
    host.addEventListener("wheel", function (e) {
      e.preventDefault();
      aci.setNormalized(p, aci.getNormalized(p) - Math.sign(e.deltaY) * (e.shiftKey ? 0.005 : 0.03));
    }, { passive: false });
    host.addEventListener("mouseenter", function () { show(p); });

    aci.onChange(function (q) {
      if (q !== p) return;
      render();
      show(p);
    });
    render();
  }

  document.querySelectorAll(".knob").forEach(knob);
  show(aci.param("Cutoff") || aci.params[0]);

  // Two-octave audition keyboard (C3..B4).
  var keys = document.getElementById("keys");
  var whites = [0, 2, 4, 5, 7, 9, 11], blacks = { 1: 0, 3: 1, 6: 3, 8: 4, 10: 5 };
  var w = 100 / 14;
  for (var oct = 0; oct < 2; ++oct) {
    for (var s = 0; s < 12; ++s) {
      var k = document.createElement("div"), note = 48 + oct * 12 + s;
      var wi = whites.indexOf(s);
      if (wi >= 0) {
        k.className = "white";
        k.style.left = (oct * 7 + wi) * w + "%";
        k.style.width = w + "%";
      } else {
        k.className = "black";
        k.style.left = (oct * 7 + blacks[s] + 0.68) * w + "%";
        k.style.width = w * 0.64 + "%";
      }
      (function (k, note) {
        k.addEventListener("pointerdown", function () {
          k.classList.add("down");
          aci.note(note, 100, 700);
          setTimeout(function () { k.classList.remove("down"); }, 250);
        });
      })(k, note);
      keys.appendChild(k);
    }
  }
})();
