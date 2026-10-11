// aci-bridge.js - the `window.aci` object of ACI custom HTML UIs (ACI spec section 10.2).
// SPDX-License-Identifier: MIT-0
// The host injects this script before the page loads, after `window.__ACI_INIT__` (the initial state);
// pages must not include it themselves. Outside a host (e.g. `aicontact.py ui preview`) it runs on
// window.__ACI_PREVIEW__ with local state.
// Keep this file ASCII-only: JUCE's Linux web view drops messages containing non-ASCII text.
(function () {
  "use strict";
  if (window.aci) return;

  var juce = window.__JUCE__ && window.__JUCE__.backend && window.__ACI_INIT__ ? window.__JUCE__ : null;
  var init = juce ? window.__ACI_INIT__ : (window.__ACI_PREVIEW__ || { params: [] });
  var params = (init.params || []).map(function (p, i) {
    return Object.assign({ index: i, unit: "", scale: "linear", steps: 0, labels: [] }, p);
  });
  var listeners = [];
  var lastLocalSet = {};  // param index -> time of our own last set (ignore stale echoes meanwhile)

  // Non-ASCII text is sent as \uXXXX escapes (see the note at the top).
  function ascii(s) {
    return String(s).replace(/[^\x00-\x7f]/g, function (c) { return "\\u" + ("000" + c.charCodeAt(0).toString(16)).slice(-4); });
  }

  function send(msg) {
    if (msg.message !== undefined) msg.message = juce ? ascii(msg.message) : msg.message;
    if (juce) juce.backend.emitEvent("aci", msg);
    else if (msg.type === "log") console.log("[aci]", msg.message);
    else if (msg.type === "note") console.log("[aci] note", msg.key, msg.velocity, msg.ms + " ms");
  }

  function param(ref) {
    if (ref && typeof ref === "object") return params[ref.index];
    if (typeof ref === "number") return params.find(function (p) { return p.id === ref; });
    if (typeof ref === "string") {
      var n = ref.toLowerCase();
      return params.find(function (p) { return p.name.toLowerCase() === n; });
    }
    return undefined;
  }

  function need(ref) {
    var p = param(ref);
    if (!p) throw new Error("aci: no param " + JSON.stringify(ref) + " (params: " +
                            params.map(function (q) { return q.name; }).join(", ") + ")");
    return p;
  }

  function clamp(p, v) {
    v = Number(v);
    if (!isFinite(v)) v = p.default;
    v = Math.min(p.max, Math.max(p.min, v));
    if (p.steps > 1) {
      var step = (p.max - p.min) / (p.steps - 1);
      v = p.min + Math.round((v - p.min) / step) * step;
    }
    return v;
  }

  function toNormalized(ref, v) {
    var p = need(ref);
    v = clamp(p, v === undefined ? p.value : v);
    if (p.scale === "log" && p.min > 0) return Math.log(v / p.min) / Math.log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
  }

  function fromNormalized(ref, n) {
    var p = need(ref);
    n = Math.min(1, Math.max(0, Number(n) || 0));
    if (p.scale === "log" && p.min > 0) return clamp(p, p.min * Math.pow(p.max / p.min, n));
    return clamp(p, p.min + n * (p.max - p.min));
  }

  function fixed(v, digits) { return Number(v).toFixed(digits); }

  function format(ref, v) {
    var p = need(ref);
    v = v === undefined ? p.value : clamp(p, v);
    if (p.steps > 1 && p.labels.length === p.steps)
      return p.labels[Math.round((v - p.min) / (p.max - p.min) * (p.steps - 1))];
    var a = Math.abs(v), u = p.unit;
    if (u === "Hz" && a >= 1000) return fixed(v / 1000, a >= 10000 ? 1 : 2) + " kHz";
    if (u === "s" && a < 1) return fixed(v * 1000, a < 0.01 ? 1 : 0) + " ms";
    var s = fixed(v, a >= 100 ? 0 : a >= 10 ? 1 : 2);
    return u ? s + " " + u : s;
  }

  function changed(p, v) {
    for (var i = 0; i < listeners.length; ++i) {
      try { listeners[i](p, v); } catch (e) { console.error(e); }
    }
  }

  function set(ref, v) {
    var p = need(ref);
    v = clamp(p, v);
    lastLocalSet[p.index] = Date.now();
    if (v === p.value) return v;
    p.value = v;
    send({ type: "set", index: p.index, value: v });
    changed(p, v);
    return v;
  }

  if (juce) {
    juce.backend.addEventListener("aci", function (msg) {
      if (!msg || msg.type !== "values") return;
      var now = Date.now();
      Object.keys(msg.values).forEach(function (k) {
        var p = params[Number(k)], v = msg.values[k];
        if (!p || now - (lastLocalSet[p.index] || 0) < 400) return;  // the user is still dragging it here
        if (Math.abs(v - p.value) <= 1e-6 * Math.max(1, Math.abs(v))) return;
        p.value = v;
        changed(p, v);
      });
    });
  }

  // Declarative bindings: <input type="range" data-aci-param="Cutoff">, <select data-aci-param="Wave">,
  // <input type="checkbox" data-aci-param="Mono">, <span data-aci-value="Cutoff"></span>.
  function bindAll(root) {
    (root || document).querySelectorAll("[data-aci-param]").forEach(function (el) {
      if (el.__aciBound) return;
      var p = param(el.getAttribute("data-aci-param"));
      if (!p) { console.warn("aci: no param", el.getAttribute("data-aci-param")); return; }
      el.__aciBound = true;
      var tag = el.tagName.toLowerCase(), type = (el.type || "").toLowerCase();
      var update, read;
      if (tag === "select") {
        if (!el.options.length)
          for (var i = 0; i < Math.max(p.steps, 2); ++i) {
            var o = document.createElement("option");
            o.value = String(i);
            o.textContent = p.labels[i] !== undefined ? p.labels[i] : String(i);
            el.appendChild(o);
          }
        var n = Math.max(p.steps, 2) - 1;
        update = function () { el.value = String(Math.round(toNormalized(p) * n)); };
        read = function () { return fromNormalized(p, Number(el.value) / n); };
      } else if (type === "checkbox") {
        update = function () { el.checked = p.value > (p.min + p.max) / 2; };
        read = function () { return el.checked ? p.max : p.min; };
      } else {
        // Ranges work in normalized units so log scales and steps feel right.
        el.min = "0"; el.max = "1";
        el.step = p.steps > 1 ? String(1 / (p.steps - 1)) : "0.001";
        update = function () { el.value = String(toNormalized(p)); };
        read = function () { return fromNormalized(p, Number(el.value)); };
        el.addEventListener("dblclick", function () { set(p, p.default); });
      }
      el.addEventListener(type === "checkbox" || tag === "select" ? "change" : "input", function () { set(p, read()); });
      onChange(function (q) { if (q === p) update(); });
      update();
    });
    (root || document).querySelectorAll("[data-aci-value]").forEach(function (el) {
      if (el.__aciBound) return;
      var p = param(el.getAttribute("data-aci-value"));
      if (!p) return;
      el.__aciBound = true;
      var update = function () { el.textContent = format(p); };
      onChange(function (q) { if (q === p) update(); });
      update();
    });
  }

  function onChange(fn) {
    listeners.push(fn);
    return function () { listeners = listeners.filter(function (f) { return f !== fn; }); };
  }

  window.aci = {
    host: juce ? "aicontact" : "preview",
    slot: init.slot || "",
    instrument: init.instrument || {},
    params: params,
    param: param,
    get: function (ref) { return need(ref).value; },
    set: set,
    getNormalized: function (ref) { return toNormalized(ref); },
    setNormalized: function (ref, n) { return set(ref, fromNormalized(ref, n)); },
    toNormalized: toNormalized,
    fromNormalized: fromNormalized,
    format: format,
    onChange: onChange,
    bind: bindAll,
    note: function (key, velocity, ms) {
      send({ type: "note", key: key, velocity: velocity === undefined ? 100 : velocity, ms: ms === undefined ? 500 : ms });
    },
    log: function (message) { send({ type: "log", message: String(message) }); }
  };

  window.addEventListener("error", function (e) {
    send({ type: "log", level: "error", message: (e.message || "error") + (e.filename ? " (" + e.filename.split("/").pop() + ":" + e.lineno + ")" : "") });
  });
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", function () { bindAll(); });
  else bindAll();
  send({ type: "hello" });
})();
