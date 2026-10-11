# CLAUDE.md

AI Contact: a VST3/AU/Standalone plugin (JUCE) that hosts many software instruments at once and
hot-reloads them at runtime. Instruments are single-file WebAssembly modules in the **ACI** format,
deployed through a localhost HTTP/JSON API, normally driven by the Claude skill in `skill/aicontact/`.
User-facing docs (README, spec) are in Japanese; code, comments and the skill are in English.

## Layout

| path | what |
|---|---|
| `docs/ACI-SPEC.md` | ACI v1 format spec (normative). ABI = 7 required + 6 optional exports, metadata JSON, optional custom UI (§10) |
| `sdk/c/aci.h` | C SDK for instruments: ABI declarations, self-contained libm, oscillators, ADSR, SVF, voice allocator |
| `sdk/web/aci-bridge.js` | `window.aci` injected into HTML instrument UIs (plugin embeds it as binary data; the skill's copy runs previews) |
| `core/` | JUCE-free C++17 engine (`aicontact_core`): `WasmInfo` (binary inspection), `Instrument` (wasm3 instance), `Rack` (slots, routing, snapshots, project state, RPC), `Analyze` (validation/render), `Ui` (custom UI bundle in `aci.ui/*` custom sections, skin/HTML checks), `ControlServer` (HTTP, rack directory/relay on 47860), `Auth` (token helpers), `Log`, `Util` |
| `plugin/Source/` | JUCE plugin: `PluginProcessor`, `PluginEditor` (slot list, log, drag & drop, Locate for broken slots), `ParamPanel` (per-instrument UI generated from param metadata, or the instrument's own), `CustomUi` (native skin renderer + WebView host for HTML UIs), `ApiToken` (token in the app settings) |
| `host/aicontact_host.cpp` | `aicontact-host`: `check` (metadata + custom UI report) / `validate` / `render` / `serve` (headless rack; prints its token, `--token`, `--state FILE` stands in for a DAW project) |
| `skill/aicontact/` | Claude skill: `SKILL.md`, stdlib-only CLI `scripts/aicontact.py`, `reference/authoring.md`, `reference/ui.md`, `reference/api.md`, bundled `sdk/aci.h` + `sdk/aci-bridge.js` |
| `examples/` | instrument sources (`c/`, `wat/`), custom UIs in `c/<name>.ui/` (supersaw: HTML, pluck: skin), committed binaries in `prebuilt/` (rebuild with `examples/build.sh`, which embeds the UIs), `tools/gen_filmstrip.py` (stdlib PNG filmstrip knob) |
| `tests/` | `test_cli_e2e.py` (CLI against `aicontact-host serve`: auth, remote rack, deploy, params, custom UIs, hot reload, errors), `plugin_host/` (JUCE VST3 hosting test) |

## Build & test

```sh
# core + headless host only (fast, ~30 s)
cmake -S . -B build -DAICONTACT_BUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# full plugin (+ VST3 hosting test); JUCE build with LTO takes ~10 min
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAICONTACT_BUILD_PLUGIN_TEST=ON && cmake --build build -j

python3 tests/test_cli_e2e.py build/aicontact-host           # end-to-end: deploy, params, UIs, hot reload, errors
build/aicontact-host validate examples/prebuilt/supersaw.aci.wasm
examples/build.sh                                          # needs clang with wasm32 + wasm-ld (WAT example: pip wasmtime)
python3 skill/aicontact/scripts/aicontact.py ui preview examples/c/supersaw.c --screenshot ui.png  # HTML UI, headless Chrome
```

Dependencies come from FetchContent (JUCE 8.0.4, wasm3 pinned commit, nlohmann_json 3.11.3).
Offline: `-DFETCHCONTENT_SOURCE_DIR_JUCE=... -DFETCHCONTENT_SOURCE_DIR_WASM3=...`.
Linux plugin builds need the X11/ALSA/freetype dev packages listed in `.github/workflows/build.yml`, plus
`libwebkit2gtk-4.1-dev libgtk-3-dev` for HTML instrument UIs. Windows needs the `Microsoft.Web.WebView2` NuGet
package (`-DJUCE_WEBVIEW2_PACKAGE_LOCATION=<dir>`, see the workflow). Without them `AICONTACT_WEBVIEW` switches
itself off (or pass `-DAICONTACT_WEBVIEW=OFF`): skins and the generated panel still work.

Environment: `AICONTACT_PORT` (rack directory port, default 47860; racks use the next 39 — tests set it to
stay away from running racks), `AICONTACT_TOKEN` (CLI: the API token), `AICONTACT_ADDR` (CLI → racks on another
machine), `AICONTACT_GAS_PER_FRAME` (gas override), `AICONTACT_CLANG`, `AICONTACT_HOST_BIN`, `AICONTACT_BROWSER`
(CLI tool overrides). Nothing lives in a per-user data folder (`~/.aicontact` is gone).

## CI / releases

`.github/workflows/build.yml`: `core-and-skill` (Linux + Windows: SDK copies identical + bridge ASCII-only, host
build, examples, CLI E2E) and `plugin` (Windows/macOS/Linux: build with HTML UI support required — WebKitGTK via
apt, WebView2 SDK downloaded from nuget.org — then load the VST3 in the test host, upload `AIContact-<OS>` artifacts).
Pushing a `v*` tag runs `release`, which publishes per-OS zips and the skill zip to GitHub Releases.
On Windows the multi-config generator puts binaries in `build/Release/`; Python is `python`, not `python3`.

## Rules that matter

- **Audio thread (`Rack::process`, `InstrumentInstance` audio methods) must not lock or allocate.**
  Control operations build a new `Snapshot` and publish it atomically (`publishLocked`); old snapshots
  are freed by `collectGarbageLocked` using the epoch counter. Keep that pattern for any new state.
- Everything that calls into a wasm instance after publication happens on the audio thread only;
  instantiation/compilation happens on control threads (wasm3 is not thread-safe).
- ACI is **pre-release**: breaking ABI/format changes are fine (the user decided; no version bumps or
  compatibility shims needed, `aci` stays 1 for now). Keep `docs/ACI-SPEC.md`, `WasmInfo.cpp`
  (`checkAciStructure`) / `Ui.cpp`, `sdk/c/aci.h`, the examples and `skill/aicontact/reference/*.md` in sync.
- Modules may import only `env.aci_log`. Math/memory functions live in `aci.h`.
- `skill/aicontact/sdk/aci.h` must stay byte-identical to `sdk/c/aci.h`, and `skill/aicontact/sdk/aci-bridge.js`
  to `sdk/web/aci-bridge.js` (CI checks with `cmp`); copy after edits.
- Custom UIs (spec §10) never block loading: `InstrumentModule::load` ignores UI problems, `Rack` logs them,
  `instrument.validate` reports them (`ui.errors` → `ok: false`) and the editor falls back to the default panel.
  HTML UIs get no network: `CustomUi.cpp` only serves the bundle, injects a CSP and refuses navigation.
  The page ↔ host protocol is JUCE's event channel `"aci"` (`hello` / `set` / `note` / `log` up, `values` down);
  the initial state is the `window.__ACI_INIT__` user script, injected before `aci-bridge.js`.
- `sdk/web/aci-bridge.js` must stay **ASCII-only** and anything sent to a Linux web view must be ASCII
  (`__ACI_INIT__` is dumped with `ensure_ascii`, the bridge escapes log text): JUCE 8.0.4's Linux web view sizes
  IPC messages in characters, not bytes, so non-ASCII text truncates them and kills the GTK child and the host.
  JUCE's Linux web view also ignores visibility, so `CustomUi.cpp` detaches the browser while it is not showing.
  Skin keys are checked in `core/src/Ui.cpp` and drawn in `plugin/Source/CustomUi.cpp` — change both and
  `docs/ACI-SPEC.md` §10 / `skill/aicontact/reference/ui.md` together.
- `AICONTACT_WEBVIEW` (plugin CMake) turns itself off when WebView2 (Windows NuGet package) or WebKitGTK
  dev packages are missing; CI requires it on.
- The control API is authenticated: `ControlServer::handleClient` rejects any request without
  `Authorization: Bearer <token>` equal (constant time) to what the app's `TokenProvider` returns, asked per
  request. The plugin keeps the token in its JUCE settings (`ApiToken.cpp`, shared by all instances, reloaded
  when another process rotates it); the editor's "API Token" button copies/regenerates it; the headless host
  takes `--token` or generates one. No token file for clients: users hand it over (`AICONTACT_TOKEN`).
  Never add an unauthenticated endpoint.
- Discovery: each rack listens on its own port; whoever binds the directory port (47860) answers
  `racks.list`, accepts `racks.register` heartbeats (ports in its range only) and relays requests with
  `"rack"`. Racks take the port over when it frees up. Requests run on their own threads (max 16).
- **Projects store paths, not binaries**: `Rack::saveState` writes each slot's file path, its SHA-256
  (`Slot::sha256`, set on deploy), routing and params; `loadState` reads every file again and compares the hash
  **before `InstrumentModule::load`** (which runs module code). Missing/changed/invalid files leave the slot
  silent with `status: "error"` — never substitute a stale copy, never load an unhashed or mismatching file.
  Deploying is the only way to trust a (new) file. No file watching: clients redeploy. `slot.deploy` takes
  `path` only. `InstrumentModule::hash()` is SHA-256 (`sha256Hex` in `Util.cpp`). Apply the same rule to any
  other file a slot reads (assets).
- API `path` parameters go through `readInstrumentFile` (`Rack.cpp`): regular `.wasm` files ≤ 64 MiB only,
  checked before opening. Use it for any new path-taking method.
- New RPC methods: implement in `Rack::handleRpc`, document in `skill/aicontact/reference/api.md`,
  expose in `scripts/aicontact.py` if useful, cover in `tests/test_cli_e2e.py`.
- The skill CLI must remain Python-stdlib-only (no pip installs for users). It talks only to the directory
  port (with `"rack"`), builds next to the source and deploys by path; for a Windows rack from WSL it converts
  paths with `wslpath -w` (`rack_path`). `validate` sends bytes (`wasm_base64`).
- Gas budget (`gasPerFrame`, `kGasEvent` in `Instrument.cpp`) is calibrated so a hung module traps in
  ~20–40 ms while heavy legitimate patches use ~1/20 of it; re-measure if changed.
- MSVC: wasm3 needs `/Zc:preprocessor` (set in the top-level `CMakeLists.txt`).

## Licenses

Own code: MIT (`LICENSE`, copy in `skill/aicontact/LICENSE` for the skill zip). Instrument SDK and examples:
MIT-0 (`sdk/LICENSE`, `skill/aicontact/sdk/LICENSE`, `examples/LICENSE`) because `aci.h` is compiled into
instruments. Plugin binaries ship under AGPLv3 (JUCE) with the GPLv3 VST3 SDK; release zips include
`LICENSE` and `THIRD_PARTY_NOTICES.md` — update the notices when adding a dependency.

## Gotchas

- JSON output of floats goes through `num()` in `Rack.cpp` (6 significant digits) to avoid `0.30000001`.
- Screenshots: run the Standalone app under Xvfb (e.g. `:99`), deploy with the CLI, click with xdotool and
  capture with ImageMagick `import -window root`. With audio, use PulseAudio's null sink
  (`~/.asoundrc` → `pcm.!default { type pulse }`); the ALSA null device has no clock and runs audio faster
  than real time. The JUCE VST3 test host cannot capture the hosted editor on Linux. HTML UIs can be
  checked without the plugin via `aicontact.py ui preview --screenshot`.
- When killing the Standalone, use `pkill -x "AI Contact"` — `pkill -f <path>` matches its own shell.
