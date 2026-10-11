#!/usr/bin/env python3
"""aicontact — command-line client for AI Contact racks (localhost API). Python stdlib only.

Every command prints JSON. Run `aicontact.py <command> -h` for options.

  racks                               list running racks
  status [--detailed]                 slots, routing, CPU, peaks, errors, params
  build SRC.c [-o OUT.wasm] [--ui DIR]   compile C (or .wat) to an ACI .wasm (+ custom UI)
  validate FILE [--keys 48,60] [--wav out.wav]   test render + analysis (no deploy)
  deploy FILE [--slot ID] [--channel N] ...      build, check and (hot-)deploy
  note KEY... [--slot ID | --channel N]          audition through the DAW
  params SLOT NAME=VALUE...           set instrument parameters
  update SLOT [--channel N] [--mute] ...         routing / mixer
  remove SLOT | clear | panic | logs [--since N] | export SLOT [-o F] | rename NAME
  ui list|extract|pack|preview ...    custom UI files embedded in a .wasm (see `ui -h`)
  rpc METHOD [JSON]                   raw API call

FILE may be .c (built with the bundled sdk/aci.h), .wat (needs wat2wasm) or .wasm; sources are built to
`<name>.aci.wasm` next to them. A folder `<name>.ui/` next to FILE (index.html or ui.json + assets) is
embedded as its custom UI; --ui DIR picks another folder, --no-ui skips it.

Racks read instrument files by path and DAW projects store those paths, so keep built files in place.
The API token comes from $AICONTACT_TOKEN (copy it in the plugin: "API Token" -> Copy token).
Racks are found through the rack directory on port 47860 ($AICONTACT_PORT, $AICONTACT_ADDR).
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

SKILL_DIR = pathlib.Path(__file__).resolve().parent.parent
_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # localhost only


class Fail(Exception):
    pass


# ---------------------------------------------------------------------------
# Rack discovery and API


def gateway_port() -> int:
    """The rack directory: one well-known port that lists every open rack and relays requests to it."""
    return int(os.environ.get("AICONTACT_PORT") or 47860)


def api_token() -> str:
    """The API secret, from $AICONTACT_TOKEN: the user copies it from the plugin ("API Token" -> Copy token)."""
    return (os.environ.get("AICONTACT_TOKEN") or "").strip()


def rack_addr() -> str:
    """Where racks listen: 127.0.0.1, or $AICONTACT_ADDR for racks on another machine whose directory port
    is forwarded to that machine's loopback (e.g. WSL -> Windows host via `netsh interface portproxy`)."""
    return os.environ.get("AICONTACT_ADDR") or "127.0.0.1"


def remote_rack() -> bool:
    return rack_addr() not in ("127.0.0.1", "localhost")


_NO_TOKEN = ("no API token. Ask the user to copy it in the AI Contact plugin (\"API Token\" button -> Copy token) "
             "and set it as AICONTACT_TOKEN for these commands")


def _post(body: dict, timeout: float) -> dict:
    token = api_token()
    if not token:
        raise Fail(_NO_TOKEN)
    port = gateway_port()
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {token}"}
    if remote_rack():
        headers["Host"] = f"localhost:{port}"  # racks reject foreign Host headers (DNS rebinding guard)
    req = urllib.request.Request(f"http://{rack_addr()}:{port}/rpc", data=json.dumps(body).encode(), headers=headers)
    try:
        with _NO_PROXY.open(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        if e.code == 401:
            raise Fail("the rack rejected the API token: it may have been regenerated in the plugin, copy it again "
                       "(\"API Token\" -> Copy token) into AICONTACT_TOKEN")
        try:
            return json.loads(e.read().decode())
        except Exception:
            raise Fail(f"rack directory returned HTTP {e.code}")
    except (urllib.error.URLError, OSError) as e:
        where = f"{rack_addr()}:{port}"
        hint = (" Check that the DAW (or `aicontact-host serve`) is running and that the port is forwarded to that "
                "machine's 127.0.0.1." if remote_rack() else
                " Open a DAW project with the AI Contact plugin, or run `aicontact-host serve`.")
        raise Fail(f"no AI Contact reachable at {where} ({getattr(e, 'reason', e)}).{hint}")


def discover() -> list[dict]:
    """Every open rack: [{name, port, os, pid, version, gateway}] from the rack directory."""
    reply = _post({"method": "racks.list"}, 5.0)
    if "error" in reply:
        raise Fail(reply["error"])
    return reply["result"]["racks"]


def pick_rack(rack: str | None) -> dict:
    racks = discover()
    if not rack:
        if len(racks) == 1:
            return racks[0]
        raise Fail("several racks are running, pass --rack NAME|PORT: " + ", ".join(f"{r['name']} ({r['port']})" for r in racks))
    for r in racks:
        if rack == str(r["port"]) or rack.lower() == r["name"].lower():
            return r
    raise Fail(f"no rack '{rack}'. running: " + ", ".join(f"{r['name']} ({r['port']})" for r in racks))


def rpc(rack: dict, method: str, params: dict | None = None, timeout: float = 60.0):
    reply = _post({"method": method, "params": params or {}, "rack": rack["port"]}, timeout)
    if "error" in reply:
        raise Fail(reply["error"])
    return reply["result"]


def _in_wsl() -> bool:
    try:
        return sys.platform.startswith("linux") and "microsoft" in pathlib.Path("/proc/version").read_text().lower()
    except OSError:
        return False


def rack_path(rack: dict, path: pathlib.Path) -> str:
    """`path` as the rack's machine sees it. Racks read instrument files themselves (projects store the path),
    so a Windows rack driven from WSL gets the \\\\wsl.localhost\\... form of a WSL file."""
    path = path.resolve()
    if rack.get("os") == "windows" and _in_wsl() and shutil.which("wslpath"):
        out = subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    return str(path)


# ---------------------------------------------------------------------------
# Building


def find_clang() -> str | None:
    for c in [os.environ.get("AICONTACT_CLANG"), "clang", "clang-19", "clang-18", "clang-17",
              "/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang",
              r"C:\Program Files\LLVM\bin\clang.exe"]:
        if not c:
            continue
        path = shutil.which(c) or (c if os.path.isfile(c) else None)
        if not path:
            continue
        try:
            if "wasm32" in subprocess.run([path, "--print-targets"], capture_output=True, text=True, timeout=10).stdout:
                return path
        except Exception:
            pass
    return None


def slugify(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:40] or "instrument"


def build(src: pathlib.Path, out: pathlib.Path | None = None, ui: pathlib.Path | None = None) -> pathlib.Path:
    """Compiles .c / .wat to `<name>.aci.wasm` next to the source (or `out`) and embeds the custom UI
    folder `ui` (if any). A .wasm is updated in place when `ui` is given, else returned unchanged."""
    if src.suffix == ".wasm":
        if ui is None and out is None:
            return src
        out = out or src
        data = src.read_bytes()
        tmp = out.with_name(out.name + ".tmp")
        tmp.write_bytes(pack_ui(data, ui) if ui else data)
        os.replace(tmp, out)
        return out
    if out is None:
        out = src.with_name(src.stem + ".aci.wasm")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Build next to the target and rename, so a rack loading `out` never sees a half-written file
    # or a module without its UI.
    tmp = out.with_name(out.name + ".tmp")
    if src.suffix in (".c", ".cc", ".cpp"):
        clang = find_clang()
        if not clang:
            raise Fail("clang with the wasm32 target not found. Install LLVM (Windows: releases.llvm.org, "
                       "macOS: `brew install llvm`, Linux: clang + lld) or set AICONTACT_CLANG.")
        cmd = [clang, "--target=wasm32", "-O2", "-nostdlib", "-fno-exceptions", "-Wall",
               "-Wno-unused-function", "-Wno-unused-parameter", "-Wl,--no-entry", "-Wl,-z,stack-size=65536",
               "-Wl,--strip-debug", "-I", str(SKILL_DIR / "sdk"), "-o", str(tmp), str(src)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if proc.returncode != 0:
            raise Fail("compile failed:\n" + (proc.stderr or proc.stdout))
    elif src.suffix in (".wat", ".wast"):
        tool = shutil.which("wat2wasm")
        if not tool:
            raise Fail("wat2wasm (wabt) not found; install wabt or write the instrument in C")
        proc = subprocess.run([tool, str(src), "-o", str(tmp)], capture_output=True, text=True)
        if proc.returncode != 0:
            raise Fail("wat2wasm failed:\n" + proc.stderr)
    else:
        raise Fail(f"unsupported file type: {src}")
    if ui:
        tmp.write_bytes(pack_ui(tmp.read_bytes(), ui))
    os.replace(tmp, out)
    return out


# ---------------------------------------------------------------------------
# Custom UI: files embedded as wasm custom sections "aci.ui/<path>" (ACI spec §10)

UI_PREFIX = "aci.ui/"
_UI_NAME = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")


def _leb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _read_leb(data: bytes, i: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        if i >= len(data):
            raise Fail("malformed wasm binary (truncated LEB128)")
        b = data[i]
        i += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, i


def wasm_sections(data: bytes) -> list[tuple[int, int, int, str | None]]:
    """[(id, start, end, custom_name)] where data[start:end] is the whole section."""
    if data[:4] != b"\0asm":
        raise Fail("not a WebAssembly binary")
    out, i = [], 8
    while i < len(data):
        start, sid = i, data[i]
        size, body = _read_leb(data, i + 1)
        end = body + size
        if end > len(data):
            raise Fail("malformed wasm binary (section exceeds file)")
        name = None
        if sid == 0:
            n, j = _read_leb(data, body)
            name = data[j:j + n].decode("utf-8", "replace")
        out.append((sid, start, end, name))
        i = end
    return out


def ui_files(data: bytes) -> dict[str, bytes]:
    files = {}
    for sid, start, end, name in wasm_sections(data):
        if sid == 0 and name and name.startswith(UI_PREFIX):
            _, body = _read_leb(data, start + 1)
            n, j = _read_leb(data, body)
            files[name[len(UI_PREFIX):]] = data[j + n:end]
    return files


def strip_ui(data: bytes) -> bytes:
    keep = [data[start:end] for sid, start, end, name in wasm_sections(data)
            if not (sid == 0 and name and name.startswith(UI_PREFIX))]
    return data[:8] + b"".join(keep)


def collect_ui(ui_dir: pathlib.Path) -> dict[str, bytes]:
    if not ui_dir.is_dir():
        raise Fail(f"UI folder not found: {ui_dir}")
    files = {}
    for f in sorted(ui_dir.rglob("*")):
        rel = f.relative_to(ui_dir).as_posix()
        if not f.is_file() or any(part.startswith(".") for part in rel.split("/")):
            continue
        if not _UI_NAME.match(rel):
            raise Fail(f"UI file name '{rel}' may only use letters, digits, '.', '_', '-' and '/'")
        files[rel] = f.read_bytes()
    if "index.html" not in files and "ui.json" not in files:
        raise Fail(f"{ui_dir} needs an index.html (HTML UI) or a ui.json (skin)")
    return files


def pack_ui(data: bytes, ui_dir: pathlib.Path) -> bytes:
    """Replaces the module's UI with the files of `ui_dir`."""
    out = bytearray(strip_ui(data))
    for rel, content in collect_ui(ui_dir).items():
        name = (UI_PREFIX + rel).encode()
        payload = _leb(len(name)) + name + content
        out += b"\0" + _leb(len(payload)) + payload
    return bytes(out)


def resolve_ui(src: pathlib.Path, ui: str | None, no_ui: bool = False) -> pathlib.Path | None:
    """--ui DIR, else a `<name>.ui/` folder next to the source (supersaw.c -> supersaw.ui/)."""
    if no_ui:
        return None
    if ui:
        return pathlib.Path(ui).expanduser()
    stem = src.name[:-len(".aci.wasm")] if src.name.endswith(".aci.wasm") else src.stem
    auto = src.with_name(stem + ".ui")
    return auto if auto.is_dir() else None


_PREVIEW_PAGE = """<!doctype html><meta charset="utf-8"><title>{title} - UI preview</title>
<style>html,body{{margin:0;height:100%;background:#16181d;color:#8a909c;font:13px system-ui}}
#bar{{padding:6px 10px}} iframe{{border:0;display:block;margin:10px;background:#1f232b}}</style>
<div id="bar">AI Contact UI preview: {title} &mdash; parameter changes are local; notes are logged to the console</div>
<iframe src="index.html" style="width:{w}px;height:{h}px"></iframe>"""


def ui_preview(wasm: pathlib.Path, meta: dict, out_dir: pathlib.Path, width: int, height: int) -> pathlib.Path:
    """Writes the module's HTML UI to `out_dir` with the bridge in preview mode."""
    files = ui_files(wasm.read_bytes())
    if "index.html" not in files:
        raise Fail("the module has no HTML UI (aci.ui/index.html); skins (ui.json) are previewed in the plugin")
    if out_dir.exists():
        shutil.rmtree(out_dir)
    for rel, content in files.items():
        dest = out_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(content)
    params = [dict(p, value=p.get("default", p["min"])) for p in meta.get("params", [])]
    state = {"slot": "preview", "params": params,
             "instrument": {k: meta.get(k, "") for k in ("id", "name", "version", "author", "description")}}
    bridge = (SKILL_DIR / "sdk" / "aci-bridge.js").read_text(encoding="utf-8")
    inject = ("<script>window.__ACI_PREVIEW__ = " + json.dumps(state).replace("</", "<\\/") + ";</script>"
              "<script>" + bridge + "</script>")
    html = files["index.html"].decode("utf-8", "replace")
    m = re.search(r"<head[^>]*>", html, re.I)
    html = html[:m.end()] + inject + html[m.end():] if m else inject + html
    (out_dir / "index.html").write_text(html, encoding="utf-8")
    page = out_dir / "_preview.html"
    page.write_text(_PREVIEW_PAGE.format(title=meta.get("name", "instrument"), w=width, h=height), encoding="utf-8")
    return page


def find_browser() -> str | None:
    env = os.environ.get("AICONTACT_BROWSER")
    if env:
        return env
    for name in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "chrome", "msedge",
                 "microsoft-edge"):
        if shutil.which(name):
            return shutil.which(name)
    for path in (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
                 r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                 "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                 "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                 # WSL: the Windows browsers
                 "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe",
                 "/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"):
        if os.path.isfile(path):
            return path
    return None


def _browser_path(browser: str, path: pathlib.Path) -> str:
    """A path the browser understands (a Windows browser launched from WSL needs a Windows path)."""
    if browser.lower().endswith(".exe") and os.name != "nt" and shutil.which("wslpath"):
        return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True).stdout.strip()
    return str(path)


def screenshot(page: pathlib.Path, png: pathlib.Path, width: int, height: int) -> None:
    browser = find_browser()
    if not browser:
        raise Fail("no Chrome/Chromium/Edge found for --screenshot (set AICONTACT_BROWSER); open the preview "
                   f"page instead: {page}")
    png.parent.mkdir(parents=True, exist_ok=True)
    png = png.resolve()
    url = _browser_path(browser, page.resolve())
    url = url if url.startswith("\\\\") or re.match(r"^[A-Za-z]:", url) else page.resolve().as_uri()
    cmd = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--allow-file-access-from-files",
           f"--window-size={width},{height}", f"--screenshot={_browser_path(browser, png)}", url]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if not png.exists():
        raise Fail("screenshot failed: " + (proc.stderr or proc.stdout)[-800:])


def find_host_bin() -> str | None:
    return os.environ.get("AICONTACT_HOST_BIN") or shutil.which("aicontact-host")


def validate(path: pathlib.Path, rack_name: str | None, keys: list | None, seconds: float,
             wav: str | None, sweep: bool) -> dict:
    """Validates via a running rack (same engine as the plugin), else via aicontact-host."""
    # Send the bytes, not the path: the rack may not see this machine's file system.
    params = {"wasm_base64": base64.b64encode(path.read_bytes()).decode(), "seconds": seconds, "param_sweep": sweep}
    if keys:
        params["keys"] = keys
    try:
        rack = pick_rack(rack_name)
    except Fail as e:
        rack, why = None, str(e)
    if rack:
        if wav:  # the rack writes the WAV itself, so the path must make sense on its machine
            params["wav_path"] = rack_path(rack, pathlib.Path(wav))
        return rpc(rack, "instrument.validate", params, timeout=120)
    host = find_host_bin()
    if not host:
        raise Fail(f"validation needs a rack or the aicontact-host binary (set AICONTACT_HOST_BIN). Rack: {why}")
    cmd = [host, "validate", str(path), "--seconds", str(seconds)]
    if keys:
        cmd += ["--keys", ",".join(str(k) for k in keys)]
    if wav:
        cmd += ["--wav", wav]
    if not sweep:
        cmd.append("--no-sweep")
    return json.loads(subprocess.run(cmd, capture_output=True, text=True, timeout=120).stdout)


# ---------------------------------------------------------------------------
# Commands


def parse_keys(text: str | None) -> list | None:
    if not text:
        return None
    return [int(k) if k.strip().lstrip("-").isdigit() else k.strip() for k in text.split(",")]


def slot_settings(a: argparse.Namespace) -> dict:
    out = {}
    for key in ("name", "channel", "key_low", "key_high", "transpose", "gain_db", "pan"):
        v = getattr(a, key, None)
        if v is not None:
            out[key] = v
    for key in ("mute", "solo"):
        v = getattr(a, key, None)
        if v is not None:
            out[key] = v
    return out


def cmd_deploy(a):
    src = pathlib.Path(a.file).expanduser()
    if not src.exists():
        raise Fail(f"no such file: {src}")
    wasm = build(src, ui=resolve_ui(src, a.ui, a.no_ui))
    result = {"wasm_path": str(wasm)}
    if not a.force:
        report = validate(wasm, a.rack, None, 1.0, None, False)
        if not report.get("ok"):
            return {"ok": False, "stage": "validation", "report": report,
                    "hint": "fix the instrument, or pass --force to deploy anyway"}
        if report["audio"]["warnings"]:
            result["sound_warnings"] = report["audio"]["warnings"]
        result["cpu_percent"] = report.get("cpu_percent")
        ui = report.get("ui", {})
        if ui.get("kind", "none") != "none":
            result["ui"] = ui["kind"]
            if ui.get("warnings"):
                result["ui_warnings"] = ui["warnings"]
    rack = pick_rack(a.rack)
    req = slot_settings(a)
    if a.slot:
        req["slot_id"] = a.slot
    if a.params:
        req["params"] = json.loads(a.params)
    # The rack reads the file itself and the DAW project stores its path: keep the file where it is.
    req["path"] = rack_path(rack, wasm)
    info = rpc(rack, "slot.deploy", req)
    keep = ("id", "name", "reloaded", "status", "error", "source", "channel", "key_low", "key_high", "params")
    result.update(ok=True, rack=rack["name"], slot={k: info[k] for k in keep if k in info},
                  instrument=f"{info['instrument']['name']} {info['instrument']['version']}")
    return result


def cmd_ui(a):
    src = pathlib.Path(a.file).expanduser()
    if not src.exists():
        raise Fail(f"no such file: {src}")
    if a.ui_cmd == "list":
        files = ui_files(src.read_bytes())
        kind = "html" if "index.html" in files else "skin" if "ui.json" in files else "none"
        return {"kind": kind, "files": [{"path": k, "bytes": len(v)} for k, v in files.items()]}
    if a.ui_cmd == "extract":
        files = ui_files(src.read_bytes())
        if not files:
            raise Fail(f"{src} has no embedded UI")
        dest = pathlib.Path(a.dir).expanduser()
        for rel, content in files.items():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            (dest / rel).write_bytes(content)
        return {"dir": str(dest), "files": sorted(files)}
    if a.ui_cmd == "pack":
        data = src.read_bytes()
        if a.strip:
            data = strip_ui(data)
        elif a.dir:
            data = pack_ui(data, pathlib.Path(a.dir).expanduser())
        else:
            raise Fail("give a UI folder, or --strip")
        out = pathlib.Path(a.output).expanduser() if a.output else src
        tmp = out.with_name(out.name + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, out)
        return {"wasm_path": str(out), "ui_files": sorted(ui_files(data))}
    # preview
    wasm = build(src, ui=resolve_ui(src, a.ui, a.no_ui))
    report = validate(wasm, a.rack, [60], 0.2, None, False)
    if "meta" not in report:
        return {"ok": False, "stage": "validation", "report": report}
    ui = report.get("ui", {})
    width, height = ui.get("size", [720, 360])
    out_dir = (pathlib.Path(a.output).expanduser() if a.output else
               pathlib.Path(tempfile.gettempdir()) / "aicontact-preview" / slugify(report["meta"]["name"]))
    page = ui_preview(wasm, report["meta"], out_dir, width, height)
    result = {"preview": str(page), "size": [width, height], "ui_errors": ui.get("errors", []),
              "ui_warnings": ui.get("warnings", [])}
    if a.screenshot:
        png = pathlib.Path(a.screenshot).expanduser()
        # The frame page shows the UI at exactly its design size, whatever viewport the browser picks.
        screenshot(page, png, width + 40, height + 140)
        result["screenshot"] = str(png)
    return result


def main(argv: list[str] | None = None) -> int:
    # JSON output is UTF-8 everywhere (Windows pipes default to the ANSI code page).
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog="aicontact", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rack", help="rack name or port (when several are running)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("racks")
    p = sub.add_parser("status"); p.add_argument("--detailed", action="store_true")

    def add_ui_opts(p):
        p.add_argument("--ui", help="custom UI folder (default: <name>.ui/ next to the source, if present)")
        p.add_argument("--no-ui", action="store_true", help="do not embed a UI folder")

    p = sub.add_parser("build"); p.add_argument("file"); p.add_argument("-o", "--output"); add_ui_opts(p)
    p = sub.add_parser("validate"); add_ui_opts(p)
    p.add_argument("file"); p.add_argument("--keys", help="e.g. 48,60,64 or C3,E3 (drums: 36,38,42)")
    p.add_argument("--seconds", type=float, default=2.0); p.add_argument("--wav")
    p.add_argument("--no-sweep", action="store_true")

    def add_slot_opts(p):
        p.add_argument("--name"); p.add_argument("--channel", type=int, help="1-16, 0 = omni")
        p.add_argument("--key-low", dest="key_low", type=int); p.add_argument("--key-high", dest="key_high", type=int)
        p.add_argument("--transpose", type=int); p.add_argument("--gain-db", dest="gain_db", type=float)
        p.add_argument("--pan", type=float)

    p = sub.add_parser("deploy")
    p.add_argument("file"); p.add_argument("--slot", help="slot id; an existing id hot-reloads that slot")
    add_slot_opts(p)
    p.add_argument("--params", help='JSON, e.g. \'{"Cutoff": 800}\'')
    p.add_argument("--force", action="store_true", help="skip validation")
    add_ui_opts(p)

    p = sub.add_parser("note")
    p.add_argument("keys", nargs="+", help="MIDI numbers or names (C4 = 60)")
    p.add_argument("--slot"); p.add_argument("--channel", type=int)
    p.add_argument("--velocity", type=int, default=100); p.add_argument("--ms", type=float, default=600)

    p = sub.add_parser("params"); p.add_argument("slot"); p.add_argument("values", nargs="+", help="Name=Value")
    p = sub.add_parser("update"); p.add_argument("slot"); add_slot_opts(p)
    for flag in ("mute", "solo"):
        p.add_argument(f"--{flag}", dest=flag, action="store_true", default=None)
        p.add_argument(f"--no-{flag}", dest=flag, action="store_false")
    p.add_argument("--index", type=int, help="move to position (0-based)")
    p = sub.add_parser("remove"); p.add_argument("slot")
    sub.add_parser("clear"); sub.add_parser("panic")
    p = sub.add_parser("logs"); p.add_argument("--since", type=int, default=0)
    p = sub.add_parser("export"); p.add_argument("slot"); p.add_argument("-o", "--output")
    p = sub.add_parser("rename"); p.add_argument("name")
    p = sub.add_parser("rpc"); p.add_argument("method"); p.add_argument("params", nargs="?", default="{}")

    p = sub.add_parser("ui", help="custom UI files embedded in a .wasm")
    uisub = p.add_subparsers(dest="ui_cmd", required=True)
    q = uisub.add_parser("list", help="files embedded in a module"); q.add_argument("file")
    q = uisub.add_parser("extract", help="write a module's UI files to a folder (to edit them)")
    q.add_argument("file"); q.add_argument("dir")
    q = uisub.add_parser("pack", help="replace a module's UI with a folder (--strip removes it)")
    q.add_argument("file"); q.add_argument("dir", nargs="?"); q.add_argument("-o", "--output")
    q.add_argument("--strip", action="store_true", help="remove the embedded UI")
    q = uisub.add_parser("preview", help="open an HTML UI in a browser with simulated parameters")
    q.add_argument("file"); add_ui_opts(q); q.add_argument("-o", "--output", help="preview folder")
    q.add_argument("--screenshot", help="PNG path; needs Chrome/Chromium/Edge (headless)")

    a = ap.parse_args(argv)
    try:
        if a.cmd == "racks":
            out = discover()
        elif a.cmd == "status":
            out = rpc(pick_rack(a.rack), "rack.info", {"detailed": a.detailed})
        elif a.cmd == "build":
            src = pathlib.Path(a.file).expanduser()
            ui = resolve_ui(src, a.ui, a.no_ui)
            out = {"wasm_path": str(build(src, pathlib.Path(a.output) if a.output else None, ui))}
            if ui:
                out["ui_dir"] = str(ui)
        elif a.cmd == "ui":
            out = cmd_ui(a)
        elif a.cmd == "validate":
            src = pathlib.Path(a.file).expanduser()
            wasm = build(src, ui=resolve_ui(src, a.ui, a.no_ui))
            out = validate(wasm, a.rack, parse_keys(a.keys), a.seconds, a.wav, not a.no_sweep)
            out["wasm_path"] = str(wasm)
        elif a.cmd == "deploy":
            out = cmd_deploy(a)
        elif a.cmd == "note":
            req = {"keys": [int(k) if k.isdigit() else k for k in a.keys], "velocity": a.velocity, "duration_ms": a.ms}
            if a.slot:
                req["slot_id"] = a.slot
            elif a.channel:
                req["channel"] = a.channel
            out = rpc(pick_rack(a.rack), "slot.note", req)
        elif a.cmd == "params":
            values = {}
            for kv in a.values:
                if "=" not in kv:
                    raise Fail(f"expected Name=Value, got '{kv}'")
                k, v = kv.split("=", 1)
                values[k] = float(v)
            out = rpc(pick_rack(a.rack), "slot.set_params", {"slot_id": a.slot, "params": values})
        elif a.cmd == "update":
            rack = pick_rack(a.rack)
            out = rpc(rack, "slot.update", {"slot_id": a.slot, **slot_settings(a)})
            if a.index is not None:
                rpc(rack, "slot.move", {"slot_id": a.slot, "index": a.index})
                out = rpc(rack, "slot.info", {"slot_id": a.slot})
                out.pop("meta_json", None)
        elif a.cmd == "remove":
            out = rpc(pick_rack(a.rack), "slot.remove", {"slot_id": a.slot})
        elif a.cmd in ("clear", "panic"):
            out = rpc(pick_rack(a.rack), f"rack.{a.cmd}")
        elif a.cmd == "logs":
            out = rpc(pick_rack(a.rack), "rack.logs", {"since": a.since})
        elif a.cmd == "export":
            res = rpc(pick_rack(a.rack), "slot.export", {"slot_id": a.slot})
            path = pathlib.Path(a.output).expanduser() if a.output else pathlib.Path(res["filename"])
            path.write_bytes(base64.b64decode(res["wasm_base64"]))
            out = {"wasm_path": str(path)}
        elif a.cmd == "rename":
            out = rpc(pick_rack(a.rack), "rack.set_name", {"name": a.name})
        elif a.cmd == "rpc":
            out = rpc(pick_rack(a.rack), a.method, json.loads(a.params))
        else:
            raise Fail(f"unknown command {a.cmd}")
    except Fail as e:
        print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False, indent=2))
        return 1
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if not (isinstance(out, dict) and out.get("ok") is False) else 1


if __name__ == "__main__":
    sys.exit(main())
