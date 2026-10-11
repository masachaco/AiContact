# AI Contact localhost API

Each AI Contact plugin instance (or `aicontact-host serve`) is a **rack** with its own HTTP port on
127.0.0.1 (47861-47899). One of them also holds the **rack directory** port **47860**: the other racks
register with it every second, it lists them (`racks.list`) and relays any request that names a rack
(`"rack"`), so clients only ever talk to 47860. When the directory's rack closes, another rack takes the
port over within a few seconds. `AICONTACT_PORT` moves the directory port (racks use the 39 ports after it).
For racks on another machine only 47860 needs forwarding to its 127.0.0.1 (the skill CLI then uses
`AICONTACT_ADDR` and sends `Host: localhost:<port>`).

**Authentication**: every request (including `/health`) needs `Authorization: Bearer <token>`: a
64-hex-character secret the plugin keeps in its settings (shared by all its instances). The user copies it
from the editor's "API Token" button (which can also regenerate it; old tokens are rejected at once);
`aicontact-host serve` prints its own (or takes `--token`). Missing or wrong tokens get HTTP 401. A rack
whose token differs from the directory's cannot register and is reachable on its own port only.

- `GET /health` → `{"ok": true, "name", "port", "pid", "os", "version", "aci_abi", "gateway", "gateway_port"}`
- `POST /rpc` with `{"method": "...", "params": {...}, "rack"?: name | port}` → `{"result": ...}` or
  `{"error": "message"}`. Without `"rack"` the receiving rack answers; with it, the directory relays the
  request to that rack (names: exact, then case-insensitive; ambiguous names must use the port).

Requests carrying an `Origin` header or a non-local `Host` header are rejected (browsers cannot drive
the rack). Bodies up to 64 MiB.

**Files**: slots always refer to an instrument file on the rack's machine (`path`: a regular file ending in
`.wasm`, at most 64 MiB, checked before it is opened). A DAW project stores each slot's path, the file's
SHA-256 at deploy time, routing and params — no binaries — and reads the files again whenever it is opened.
A file that is missing, invalid or **changed since it was deployed** (hash mismatch, checked before any of its
code runs) puts the slot in `status: "error"` (silent, `error` says why) until a file is deployed to it —
deploying is what makes a file (or a new version of it) trusted. Racks do not watch files: deploy again to
pick up a rebuilt file.

```sh
curl -s -H "Authorization: Bearer $AICONTACT_TOKEN" localhost:47860/rpc -d '{"method":"racks.list"}'
curl -s -H "Authorization: Bearer $AICONTACT_TOKEN" localhost:47860/rpc -d '{"method":"slot.note","rack":"Song A","params":{"slot_id":"pad","keys":["C4","E4"]}}'
```

## Methods

| method | params | result |
|---|---|---|
| `racks.list` | (directory only) | `{racks:[{name, port, pid, os, version, gateway}]}` |
| `racks.register` / `racks.unregister` | `name, port, pid, os, version` / `port` | used by racks among themselves |
| `rack.info` | `detailed?` | `{name, version, aci_abi, sample_rate, audio_running, max_slots, slots:[slot]}` |
| `rack.set_name` | `name` | `{name}` |
| `rack.panic` | | all notes off on every slot |
| `rack.clear` | | removes every slot |
| `rack.logs` | `since?` (seq) | `{lines:[{seq, time, level, slot, message}]}` |
| `instrument.validate` | `wasm_base64` \| `path`, `keys?`, `seconds?`, `wav_path?`, `param_sweep?` | report: `ok, size_bytes, hash, meta, audio{keys, peak_dbfs, rms_dbfs, dc_offset, nan_or_inf_samples, clipped_samples, stereo, tail_rms_dbfs, warnings}, cpu_percent, param_sweep_problems, ui{kind, files, bytes, size, errors, warnings}, wav_path?, log?, error?`. UI errors make `ok` false |
| `slot.deploy` | `path`, `slot_id?`, `name?`, `channel?`, `key_low?`, `key_high?`, `transpose?`, `gain_db?`, `pan?`, `mute?`, `solo?`, `params?` | slot + `reloaded` (true = hot reload of an existing `slot_id`, also how a slot in error gets its file back) |
| `slot.remove` | `slot_id` | `{removed}` |
| `slot.update` | `slot_id` + any of the settings above, `params?` | slot |
| `slot.set_params` | `slot_id`, `params: {name_or_id: value}` | slot |
| `slot.info` | `slot_id` | slot (detailed) + `meta_json` |
| `slot.move` | `slot_id`, `index` | `{id, index}` |
| `slot.note` | `slot_id?` \| `channel?` (default 1), `key?` / `keys?` (numbers or "C4"), `velocity?` (100), `duration_ms?` (500) | `{keys, velocity, duration_ms}` |
| `slot.export` | `slot_id` | `{slot_id, filename, wasm_base64}` |

Slot object: `{id, index, name, instrument{id, name, version, author, hash (SHA-256), size, (detailed: description, tags,
outputs, host_sustain, ui{kind ("html" | "skin" | "none"), files, bytes})}, channel (0 = omni),
key_low, key_high, transpose, gain_db, pan, mute, solo, cpu, peak_db, status ("ok" | "crashed" | "error" | "empty"),
error? (crash reason, or why the file could not be loaded), nan_samples?, source{path},
params[{index, id, name, value, (detailed: min, max, default, unit, scale, steps, labels)}]}`.
