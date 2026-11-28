"""Regression runner for hancho-cli, the MCP server, and the core.

See doc/regression-test-cli-mcp-core-plan.md. Python 3, standard library only (Windows).

Commands:
  run.cmd run    [--bin-dir DIR] [--filter TEXT]    compare every layer with the goldens
  run.cmd check  SCENARIO_ID [--bin-dir DIR]        same, for one scenario, with diffs
  run.cmd record [--bin-dir DIR] [--filter TEXT]    overwrite goldens (explicit only)

Every layer runs on its own headless core with a fresh data directory. The core is started
by the runner (never auto-started by a client), the scenario's operations run on that layer,
the core is stopped, and the normalized observations are compared with the goldens.
"""

import argparse
import ctypes
import difflib
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes as wt
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DUMMY_CMD = HERE / "dummy_llm.cmd"
SCENARIO_DIR = HERE / "scenarios"
GOLDEN_DIR = HERE / "golden"
OP_MAP = json.loads((HERE / "op-map.json").read_text(encoding="utf-8"))
ACTIVE_CORES = Path(os.environ.get("LOCALAPPDATA", "")) / "Hancho" / "active-cores"
DEFAULT_BIN = REPO / "build" / "Release"

LAYERS = ["core", "cli", "mcp-claude", "mcp-opencode"]
TERMINAL_STATES = {"succeeded", "failed", "cancelled", "superseded"}


class RunnerError(Exception):
    """The runner could not complete a step. This is not a golden mismatch."""


# ---------------------------------------------------------------------------
# Named pipe client (L1). Win32 calls through ctypes, one message per connection.
# ---------------------------------------------------------------------------

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wt.HANDLE
_k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
_k32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
_k32.ReadFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
_k32.CloseHandle.argtypes = [wt.HANDLE]
_INVALID_HANDLE = wt.HANDLE(-1).value
_ERROR_MORE_DATA = 234
_ERROR_BROKEN_PIPE = 109
_ERROR_PIPE_NOT_CONNECTED = 233


def pipe_request(pipe, request):
    payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
    handle = _k32.CreateFileW(pipe, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
    if handle == _INVALID_HANDLE:
        raise RunnerError("cannot connect to %s (error %d)" % (pipe, ctypes.get_last_error()))
    try:
        written = wt.DWORD(0)
        if not _k32.WriteFile(handle, payload, len(payload), ctypes.byref(written), None):
            raise RunnerError("WriteFile to core failed (error %d)" % ctypes.get_last_error())
        out = bytearray()
        buf = ctypes.create_string_buffer(65536)
        while True:
            got = wt.DWORD(0)
            ok = _k32.ReadFile(handle, buf, 65536, ctypes.byref(got), None)
            err = ctypes.get_last_error()
            if got.value:
                out += buf.raw[:got.value]
            if ok:
                break
            if err == _ERROR_MORE_DATA:
                continue
            if err in (_ERROR_BROKEN_PIPE, _ERROR_PIPE_NOT_CONNECTED):
                break
            raise RunnerError("ReadFile from core failed (error %d)" % err)
        return json.loads(bytes(out).decode("utf-8"))
    finally:
        _k32.CloseHandle(handle)


# ---------------------------------------------------------------------------
# Core process owned by the runner
# ---------------------------------------------------------------------------

def base_env(demo_now):
    env = dict(os.environ)
    env["HANCHO_AUTOSTART"] = "off"  # clients must never start a second core
    env["HANCHO_DEMO"] = "1"
    env["HANCHO_DEMO_NOW"] = demo_now
    # The core is a child of Hancho.exe and does not receive --dummy-llm; its demo jobs
    # read this variable to find the Python dummy.
    env["HANCHO_DUMMY_LLM"] = str(DUMMY_CMD)
    # origin.session / origin.host_session are read from these; a parent Claude session must not leak
    # its ids into recorded goldens.
    for name in ("PC_ORIGIN_SESSION", "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_HOST_SESSION_ID"):
        env.pop(name, None)
    return env


def process_alive(pid):
    out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"],
                         capture_output=True, timeout=60).stdout.decode("mbcs", "replace")
    return str(pid) in out


class Core:
    """A headless core on one data directory."""

    def __init__(self, bin_dir, data_dir, demo_now):
        self.exe = bin_dir / "Hancho.exe"
        self.data_dir = data_dir
        self.env = base_env(demo_now)
        # Hancho.exe starts the real core as a detached process and returns at once. The core is
        # identified by its active-core record (dataDir), not by this process.
        subprocess.Popen(
            [str(self.exe), "--headless", "--data-dir", str(data_dir),
             "--demo", "--demo-now", demo_now, "--dummy-llm", str(DUMMY_CMD)],
            env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.pipe, self.pid = self._wait_for_record()

    def _wait_for_record(self, timeout=60):
        want = os.path.normcase(str(self.data_dir))
        deadline = time.time() + timeout
        while time.time() < deadline:
            for f in glob.glob(str(ACTIVE_CORES / "core-*.json")):
                try:
                    rec = json.loads(Path(f).read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    continue
                if os.path.normcase(rec.get("dataDir", "")) == want:
                    return rec["pipe"], int(rec["pid"])
            time.sleep(0.2)
        raise RunnerError("core did not register on %s within %d s" % (self.data_dir, timeout))

    def stop(self):
        subprocess.run([str(self.exe), "quit", "--data-dir", str(self.data_dir)],
                       capture_output=True, timeout=60, env=self.env)
        deadline = time.time() + 30
        while time.time() < deadline:
            if not process_alive(self.pid):
                return
            time.sleep(0.2)
        subprocess.run(["taskkill", "/PID", str(self.pid), "/T", "/F"], capture_output=True, timeout=60)
        raise RunnerError("core pid %d did not stop after quit; killed" % self.pid)


# ---------------------------------------------------------------------------
# Placeholders and normalization
# ---------------------------------------------------------------------------

_PH = re.compile(r"\{(\w+)\}")


def substitute(value, variables):
    if isinstance(value, str):
        whole = _PH.fullmatch(value)
        if whole and whole.group(1) in variables:
            return json.loads(json.dumps(variables[whole.group(1)]))
        return _PH.sub(lambda m: str(variables[m.group(1)]) if m.group(1) in variables else m.group(0), value)
    if isinstance(value, dict):
        return {k: substitute(v, variables) for k, v in value.items()}
    if isinstance(value, list):
        return [substitute(v, variables) for v in value]
    return value


def path_replacements(ctx):
    pairs = [
        (str(ctx["data"]), "<data>"),
        (str(ctx["cwd"]), "<cwd>"),
        (str(ctx["tmp"]), "<tmp>"),
        (str(DUMMY_CMD), "<dummy>"),
        (str(ctx["bin"]), "<bin>"),
        (str(REPO), "<repo>"),
    ]
    out = []
    for path, token in pairs:
        for variant in {path, path.replace("\\", "/"), path.replace("/", "\\")}:
            out.append((variant, token))
    out.sort(key=lambda p: len(p[0]), reverse=True)
    return out


VOLATILE_TEXT = [
    # Per-run temp directory names, also inside text that carries escaped paths.
    (re.compile(r"hancho-regress-[A-Za-z0-9._-]+?-(?:core|cli|mcp-claude|mcp-opencode)-[a-z0-9_]+"),
     "hancho-regress-<run>"),
    (re.compile(r"(PID\s*[:=]\s*)\d+"), r"\1<pid>"),
    (re.compile(r"(#)\d+(\s*\[)"), r"\1<n>\2"),
    (re.compile(r"\(\d+(\.\d+)?s\)"), "(<t>s)"),
    (re.compile(r"(Uptime\s*:\s*)\S+"), r"\1<t>"),
]


def normalize(value, replacements, drop_keys=()):
    """Replace paths with tokens and remove keys whose values are not part of the contract.

    drop_keys holds revision counters (for example "rev") that count every change in the core,
    including background demo jobs, so their values depend on timing.
    """
    if isinstance(value, str):
        for old, new in replacements:
            value = value.replace(old, new)
        # Process ids and durations inside log excerpts change on every run.
        for pattern, new in VOLATILE_TEXT:
            value = pattern.sub(new, value)
        return value
    if isinstance(value, dict):
        return {k: normalize(v, replacements, drop_keys) for k, v in value.items() if k not in drop_keys}
    if isinstance(value, list):
        return [normalize(v, replacements, drop_keys) for v in value]
    return value


def mcp_result_payload(result):
    """MCP tool results carry JSON as text. Parse it so the golden holds the structure."""
    out = dict(result)
    content = out.get("content")
    if isinstance(content, list):
        new_content = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                try:
                    new_content.append({"json": json.loads(item["text"])})
                    continue
                except (ValueError, TypeError):
                    pass
            new_content.append(item)
        out["content"] = new_content
    return out


def to_golden_text(obj):
    return json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def op_spec(op_name):
    if op_name not in OP_MAP:
        raise RunnerError("operation not in op-map.json: %s" % op_name)
    return OP_MAP[op_name]


def check_ok(inner, where):
    if not (isinstance(inner, dict) and inner.get("ok") is True):
        raise RunnerError("%s: expected ok, got %s" % (where, json.dumps(inner, ensure_ascii=False)[:400]))


# ---------------------------------------------------------------------------
# Layers. Each returns (observation, inner) where observation is stored in the golden and
# inner is the parsed payload used for checks.
# ---------------------------------------------------------------------------

class CoreLayer:
    def __init__(self, core):
        self.core = core

    def invoke(self, op, variables):
        spec = op_spec(op)["core"]
        request = {"action": spec["action"]}
        request.update(substitute(spec["args"], variables))
        resp = pipe_request(self.core.pipe, request)
        return resp, resp

    def close(self):
        pass


class CliLayer:
    def __init__(self, core, ctx):
        self.core = core
        self.ctx = ctx
        self.cli = ctx["bin"] / "hancho-cli.exe"

    def invoke(self, op, variables):
        spec = op_spec(op)["cli"]
        argv = substitute(spec["argv"], variables)
        if op == "taskqueue.add":
            jobs_file = self.ctx["tmp"] / "jobs.json"
            jobs_file.write_text(json.dumps({"jobs": variables["jobs"], "triage": variables["triage"]},
                                            ensure_ascii=False), encoding="utf-8")
            argv = [str(jobs_file) if a == "{jobs_file}" else a for a in argv]
        cmd = [str(self.cli), "--json", "--data-dir", str(self.ctx["data"])] + [str(a) for a in argv]
        proc = subprocess.run(cmd, capture_output=True, timeout=300, env=self.core.env,
                              cwd=str(self.ctx["cwd"]))
        text = proc.stdout.decode("utf-8", "replace").strip()
        try:
            resp = json.loads(text)
        except ValueError:
            raise RunnerError("%s: stdout is not JSON (exit %s): %s" % (op, proc.returncode, text[:300]))
        return resp, resp

    def close(self):
        pass


class McpLayer:
    """An MCP session: the same entry claude or opencode would write, started over stdio and kept open."""

    def __init__(self, core, ctx, client):
        self.core = core
        self.ctx = ctx
        command = self._install(client)
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, cwd=str(ctx["cwd"]), env=core.env)
        self.next_id = 1
        self._request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                     "clientInfo": {"name": "hancho-regression", "version": "0"}})
        self._notify("notifications/initialized")
        self.tools_list = self._request("tools/list", {})

    def _install(self, client):
        cli = self.ctx["bin"] / "hancho-cli.exe"
        proc = subprocess.run([str(cli), "mcp", "install", "--data-dir", str(self.ctx["data"]),
                               "--client", client],
                              capture_output=True, timeout=120, cwd=str(self.ctx["cwd"]),
                              env=self.core.env)
        if proc.returncode != 0:
            raise RunnerError("mcp install failed: " + proc.stderr.decode("utf-8", "replace")[:300])
        if client == "claude":
            cfg = json.loads((self.ctx["cwd"] / ".mcp.json").read_text(encoding="utf-8"))
            entry = cfg["mcpServers"]["hancho"]
            return [entry["command"]] + list(entry["args"])
        cfg = json.loads((self.ctx["cwd"] / "opencode.json").read_text(encoding="utf-8"))
        return list(cfg["mcp"]["hancho"]["command"])

    def _send(self, obj):
        self.proc.stdin.write((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def _notify(self, method):
        self._send({"jsonrpc": "2.0", "method": method})

    def _request(self, method, params):
        req_id = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RunnerError("MCP server closed stdout while waiting for %s" % method)
            obj = json.loads(line.decode("utf-8"))
            if obj.get("id") != req_id:
                continue
            if "error" in obj:
                raise RunnerError("MCP %s failed: %s" % (method, json.dumps(obj["error"])[:300]))
            return obj["result"]

    def invoke(self, op, variables):
        spec = op_spec(op)["mcp"]
        args = substitute(spec["args"], variables)
        result = self._request("tools/call", {"name": spec["tool"], "arguments": args})
        if result.get("isError"):
            raise RunnerError("%s: tool returned isError" % op)
        payload = mcp_result_payload(result)
        content = payload.get("content") or [{}]
        inner = content[0].get("json", {}) if isinstance(content[0], dict) else {}
        return payload, inner

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=60)
        except (subprocess.TimeoutExpired, OSError):
            self.proc.kill()
            raise RunnerError("MCP server did not exit after stdin closed; killed")


# ---------------------------------------------------------------------------
# Scenario execution
# ---------------------------------------------------------------------------

def wait_until_done(layer, variables, budget_s):
    """Call taskqueue.wait until every job in the queue is in a terminal state.

    taskqueue_wait returns when the queue version changes, so the loop is event-driven: each
    call passes the version from the previous reply.
    """
    since = 0
    deadline = time.time() + budget_s
    while True:
        extra = dict(variables, since_version=since, timeout_ms=5000)
        obs, inner = layer.invoke("taskqueue.wait", extra)
        check_ok(inner, "taskqueue.wait")
        since = inner.get("version", since)
        jobs = inner.get("jobs", [])
        if jobs and all(j.get("state") in TERMINAL_STATES for j in jobs):
            return obs
        if time.time() > deadline:
            raise RunnerError("queue %s did not reach a terminal state within %d s"
                              % (variables.get("queue"), budget_s))


def wait_until_advice(layer, variables, budget_s):
    """Poll triage_list until every card has a finished advice run (done or failed).

    Triage advice runs are plain processes, not queue jobs, so there is no version to wait on.
    """
    deadline = time.time() + budget_s
    while True:
        obs, inner = layer.invoke("triage.list", variables)
        check_ok(inner, "triage.list")
        # Only the cards of the scenario's own job count; other queues may have their own cards.
        cards = [c for c in inner.get("cards", [])
                 if card_matches(c, {"target.failedJobIds": variables["id"]})]
        if cards and all(c.get("adviceState") in ("done", "failed") for c in cards):
            return obs, inner
        if time.time() > deadline:
            raise RunnerError("triage advice did not finish within %d s" % budget_s)
        time.sleep(0.5)


def value_at(payload, path):
    """Read a value by a dotted path such as cards.0.id (list indexes are numbers)."""
    node = payload
    for part in path.split("."):
        node = node[int(part)] if isinstance(node, list) else node[part]
    return node


def card_value(item, key):
    """A dotted path into the card, a top-level field, or the "v" of an info.background entry."""
    if "." in key:
        try:
            return value_at(item, key)
        except (KeyError, IndexError, TypeError, ValueError):
            return None
    if key in item:
        return item[key]
    for entry in (item.get("info") or {}).get("background", []):
        if entry.get("k") == key:
            return entry.get("v")
    return None


def card_matches(item, where):
    """True when every key of where matches. A list value matches when it contains the wanted id."""
    for key, want in where.items():
        got = card_value(item, key)
        if isinstance(got, list):
            if str(want) not in [str(x) for x in got]:
                return False
        elif str(got) != str(want):
            return False
    return True


def capture_matching(payload, spec, variables):
    """Pick the first item of a list in payload whose keys equal the given values.

    spec: {"list": "cards", "where": {"jobName": "{id}"}, "field": "id"}. Values in "where" are
    formatted with the scenario variables, so a card is selected by its own job, not by position.
    """
    items = value_at(payload, spec["list"])
    want = {k: str(v).format(**variables) for k, v in spec["where"].items()}
    for item in items:
        if card_matches(item, want):
            return value_at(item, spec["field"])
    raise RunnerError("capture: no item in %s matches %s" % (spec["list"], want))


def execute(layer, scenario, variables):
    observations = []
    if isinstance(layer, McpLayer):
        observations.append(("00-tools-list", mcp_result_payload(layer.tools_list)))
    budget = int(scenario.get("setup", {}).get("wait_budget_s", 120))
    for index, step in enumerate(scenario["operations"], start=1):
        op = step["op"]
        name = "%02d-%s" % (index, op)
        if op == "taskqueue.wait":
            obs = wait_until_done(layer, variables, budget)
        elif step.get("until") == "advice":
            obs, inner = wait_until_advice(layer, variables, budget)
        else:
            obs, inner = layer.invoke(op, variables)
            if step.get("expect", "ok") == "ok":
                check_ok(inner, op)
        for var, spec in step.get("capture", {}).items():
            if isinstance(spec, str):
                variables[var] = value_at(inner, spec)
            else:
                variables[var] = capture_matching(inner, spec, variables)
        if step.get("keep", True):
            observations.append((name, obs))
    return observations


def load_scenarios(filter_text=None, exact=None):
    found = []
    for path in sorted(SCENARIO_DIR.rglob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        sid = scenario["id"]
        if exact is not None and sid != exact:
            continue
        if filter_text and filter_text not in sid:
            continue
        found.append(scenario)
    return found


def run_scenario_layer(scenario, layer_name, bin_dir):
    tmp = Path(tempfile.mkdtemp(prefix="hancho-regress-%s-%s-" % (scenario["id"], layer_name)))
    data = tmp / "data"
    cwd = tmp / "cwd"
    data.mkdir()
    cwd.mkdir()
    shutil.copy(REPO / "tests" / "data" / "demo" / "demo.json", data / "demo.json")
    now = scenario["setup"]["now"]
    ctx = {"tmp": tmp, "data": data, "cwd": cwd, "bin": bin_dir, "now": now}
    base = {"dummy": str(DUMMY_CMD), "data": str(data), "cwd": str(cwd)}
    variables = substitute(scenario["vars"], base)
    variables.update(base)
    for file_name, content in scenario.get("setup", {}).get("files", {}).items():
        # Written before the core starts, so the core reads these as its own config.
        (data / file_name).write_text(json.dumps(substitute(content, base), indent=2, ensure_ascii=False) + "\n",
                                      encoding="utf-8", newline="\n")
    replacements = path_replacements(ctx)
    ok = False
    core = Core(bin_dir, data, now)
    try:
        if layer_name == "core":
            layer = CoreLayer(core)
        elif layer_name == "cli":
            layer = CliLayer(core, ctx)
        elif layer_name.startswith("mcp-"):
            layer = McpLayer(core, ctx, layer_name.split("-", 1)[1])
        else:
            raise RunnerError("unknown layer %s" % layer_name)
        try:
            observations = execute(layer, scenario, variables)
        finally:
            layer.close()
        drop = tuple(scenario.get("drop_keys", []))
        result = [(name, normalize(obj, replacements, drop)) for name, obj in observations]
        ok = True
        return result
    finally:
        core.stop()
        if ok:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("   kept for inspection: %s" % tmp)


def golden_path(scenario, layer, name):
    return GOLDEN_DIR / layer / scenario["id"] / (name + ".json")


def cmd_run(args, record):
    bin_dir = Path(args.bin_dir).resolve()
    if not (bin_dir / "Hancho.exe").exists() or not (bin_dir / "hancho-cli.exe").exists():
        print("binaries not found in %s" % bin_dir)
        return 2
    scenarios = load_scenarios(args.filter)
    if not scenarios:
        print("no scenarios matched")
        return 2
    failures = 0
    errors = 0
    for scenario in scenarios:
        for layer in scenario.get("layers", LAYERS):
            label = "%s [%s]" % (scenario["id"], layer)
            try:
                observations = run_scenario_layer(scenario, layer, bin_dir)
            except RunnerError as exc:
                errors += 1
                print("ERROR  %s: %s" % (label, exc))
                continue
            for name, obj in observations:
                text = to_golden_text(obj)
                path = golden_path(scenario, layer, name)
                if record:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(text.encode("utf-8"))
                    print("RECORD %s %s" % (label, name))
                    continue
                if not path.exists():
                    failures += 1
                    print("FAIL   %s %s: no golden (run record)" % (label, name))
                    continue
                golden = path.read_text(encoding="utf-8")
                if golden != text:
                    failures += 1
                    print("FAIL   %s %s" % (label, name))
                    for line in difflib.unified_diff(golden.splitlines(), text.splitlines(),
                                                     "golden", "actual", lineterm=""):
                        print("       " + line[:300])
                else:
                    print("PASS   %s %s" % (label, name))
    if record:
        print("recorded goldens (errors: %d)" % errors)
        return 2 if errors else 0
    print("summary: failures=%d errors=%d" % (failures, errors))
    if errors:
        return 2
    return 1 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Hancho regression runner (P1).")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("run", "check", "record"):
        p = sub.add_parser(name)
        p.add_argument("--bin-dir", default=str(DEFAULT_BIN))
        if name == "check":
            p.add_argument("scenario", help="scenario id, for example taskqueue.add-list-get")
        else:
            p.add_argument("--filter", default=None)
    args = parser.parse_args(argv)
    if args.cmd == "check":
        if not load_scenarios(exact=args.scenario):
            print("no scenario with id %s" % args.scenario)
            return 2
        args.filter = args.scenario
    return cmd_run(args, record=(args.cmd == "record"))


if __name__ == "__main__":
    sys.exit(main())
