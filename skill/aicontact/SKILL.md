---
name: aicontact
description: Create, deploy, hot-reload and control software instruments (synths, drum kits, any sound generator) in the AI Contact plugin running in the user's DAW. Use when the user wants to make a new instrument or sound, change how a deployed instrument sounds, route instruments to MIDI channels, audition notes, tweak parameters, design an instrument's GUI (HTML or image skin), or debug an AI Contact instrument. Instruments are single-file WebAssembly modules (ACI format) written in C.
---

# AI Contact

AI Contact is one plugin (VST3/AU) in the user's DAW that hosts many instruments. Each **slot** holds
one instrument (an ACI `.wasm` file) and listens to a MIDI channel (0 = omni) and key range.
Instruments are written in C against `sdk/aci.h`, built with clang, and deployed live over a
localhost API — no plugin rescans, no DAW restarts.

All commands go through `scripts/aicontact.py` (Python stdlib only, prints JSON):

```sh
AICONTACT_TOKEN=<token> python3 <skill-dir>/scripts/aicontact.py <command> ...
```

**Token**: the API needs the token the user copies in the plugin ("API Token" button → Copy token). If a
command says "no API token", ask the user for it and pass it as `AICONTACT_TOKEN` on every command (it is
not stored anywhere for you). "rejected the API token": it was regenerated in the plugin; ask for the new one.

## Workflow for a new instrument

1. **Read `reference/authoring.md` first** (rules, helpers in aci.h, a complete template).
2. Write the C source where the user wants their instruments kept (ask; e.g. a folder next to their DAW
   project, `<project>/instruments/acid.c`). The build lands next to it as `acid.aci.wasm`, and **the DAW
   project refers to that file by its absolute path and reads it every time the project is opened** —
   so never put it in a temporary folder, and tell the user not to move or delete it.
3. Deploy: `aicontact.py deploy <dir>/acid.c --slot bass --channel 2`
   - builds with clang, validates (test render on the plugin's engine), then deploys
   - on `"ok": false` read `error` / `report` (compile errors, spec violations, NaN, crashes), fix, retry
   - `sound_warnings` (silent, clipping, doesn't decay after note-off, DC) are worth fixing too
   - with a custom GUI the result also has `ui` (`"html"` / `"skin"`) and any `ui_warnings`
4. Audition: `aicontact.py note C2 C3 G2 --slot bass --ms 800` — tell the user it is playing in the DAW.
5. Iterate: edit the source and `deploy` again **with the same `--slot`** → hot reload. The slot keeps
   its routing, parameter values carry over by param id, held notes keep sounding. Racks never pick up a
   changed file on their own: always deploy again.
6. Small tweaks without rebuilding: `aicontact.py params bass Cutoff=600 Resonance=4`.

## Custom GUI

When the user wants the instrument to have its own look (a "VST-like" panel, images, a theme), **read
`reference/ui.md`**: put `index.html` (+ CSS/JS/images) or a `ui.json` skin in a folder named
`<source name>.ui/` next to the C file; `deploy` embeds it in the .wasm. Check it visually first with
`aicontact.py ui preview FILE.c --screenshot out.png` and look at the PNG.

## Commands

| command | purpose |
|---|---|
| `racks` | open racks (one per plugin instance, from the rack directory on port 47860). Pass `--rack NAME\|PORT` when there are several |
| `status [--detailed]` | slots, channel, key range, mixer, `cpu`, `peak_db`, `status`/`error`, params (`--detailed`: ranges, units) |
| `build FILE.c [-o out.wasm] [--ui DIR \| --no-ui]` | compile only (default output `<name>.aci.wasm` next to the source); embeds `<name>.ui/` if present |
| `validate FILE [--keys 36,38,42] [--seconds 2] [--wav out.wav] [--no-sweep]` | test render without deploying: peak/RMS, NaN, clipping, DC, tail after release, `cpu_percent` (plugin engine), min/max sweep of every param, custom UI check (`ui.errors` fail it) |
| `deploy FILE [--slot ID] [--name NAME] [--channel N] [--key-low N --key-high N] [--transpose N] [--gain-db X] [--pan X] [--params JSON] [--force]` | build + validate + deploy the file by path; existing `--slot` = hot reload (also fixes a slot whose file went missing). `--force` skips validation |
| `note KEY... [--slot ID \| --channel N] [--velocity V] [--ms D]` | play notes (MIDI numbers or names, C4 = 60) |
| `params SLOT Name=Value...` | set params (names case-insensitive or ids; plain values within min..max) |
| `update SLOT [--channel N] [--key-low/--key-high] [--transpose] [--gain-db] [--pan] [--mute/--no-mute] [--solo/--no-solo] [--name] [--index N]` | routing / mixer / position. Slots 1-16 × first 8 params are DAW-automatable |
| `remove SLOT`, `clear`, `panic` | remove one / all slots, all notes off |
| `logs [--since SEQ]` | deploys, reloads, crashes (trap, out of gas), NaN warnings, `aci_print` output, custom UI problems and page messages/errors (`ui: ...`) |
| `export SLOT [-o file]` | save the deployed .wasm (default: current folder) |
| `rename NAME` | rename the rack |
| `ui preview FILE [-o DIR] [--screenshot PNG]` | render an HTML GUI with simulated params (headless Chrome/Edge for the PNG) |
| `ui list FILE.wasm`, `ui extract FILE.wasm DIR`, `ui pack FILE.wasm [DIR] [--strip] [-o OUT]` | inspect / edit / replace / remove the GUI files embedded in a module (`reference/ui.md`) |

`build`, `validate`, `deploy` and `ui preview` embed the folder `<source name>.ui/` next to FILE when it
exists; `--ui DIR` picks another folder, `--no-ui` skips it.
| `rpc METHOD [JSON]` | raw API call (see `reference/api.md`) |

## Tips

- Splits and layers: several slots on one channel with different key ranges, e.g. bass `--key-high 59`,
  pad `--key-low 60`. Drums conventionally on channel 10 with GM keys (36 kick, 38 snare, 42 hat).
- If no rack is running, ask the user to open the DAW project with AI Contact (or run `aicontact-host serve`
  for a headless rack; it prints the token to use). `validate` still works without a rack when `aicontact-host` is on PATH.
- Racks read instrument files themselves, so paths must exist on the rack's machine. Claude in WSL with the
  DAW on Windows works as is: the CLI passes WSL files as `\\wsl.localhost\...` paths. Only the directory port
  needs to reach Windows: with WSL's `networkingMode=mirrored` it already does; otherwise the user forwards it
  (`netsh interface portproxy add v4tov4 listenaddress=<WSL gateway IP> listenport=47860
  connectaddress=127.0.0.1 connectport=47860` + a firewall rule) and you set `AICONTACT_ADDR=<that IP>`
  (`ip route | awk '/^default/{print $3}'` in WSL). `--wav` paths are written by the rack, too.
- Each DAW project stores which file each slot uses (absolute path), the file's SHA-256 when it was
  deployed, its routing and params. A slot shows `status: "error"` when its file is missing, broken or
  **changed since it was deployed** (nothing plays; a changed file is not even run, so a replaced file cannot
  sneak code into the DAW). Fix it by deploying the file to that slot (`deploy FILE --slot ID`) or with
  "Locate..." in the plugin. So after rebuilding an instrument another project also uses, that project's slot
  reports the change until the user redeploys there: tell them, and copy the source per project when they
  want projects independent.
- `status` → `cpu` is the fraction of the audio budget a slot uses; keep the total well below 1.
- If `clang` lacks the wasm32 target: Windows → install LLVM from releases.llvm.org;
  macOS → `brew install llvm` (Apple clang has no wasm32); Linux → `clang` + `lld`.
- Tool overrides: `AICONTACT_CLANG` (compiler), `AICONTACT_HOST_BIN` (`aicontact-host` for rack-less
  validation), `AICONTACT_BROWSER` (Chrome/Edge for `ui preview --screenshot`; from WSL the Windows browser
  is found automatically), `AICONTACT_PORT` (rack directory port, default 47860).
- References: `reference/authoring.md` (instrument C code), `reference/ui.md` (custom GUIs),
  `reference/api.md` (HTTP API); the normative format spec is `docs/ACI-SPEC.md` in the AI Contact repository.
