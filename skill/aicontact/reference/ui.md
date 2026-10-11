# Custom instrument UIs

By default the plugin shows generated knobs for an instrument's params. An instrument can instead ship
its own GUI, embedded in the same `.wasm` (custom sections `aci.ui/<path>`):

- **HTML UI** — `index.html` + CSS/JS/images, shown in a web view. Most expressive; pick this by default.
- **Skin** — `ui.json` + images (PNG/JPEG/GIF/SVG): knobs, sliders, switches placed on a background,
  drawn natively. No code runs. Use it when the user wants a classic image-based panel (filmstrip knobs)
  or as a fallback inside the same folder (Linux hosts without WebKitGTK show the skin instead of HTML).

The UI only reads and sets **params** (and can audition notes). Anything a control should change must be
a param in the metadata; the generated "Default UI" stays one click away in the plugin.

## Workflow

1. Put the UI in a folder **next to the source named `<source name>.ui/`** (`instruments/acid.c` →
   `instruments/acid.ui/index.html`). `build`, `validate` and `deploy` embed it automatically
   (`--ui DIR` for another folder, `--no-ui` to skip).
2. Look at it before deploying: `aicontact.py ui preview acid.c --screenshot /tmp/acid-ui.png`, then read
   the PNG. The preview runs the page in headless Chrome/Edge with simulated params (local only, notes
   are logged to the console) and shows it at its declared size inside a preview frame. Without a browser
   it still writes an HTML preview page (`preview` in the result) the user can open. Skins (`ui.json`)
   cannot be previewed outside the plugin: check them with `validate`, then in the plugin.
3. `aicontact.py deploy acid.c --slot bass` → `"ui": "html"` and any `ui_warnings` in the result.
   UI errors (missing files, unknown params, bad `ui.json`) fail validation like audio errors do.
   Hot reload works the same: edit the UI files, deploy again with the same `--slot`, the page reloads.
4. Tell the user to click the slot in the plugin to open the panel ("Custom UI" / "Default UI" toggles).

Other commands: `ui list FILE.wasm`, `ui extract FILE.wasm DIR` (edit a deployed UI: `export SLOT` first),
`ui pack FILE.wasm DIR` / `ui pack FILE.wasm --strip`.

Limits: no network at all (external URLs, CDNs and web fonts do not load — bundle everything), ≤ 8 MiB
total, aim for < 1 MiB (the UI is saved inside every DAW project that uses the instrument). File names:
letters, digits, `.`, `_`, `-`, `/`.

## HTML UI

```html
<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="aci-size" content="720x300">   <!-- design size in CSS px (default 720x360) -->
<link rel="stylesheet" href="style.css">
</head><body>
  <input type="range" data-aci-param="Cutoff"> <span data-aci-value="Cutoff"></span>
  <select data-aci-param="Wave"></select>
  <script src="ui.js"></script>
</body></html>
```

The host defines `window.aci` before your scripts run (do not include any bridge script yourself):

| | |
|---|---|
| `aci.params` | `[{index, id, name, min, max, default, unit, scale, steps, labels, value}]` |
| `aci.instrument`, `aci.slot` | `{id, name, version, author, description}`, slot id |
| `aci.param(ref)` | param object by id (number), name (string, case-insensitive) or object |
| `aci.get(ref)`, `aci.set(ref, value)` | plain values (`set` clamps, snaps to `steps` and returns the value used) |
| `aci.getNormalized(ref)`, `aci.setNormalized(ref, n)` | 0..1, honours `"scale":"log"` — use these for knobs |
| `aci.format(ref, value?)` | display text: `"2.50 kHz"`, `"10 ms"`, `"-6.00 dB"`, step labels |
| `aci.onChange(fn)` | `fn(param, value)` on every change: own `set`s, DAW automation, API, other view. Returns an unsubscribe function. Render from here so the UI follows automation |
| `aci.note(key, velocity=100, ms=500)` | audition on this slot (e.g. an on-screen keyboard) |
| `aci.log(msg)` | to the rack log (`aicontact.py logs`, max 20 lines/s); uncaught errors are logged automatically |
| `aci.host` | `"aicontact"` in the plugin, `"preview"` in `ui preview` |

Declarative bindings need no JS: `data-aci-param` on `<input type="range">` (normalized, so log params
feel right; double-click resets), `<select>` (options from `labels`) and `<input type="checkbox">`
(min/max); `data-aci-value` elements show the formatted value. Call `aci.bind()` after adding such
elements dynamically.

Design guidance:
- Lay the panel out at the declared size (`width`/`height` on a root element, `margin: 0 auto`); the
  view may be wider than that, rarely taller. Set `html, body { margin: 0; overflow: hidden; background: ... }`
  and `user-select: none`.
- Knobs: draw them with SVG (an arc for the value, a pointer), drag vertically with pointer capture
  (`setPointerCapture`), ~180 px for the full range, Shift for fine control, double-click → default,
  wheel. Show the value while dragging. Use `aci.setNormalized`.
- Group controls the way the sound works (oscillator → filter → envelope → output), label them
  clearly, keep the font size ≥ 10 px, use one accent colour. Images: SVG or small WebP/PNG in the folder.
- System fonts only unless you bundle a font file (`@font-face { src: url(font.woff2) }`).
- Canvas/requestAnimationFrame animations are fine; the page cannot see audio, only param values.

## Skin (`ui.json`)

```json
{
  "size": [520, 220],
  "background": "panel.svg",
  "color": "#e8dcc4", "accent": "#ffaa3c",
  "elements": [
    {"type": "label", "text": "ACID", "x": 20, "y": 10, "w": 120, "h": 30, "size": 24, "bold": true, "align": "left"},
    {"type": "knob", "param": "Cutoff", "x": 40, "y": 60, "w": 80, "h": 80, "src": "knob.png", "frames": 64},
    {"type": "value", "param": "Cutoff", "x": 30, "y": 142, "w": 100, "h": 16, "size": 11},
    {"type": "knob", "param": "Resonance", "x": 150, "y": 60, "w": 80, "h": 80, "src": "cap.svg"},
    {"type": "knob", "param": "Decay", "x": 260, "y": 60, "w": 80, "h": 80},
    {"type": "slider", "param": "Volume", "x": 380, "y": 50, "w": 36, "h": 120, "src": "thumb.svg"},
    {"type": "toggle", "param": "Accent", "x": 440, "y": 60, "w": 50, "h": 24, "text": "ACC"},
    {"type": "buttons", "param": "Wave", "x": 40, "y": 180, "w": 180, "h": 26},
    {"type": "menu", "param": "Mode", "x": 240, "y": 180, "w": 120, "h": 26}
  ]
}
```

- Root: `size` (required, design pixels; the host scales uniformly), `background` (`"#rrggbb"` or image,
  stretched to `size`), `color` (text), `accent` (controls), `font` (TTF/OTF), `elements` (drawn in order).
- Every element: `type`, `x`, `y`, `w`, `h`; optional `color` (`#rgb`, `#rrggbb`, `#rrggbbaa`).
  `param` = param id or name.
- `image` `src` · `label` `text`, `size` (default 13), `bold`, `align` · `value` `param`, `size`, `bold`, `align`
- `knob`: no `src` → host-drawn knob in `accent`; `src` + `frames` > 1 → filmstrip (frames stacked along
  the image's long side, frame 0 = min); `src` alone → the image is rotated over `angle`
  (`[start, end]` degrees, 0 = up, default `[-135, 135]`), so draw it pointing up.
- `slider`: vertical when `h > w`; `src` = thumb image (aspect kept), `track` = groove image.
- `toggle`: min ⇔ max; `src` = 2-frame filmstrip (off, on), or `text` (+ `size`) for a lit button, else a switch.
- `menu` / `buttons`: params with `steps` (and `labels`). `menu` takes `background` (colour);
  `buttons` takes `size` and lays out vertically when `h > 2 * w`.
- Controls: drag (vertical or horizontal), Shift = fine, double-click = default, mouse wheel; the host
  shows the param name and value as a tooltip.
- SVG support is the basic subset (shapes, paths, linear/radial gradients, transforms) — no filters,
  CSS, masks or text-on-path; put text in `label` elements. Filmstrips can be generated with a script
  (e.g. Python writing a PNG with `zlib`), one frame per value step.
