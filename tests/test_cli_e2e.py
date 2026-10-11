"""End-to-end test: aicontact-host (headless rack) driven by the skill CLI.

    python3 tests/test_cli_e2e.py <path/to/aicontact-host>
"""

import base64
import hashlib
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
CLI = ROOT / "skill/aicontact/scripts/aicontact.py"


def forward(listen_addr: str, target_port: int) -> int:
    """TCP forwarder listen_addr:<free port> -> 127.0.0.1:target_port, standing in for a port proxy
    that exposes a rack on another machine (e.g. `netsh interface portproxy` for WSL)."""
    srv = socket.socket()
    srv.bind((listen_addr, 0))
    srv.listen(8)

    def pipe(src, dst):
        try:
            while data := src.recv(65536):
                dst.sendall(data)
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    def accept_loop():
        while True:
            client, _ = srv.accept()
            upstream = socket.create_connection(("127.0.0.1", target_port))
            threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
            threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()

    threading.Thread(target=accept_loop, daemon=True).start()
    return srv.getsockname()[1]


def free_base_port() -> int:
    """A gateway port with a few free ports after it, away from racks that may already run (47860)."""
    for base in range(49160, 60000, 50):
        try:
            socks = []
            for port in range(base, base + 4):
                s = socket.socket()
                s.bind(("127.0.0.1", port))
                socks.append(s)
            for s in socks:
                s.close()
            return base
        except OSError:
            for s in socks:
                s.close()
    raise RuntimeError("no free ports")


class Host:
    """`aicontact-host serve` with its own token (printed at start-up)."""

    def __init__(self, host_bin, env, home, name, token=None, state=None):
        self.log = open(os.path.join(home, f"host-{name}.log"), "w+", encoding="utf-8", errors="replace")
        cmd = [host_bin, "serve", "--name", name]
        if token:
            cmd += ["--token", token]
        if state:
            cmd += ["--state", str(state)]
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self.token = token
        for _ in range(100):
            self.log.flush()
            self.log.seek(0)
            text = self.log.read()
            if "token: " in text:
                self.token = text.split("token: ", 1)[1].split()[0]
                break
            time.sleep(0.05)
        assert self.token, "host did not start"

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=10)

    def dump(self, label):
        self.log.flush()
        self.log.seek(0)
        print(f"--- {label} (exit code {self.proc.poll()}) ---\n{self.log.read()[-4000:]}", file=sys.stderr)


def main(host_bin: str) -> None:
    home = tempfile.mkdtemp(prefix="aicontact-e2e-")
    base = free_base_port()
    env = {**os.environ, "AICONTACT_HOST_BIN": host_bin, "AICONTACT_PORT": str(base)}
    for key in ("AICONTACT_TOKEN", "AICONTACT_ADDR"):
        env.pop(key, None)
    # Work on copies: builds land next to their sources.
    src = pathlib.Path(home, "src")
    shutil.copytree(ROOT / "examples/c", src)

    def run(*args, expect_ok=True):
        proc = subprocess.run([sys.executable, str(CLI), *map(str, args)], capture_output=True, text=True, encoding="utf-8", env=env)
        out = json.loads(proc.stdout)
        ok = proc.returncode == 0
        assert ok == expect_ok, (args, proc.stdout[-2000:], proc.stderr[-2000:])
        return out

    # validate works without a rack (falls back to aicontact-host)
    r = run("validate", src / "pluck.c", "--wav", os.path.join(home, "pluck.wav"))
    assert r["ok"] and os.path.getsize(os.path.join(home, "pluck.wav")) > 1000, r
    assert (src / "pluck.aci.wasm").exists()  # built next to the source
    assert "AICONTACT_TOKEN" in run("status", expect_ok=False)["error"]  # no token yet -> readable error

    hosts = []
    try:
        a = Host(host_bin, env, home, "E2E Rack")
        hosts.append(a)
        token = a.token
        assert len(token) == 64, token
        env["AICONTACT_TOKEN"] = token
        assert run("racks")[0]["name"] == "E2E Rack"

        # The API requires the token; without it nothing gets in.
        port = run("racks")[0]["port"]
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def status(p, path, headers, body=None):
            req = urllib.request.Request(f"http://127.0.0.1:{p}{path}", data=body, headers=headers)
            try:
                with opener.open(req, timeout=5) as resp:
                    return resp.status
            except urllib.error.HTTPError as e:
                return e.code

        clear = json.dumps({"method": "rack.clear"}).encode()
        for p in (base, port):
            assert status(p, "/health", {}) == 401
            assert status(p, "/rpc", {}, clear) == 401
            assert status(p, "/rpc", {"Authorization": "Bearer " + "0" * 64}, clear) == 401
            assert status(p, "/health", {"Authorization": "Bearer " + token}) == 200
        env["AICONTACT_TOKEN"] = "f" * 64
        assert "rejected the API token" in run("status", expect_ok=False)["error"]
        env["AICONTACT_TOKEN"] = token
        assert run("rename", "曲のスケッチ")["name"] == "曲のスケッチ"  # non-ASCII survives the pipes
        assert run("racks")[0]["name"] == "曲のスケッチ"

        # A second rack registers with the directory; requests reach it through the gateway port.
        b = Host(host_bin, env, home, "Second", token=token)
        hosts.append(b)
        for _ in range(50):
            if len(run("racks")) == 2:
                break
            time.sleep(0.1)
        racks = {r["name"]: r for r in run("racks")}
        assert racks["曲のスケッチ"]["gateway"] and not racks["Second"]["gateway"], racks
        assert run("--rack", "Second", "status")["name"] == "Second"
        assert "several racks" in run("status", expect_ok=False)["error"]
        assert run("--rack", "Second", "deploy", src / "sine.c", "--slot", "lead")["ok"]
        env_other = {**env, "AICONTACT_TOKEN": "e" * 64}
        c = Host(host_bin, env_other, home, "Stranger", token="e" * 64)  # different token: not listed
        hosts.append(c)
        time.sleep(1.5)
        assert {r["name"] for r in run("racks")} == {"曲のスケッチ", "Second"}
        c.stop()

        # Racks on another machine: AICONTACT_ADDR + the forwarded directory port (one port for all racks).
        if sys.platform != "darwin":  # macOS has only 127.0.0.1 on lo0
            fwd_port = forward("127.0.0.2", base)
            assert status(base, "/health", {"Authorization": "Bearer " + token, "Host": "127.0.0.2"}) == 400
            env.update(AICONTACT_ADDR="127.0.0.2", AICONTACT_PORT=str(fwd_port))
            assert {r["name"] for r in run("racks")} == {"曲のスケッチ", "Second"}
            assert run("--rack", "Second", "validate", src / "pluck.c", "--seconds", "0.5", "--no-sweep")["ok"]
            assert run("--rack", "Second", "status")["slots"][0]["id"] == "lead"
            env["AICONTACT_PORT"] = str(base + 30)  # nothing forwarded there
            err = run("racks", expect_ok=False)["error"]
            assert "no AI Contact reachable at 127.0.0.2" in err and "forwarded" in err, err
            del env["AICONTACT_ADDR"]
            env["AICONTACT_PORT"] = str(base)
        b.stop()
        for _ in range(80):  # the directory drops a rack that stopped
            if len(run("racks")) == 1:
                break
            time.sleep(0.1)
        assert [r["name"] for r in run("racks")] == ["曲のスケッチ"]

        r = run("deploy", src / "supersaw.c", "--slot", "pad", "--channel", "1")
        print("deploy:", r["instrument"], "cpu", r.get("cpu_percent"), "%")
        assert r["ok"] and r["slot"]["id"] == "pad" and not r["slot"]["reloaded"]
        assert r["slot"]["source"]["path"] == str((src / "supersaw.aci.wasm").resolve()), r["slot"]
        r = run("deploy", src / "drumkit.c", "--channel", "10")
        assert r["slot"]["channel"] == 10

        # Custom UIs: a <name>.ui/ folder next to the source is embedded as aci.ui/* sections.
        r = run("rpc", "slot.info", '{"slot_id": "pad"}')
        assert r["instrument"]["ui"] == {"kind": "html", "files": 3, "bytes": r["instrument"]["ui"]["bytes"]}, r
        r = run("deploy", src / "pluck.c", "--slot", "keys")
        assert r["ok"] and r["ui"] == "skin" and "ui_warnings" not in r, r
        pad_wasm = pathlib.Path(home, "pad-ui.wasm")
        run("export", "pad", "-o", pad_wasm)
        listed = run("ui", "list", pad_wasm)
        assert listed["kind"] == "html" and {f["path"] for f in listed["files"]} == {"index.html", "knob.js", "style.css"}
        extracted = pathlib.Path(home, "pad-ui")
        run("ui", "extract", pad_wasm, extracted)
        assert (extracted / "index.html").read_bytes() == (ROOT / "examples/c/supersaw.ui/index.html").read_bytes()
        run("ui", "pack", pad_wasm, "--strip", "-o", pathlib.Path(home, "pad-noui.wasm"))
        assert run("ui", "list", pathlib.Path(home, "pad-noui.wasm"))["kind"] == "none"
        r = run("validate", pathlib.Path(home, "pad-noui.wasm"), "--no-sweep", "--seconds", "0.3")
        assert r["ok"] and r["ui"] == {"kind": "none"}, r  # stripping leaves a valid module
        r = run("ui", "preview", src / "supersaw.c", "-o", pathlib.Path(home, "preview"))
        assert r["size"] == [720, 300] and "__ACI_PREVIEW__" in pathlib.Path(home, "preview/index.html").read_text()
        # A broken skin blocks a normal deploy with readable errors; --force deploys (default UI + log).
        broken = pathlib.Path(home, "broken.ui")
        broken.mkdir()
        (broken / "ui.json").write_text(json.dumps({"size": [300, 100], "elements": [
            {"type": "knob", "param": "Nope", "x": 0, "y": 0, "w": 40, "h": 40}]}))
        r = run("deploy", src / "pluck.c", "--slot", "keys", "--ui", broken, expect_ok=False)
        assert r["stage"] == "validation" and any("Nope" in e for e in r["report"]["ui"]["errors"]), r
        assert run("deploy", src / "pluck.c", "--slot", "keys", "--ui", broken, "--force")["ok"]
        assert any("custom UI" in l["message"] and l["slot"] == "keys" for l in run("logs")["lines"])
        (broken / "ui.json").unlink()
        assert "needs an index.html" in run("build", src / "pluck.c", "--ui", broken, expect_ok=False)["error"]
        assert run("remove", "keys")["removed"] == "keys"

        r = run("params", "pad", "cutoff=900", "3=0.8")
        vals = {p["name"]: p["value"] for p in r["params"]}
        assert vals["Cutoff"] == 900 and vals["Detune"] == 0.8, vals

        assert run("note", "C4", "E4", "G4", "--slot", "pad", "--ms", "1500")["keys"] == [60, 64, 67]
        time.sleep(0.4)
        pad = next(s for s in run("status")["slots"] if s["id"] == "pad")
        assert pad["peak_db"] > -40, pad

        # hot reload a new version: params carry over, held notes keep sounding
        src2 = pathlib.Path(home, "supersaw2.c")
        src2.write_text((src / "supersaw.c").read_text(encoding="utf-8").replace('\\"version\\":\\"1.0.0\\"', '\\"version\\":\\"1.1.0\\"'), encoding="utf-8")
        r = run("deploy", src2, "--slot", "pad")
        assert r["slot"]["reloaded"] and r["instrument"].endswith("1.1.0"), r
        assert {p["name"]: p["value"] for p in r["slot"]["params"]}["Cutoff"] == 900
        time.sleep(0.3)
        pad = next(s for s in run("status")["slots"] if s["id"] == "pad")
        assert pad["peak_db"] > -40, ("held notes should be re-struck after reload", pad)

        bad = pathlib.Path(home, "bad.c")
        bad.write_text('#include "aci.h"\nint aci_init(float a, int b) { return 0; }\n', encoding="utf-8")
        r = run("deploy", bad, expect_ok=False)
        assert r["stage"] == "validation" and "missing required export" in r["report"]["error"], r
        bad.write_text('#include "aci.h"\nthis is not C;\n', encoding="utf-8")
        assert "compile failed" in run("deploy", bad, expect_ok=False)["error"]

        # Slots always refer to a file; `path` only names regular .wasm files (no devices, FIFOs, others).
        wasm_b64 = base64.b64encode((src / "sine.aci.wasm").read_bytes()).decode()
        r = run("rpc", "slot.deploy", json.dumps({"wasm_base64": wasm_b64}), expect_ok=False)
        assert "needs 'path'" in r["error"], r
        notes = pathlib.Path(home, "notes.txt")
        notes.write_text("not an instrument", encoding="utf-8")
        assert ".wasm" in run("rpc", "slot.deploy", json.dumps({"path": str(notes)}), expect_ok=False)["error"]
        pathlib.Path(home, "dir.wasm").mkdir()
        r = run("rpc", "instrument.validate", json.dumps({"path": str(pathlib.Path(home, "dir.wasm"))}), expect_ok=False)
        assert "no such regular file" in r["error"], r

        r = run("update", "drumkit", "--mute", "--index", "0")
        assert r["mute"] and r["index"] == 0, r
        r = run("export", "pad", "-o", os.path.join(home, "pad.wasm"))
        assert pathlib.Path(r["wasm_path"]).stat().st_size > 1000
        for line in run("logs")["lines"]:
            print("  log:", line["level"], line["slot"], line["message"])
        assert run("remove", "pad")["removed"] == "pad"

        # The gateway's rack closes (its project was closed): another rack takes the directory port over.
        backup = Host(host_bin, env, home, "Backup", token=token)
        hosts.append(backup)
        for _ in range(50):
            if len(run("racks")) == 2:
                break
            time.sleep(0.1)
        a.stop()
        for _ in range(100):
            try:
                if [(r["name"], r["gateway"]) for r in run("racks")] == [("Backup", True)]:
                    break
            except AssertionError:
                pass  # nobody holds the port for a moment
            time.sleep(0.1)
        assert run("status")["name"] == "Backup"
        backup.stop()

        # Project state: paths + settings, files read again on load; a missing file is an error, not silence
        # with a stale copy. Deploying a file to the slot fixes it and restores the saved params.
        state = pathlib.Path(home, "project.json")
        lead = pathlib.Path(home, "lead.aci.wasm")
        shutil.copyfile(src / "supersaw.aci.wasm", lead)
        p1 = Host(host_bin, env, home, "Project", token=token, state=state)
        hosts.append(p1)
        run("deploy", lead, "--slot", "lead", "--channel", "3", "--params", '{"Cutoff": 700}')
        for _ in range(50):  # saved on change (a killed process on Windows does not get to save at exit)
            if state.exists() and '"lead"' in state.read_text(encoding="utf-8"):
                break
            time.sleep(0.1)
        p1.stop()
        saved = json.loads(state.read_text(encoding="utf-8"))
        slot = saved["slots"][0]
        assert slot["path"] == str(lead.resolve()) and "wasm" not in slot and slot["params"]["0"] == 700, slot
        assert slot["sha256"] == hashlib.sha256(lead.read_bytes()).hexdigest(), slot

        # A file replaced behind the project's back is refused before it runs; redeploying accepts it.
        good = lead.read_bytes()
        shutil.copyfile(src / "sine.aci.wasm", lead)
        p_changed = Host(host_bin, env, home, "Project", token=token, state=state)
        hosts.append(p_changed)
        s = run("status")["slots"][0]
        assert s["status"] == "error" and "changed since it was deployed" in s["error"], s
        p_changed.stop()
        lead.write_bytes(good)  # put the original back: loads again
        p_restored = Host(host_bin, env, home, "Project", token=token, state=state)
        hosts.append(p_restored)
        assert run("status")["slots"][0]["status"] == "ok"
        p_restored.stop()
        unhashed = json.loads(state.read_text(encoding="utf-8"))
        del unhashed["slots"][0]["sha256"]
        pathlib.Path(home, "unhashed.json").write_text(json.dumps(unhashed), encoding="utf-8")
        p_unhashed = Host(host_bin, env, home, "Project", token=token, state=pathlib.Path(home, "unhashed.json"))
        hosts.append(p_unhashed)
        s = run("status")["slots"][0]
        assert s["status"] == "error" and "no SHA-256" in s["error"], s
        p_unhashed.stop()

        lead.rename(pathlib.Path(home, "moved.aci.wasm"))
        p2 = Host(host_bin, env, home, "Project", token=token, state=state)
        hosts.append(p2)
        s = run("status")["slots"][0]
        assert s["status"] == "error" and "lead.aci.wasm" in s["error"] and s["channel"] == 3, s
        r = run("deploy", pathlib.Path(home, "moved.aci.wasm"), "--slot", "lead")
        assert r["slot"]["status"] == "ok", r
        assert {p["name"]: p["value"] for p in r["slot"]["params"]}["Cutoff"] == 700
        for _ in range(50):
            if "moved.aci.wasm" in state.read_text(encoding="utf-8"):
                break
            time.sleep(0.1)
        p2.stop()
        assert json.loads(state.read_text(encoding="utf-8"))["slots"][0]["path"].endswith("moved.aci.wasm")
        print("E2E OK")
    except BaseException:
        for h in hosts:
            h.dump(" ".join(h.proc.args[2:5]))
        raise
    finally:
        for h in hosts:
            h.stop()


if __name__ == "__main__":
    main(sys.argv[1])
