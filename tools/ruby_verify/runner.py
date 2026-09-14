"""Isolated builds, process supervision, and reproducible source snapshots."""
import concurrent.futures
import fcntl
import fnmatch
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import uuid

from .catalog import build as catalog, canonical, digest
from .store import now

LIMIT = 1024 * 1024
EXCERPT = 16000


def source_files(root):
    paths = []
    for name in ("src", "tests", "tools", "examples", "bindings/ruby"):
        directory = root / name
        if directory.exists():
            # Prune retained run artifacts/worktrees instead of visiting every log.
            for directory_path, directories, filenames in os.walk(directory):
                directories[:] = [d for d in directories if d not in
                                  {".verification", ".git", "__pycache__", "build"} and not d.endswith(".dSYM")]
                for filename in filenames:
                    path = Path(directory_path) / filename
                    relative = path.relative_to(root)
                    if path.suffix in {".o", ".so", ".bundle", ".dylib", ".pyc", ".sqlite3", ".sqlite3-wal", ".sqlite3-shm"}:
                        continue
                    if path.name in {"mkmf.log", "Makefile"}:
                        continue
                    if path.is_file():
                        if not path.resolve().is_relative_to(root.resolve()):
                            raise RuntimeError("Source symlink escapes workspace: " + str(relative))
                        paths.append(path)
    paths += [root / name for name in ("Makefile", "README.md", ".gitignore") if (root / name).is_file()]
    return sorted(paths)


def capture(root):
    files = {str(p.relative_to(root)): p.read_bytes() for p in source_files(root)}
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    return files, manifest, digest(manifest)


def resolve_mise_ruby(requested):
    """Resolve the active Ruby through mise; never fall back to system Ruby."""
    mise = shutil.which("mise")
    if not mise:
        raise RuntimeError("mise is required to select Ruby")
    if requested and requested != "ruby":
        resolved = shutil.which(requested)
        if resolved:
            resolved = Path(resolved).resolve()
            mise_root = Path(os.environ.get("MISE_DATA_DIR", Path.home() / ".local/share/mise"))
            ruby_installs = (mise_root / "installs" / "ruby").resolve()
            try:
                resolved.relative_to(ruby_installs)
            except ValueError:
                raise RuntimeError("Ruby must come from mise: " + str(resolved))
            return str(resolved)
    selected = subprocess.run([mise, "which", "ruby"], capture_output=True, text=True,
                              timeout=10, env=child_env())
    if selected.returncode:
        detail = (selected.stderr or selected.stdout).strip()
        raise RuntimeError("mise could not select Ruby" + (": " + detail if detail else ""))
    active = selected.stdout.strip().splitlines()[-1] if selected.stdout.strip() else ""
    if not active or not Path(active).is_file():
        raise RuntimeError("mise returned no usable Ruby executable")
    return str(Path(active).resolve())


def environment(ruby):
    resolved = resolve_mise_ruby(ruby)
    def version(argv):
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=10, env=child_env())
        if completed.returncode:
            raise RuntimeError("Cannot query tool version: " + repr(argv))
        return (completed.stdout or completed.stderr).strip()
    cc = shutil.which("clang")
    if not cc:
        raise RuntimeError("clang is required for the C oracle and build")
    info = {"ruby": str(Path(resolved).resolve()), "ruby_version": version([resolved, "--version"]),
            "clang": str(Path(cc).resolve()), "clang_version": version([cc, "--version"]),
            "platform": os.uname().sysname + " " + os.uname().machine,
            "python": sys.version, "make_version": version(["make", "--version"]),
            "ruby_gems": version([resolved, "-rjson", "-rrubygems", "-e",
                'puts JSON.generate(Gem::Specification.map { |s| [s.name,s.version.to_s,s.full_gem_path] }.sort)']),
            "build_environment": {k: os.environ.get(k) for k in
                ("PATH", "CC", "CFLAGS", "CPPFLAGS", "LDFLAGS", "ARCHFLAGS", "SDKROOT", "GEM_HOME", "GEM_PATH")}}
    return info, digest(info)


def child_env():
    env = os.environ.copy()
    for key in ("RUBYOPT", "RUBYLIB", "BUNDLE_GEMFILE", "PYTHONPATH", "MAKEFLAGS", "MFLAGS"):
        env.pop(key, None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def process(argv, cwd, prefix, timeout=30, stop=None):
    """Never accept exit zero alone. Caller must also validate the check protocol."""
    started = time.monotonic()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    outpath, errpath = Path(str(prefix) + ".stdout"), Path(str(prefix) + ".stderr")
    reason = None
    with outpath.open("wb") as stdout, errpath.open("wb") as stderr:
        try:
            child = subprocess.Popen(argv, cwd=cwd, stdout=stdout, stderr=stderr, env=child_env(), start_new_session=True)
        except OSError as exc:
            return {"exit_code": None, "duration_ms": 0, "stdout": "", "stderr": str(exc), "reason": "launch_error", "command": argv}
        try:
            while child.poll() is None:
                if stop and stop.is_set():
                    reason = "interrupted"
                elif time.monotonic() - started > timeout:
                    reason = "timeout"
                elif outpath.stat().st_size + errpath.stat().st_size > LIMIT:
                    reason = "output_limit"
                if reason:
                    break
                time.sleep(0.02)
        finally:
            # Clean up descendants even if the immediate parent already exited.
            # Also runs on Ctrl-C instead of leaving a compiler or test behind.
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
    if outpath.stat().st_size + errpath.stat().st_size > LIMIT:
        reason = reason or "output_limit"
    def excerpt(path):
        with path.open("rb") as stream:
            data = stream.read(EXCERPT)
        return data.decode("utf-8", errors="replace") + ("\n[truncated; see artifact log]" if path.stat().st_size > EXCERPT else "")
    return dict(exit_code=child.returncode, duration_ms=int((time.monotonic() - started) * 1000),
                stdout=excerpt(outpath), stderr=excerpt(errpath), reason=reason, command=argv)


def select(checks, brief=None, patterns=None):
    owned = [c for c in checks if (not brief or c["brief"] == brief) and
             (not patterns or any(fnmatch.fnmatchcase(c["id"], pattern) for pattern in patterns))]
    if not owned:
        raise ValueError("Selection matched no checks")
    for pattern in patterns or []:
        if not any(fnmatch.fnmatchcase(c["id"], pattern) for c in owned):
            raise ValueError("Check selector matched no checks: " + pattern)
    ids = {c["id"] for c in owned}
    ids.update(c["id"] for c in checks if c.get("shared"))
    return [c for c in checks if c["id"] in ids]


def evidence_status(row, check, source_hash, catalog_hash, environment_hash):
    if row is None:
        return "unverified"
    if (row["revision"] != check["revision"] or row["source_hash"] != source_hash or
        row["catalog_hash"] != catalog_hash or row["environment_hash"] != environment_hash):
        return "stale"
    if row["run_status"] != "completed" or row["status"] in ("pending", "running"):
        return "incomplete"
    return row["status"]


def recover(store):
    locks = store.path.parent / "locks"
    locks.mkdir(exist_ok=True)
    for row in list(store.db.execute("SELECT run_id FROM runs WHERE status='running'")):
        with (locks / (row[0] + ".lock")).open("a+") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                continue
            store.finish(row[0], "interrupted", 130)


class Harness:
    def __init__(self, root, store, ruby, jobs=4, timeout=20):
        self.root, self.store = root.resolve(), store
        self.env, self.env_hash = environment(ruby)
        self.jobs, self.timeout = jobs, timeout
        self.stop = threading.Event()
        self.expected = {}

    def result(self, status, message, process_result=None, assertions=0, details=None):
        p = process_result or {}
        return dict(status=status, finished_at=now(), duration_ms=p.get("duration_ms", 0), assertions=assertions,
                    exit_code=p.get("exit_code"), message=message, details_json=json.dumps(details or {}),
                    stdout=p.get("stdout", ""), stderr=p.get("stderr", ""), command_json=json.dumps(p.get("command", [])))

    def command_result(self, p, success_message):
        if p["reason"]:
            return self.result("error", p["reason"], p)
        return self.result("pass" if p["exit_code"] == 0 else "fail", success_message if p["exit_code"] == 0 else "Command failed", p, assertions=1 if p["exit_code"] == 0 else 0)

    def gate(self, check, source, prefix):
        op = check["operation"]
        if op == "environment":
            p = process([self.env["ruby"], "-e", 'abort "Ruby 3.3 or newer is required" if (RUBY_VERSION.split(".").map(&:to_i)[0,2] <=> [3,3]) < 0'], source, prefix, 10, self.stop)
            return self.command_result(p, self.env["ruby_version"])
        if op == "oracle":
            constants = [c for c in self.checks if c.get("probe") == "constant"]
            driver = source / "oracle.c"
            lines = ['#include "tinyactors.h"', '#include <stdio.h>', 'int main(void) {']
            for c in constants:
                symbol = c["symbol"]
                if not symbol.startswith("TA_") or not symbol.replace("_", "").isalnum():
                    raise ValueError("Invalid oracle symbol")
                fmt, expr = ("%llu", "(unsigned long long)" + symbol) if c["numeric"] else ("%s", symbol)
                lines.append(f'printf("{symbol}={fmt}\\n", {expr});')
            lines += ['return 0;', '}']
            driver.write_text("\n".join(lines))
            p = process([self.env["clang"], "-std=c99", "-Isrc", str(driver), "-o", str(source / "oracle")], source, prefix, self.timeout, self.stop)
            if p["exit_code"] != 0 or p["reason"]:
                return self.command_result(p, "")
            p = process([str(source / "oracle")], source, Path(str(prefix) + ".values"), self.timeout, self.stop)
            if p["exit_code"] != 0 or p["reason"]:
                return self.command_result(p, "")
            values = dict(line.split("=", 1) for line in p["stdout"].splitlines())
            self.expected = {c["symbol"]: int(values[c["symbol"]]) if c["numeric"] else values[c["symbol"]] for c in constants}
            return self.result("pass", f"Compiled {len(values)} C constant values", p, assertions=len(values))
        if op == "build":
            ext = source / "bindings/ruby/ext/tinyactors_raw"
            if not (ext / "extconf.rb").is_file():
                return self.result("fail", "Missing bindings/ruby/ext/tinyactors_raw/extconf.rb")
            p = process([self.env["ruby"], "extconf.rb"], ext, prefix, 60, self.stop)
            if p["exit_code"] != 0 or p["reason"]:
                return self.command_result(p, "")
            p = process(["make", "-j2"], ext, Path(str(prefix) + ".make"), 120, self.stop)
            if p["exit_code"] != 0 or p["reason"]:
                return self.command_result(p, "")
            binaries = [p for p in ext.rglob("tinyactors_raw.*") if p.suffix in (".bundle", ".so", ".dylib")]
            if len(binaries) != 1:
                return self.result("fail", "Build must produce exactly one tinyactors_raw native library", p)
            destination = source / "bindings/ruby/lib/tinyactors" / binaries[0].name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(binaries[0], destination)
            return self.result("pass", "Built and staged a fresh native extension", p, assertions=1,
                               details={"binary_sha256": hashlib.sha256(destination.read_bytes()).hexdigest()})
        if op == "ledger":
            argv = [self.env["ruby"], "tools/check_ruby_interface.rb"]
        elif op == "core":
            argv = ["make", "test", "syntax", "arm", "symbols", "arity"]
        elif op == "harness":
            argv = [sys.executable, "tools/test_ruby_verify.py", "--ruby", self.env["ruby"]]
        else:
            raise ValueError("Unknown gate " + op)
        p = process(argv, source, prefix, 120, self.stop)
        return self.command_result(p, op + " checks passed")

    def ruby_check(self, check, source, prefix):
        specfile, outfile = Path(str(prefix) + ".input.json"), Path(str(prefix) + ".result.json")
        nonce = uuid.uuid4().hex
        spec = dict(check=check, expected=self.expected, root=str(source), result_path=str(outfile), nonce=nonce)
        specfile.write_text(json.dumps(spec))
        worker = "agent_worker.rb" if check["kind"] == "agent_suite" else "worker.rb"
        if worker == "agent_worker.rb":
            files = sorted({str(p) for pattern in check["files"] for p in source.glob(pattern) if p.is_file()})
            if not files:
                return self.result("fail", "Missing executable test file(s): " + ", ".join(check["files"]))
            spec["files"] = files
            specfile.write_text(json.dumps(spec))
        p = process([self.env["ruby"], str(source / "tools/ruby_verify" / worker), str(specfile)], source, prefix, self.timeout, self.stop)
        if p["reason"]:
            return self.result("error", p["reason"], p)
        try:
            if outfile.stat().st_size > LIMIT:
                raise ValueError("Result protocol exceeds size limit")
            evidence = json.loads(outfile.read_text())
            if evidence.get("nonce") != nonce or evidence.get("check_id") != check["id"]:
                raise ValueError("Result protocol identity mismatch")
            assertions = evidence.get("assertions")
            if type(assertions) is not int or assertions < 0:
                raise ValueError("Invalid assertion count")
            verdict = evidence.get("status")
            if verdict not in ("pass", "fail", "error"):
                raise ValueError("Invalid check verdict")
            if verdict == "pass" and (p["exit_code"] != 0 or assertions == 0 or evidence.get("skips", 0) != 0):
                raise ValueError("Passing result needs exit zero, assertions, and no skips")
        except (OSError, ValueError, TypeError) as exc:
            return self.result("error", "No valid test evidence: " + str(exc), p)
        return self.result(verdict, evidence.get("message", verdict), p, assertions, evidence)

    def execute(self, check, source, artifact):
        prefix = artifact / (check["id"].replace("?", "_question").replace("/", "_") + "-" + digest(check["id"])[:8])
        try:
            result = self.gate(check, source, prefix) if check["kind"] == "gate" else self.ruby_check(check, source, prefix)
        except Exception as exc:
            result = self.result("error", type(exc).__name__ + ": " + str(exc))
        result["log_prefix"] = str(prefix)
        return result

    def run(self, context, brief=None, patterns=None):
        self.stop.clear()
        self.expected = {}
        files, manifest, source_hash = capture(self.root)
        self.checks = catalog(self.root)
        catalog_hash = digest(self.checks)
        chosen = select(self.checks, brief, patterns)
        # Detect edits during capture/catalog generation. Later edits cannot change this immutable snapshot.
        if capture(self.root)[2] != source_hash:
            raise RuntimeError("Source changed during capture; rerun verification")
        self.store.sync(self.checks)
        run_id = uuid.uuid4().hex
        artifacts = self.store.path.parent / "artifacts" / run_id
        artifacts.mkdir(parents=True)
        locks = self.store.path.parent / "locks"
        locks.mkdir(exist_ok=True)
        lock = (locks / (run_id + ".lock")).open("a+")
        fcntl.flock(lock, fcntl.LOCK_EX)
        git = lambda args: subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True).stdout.strip()
        run = dict(run_id=run_id, started_at=now(), status="running", agent=context["agent"], model=context.get("model"),
                   reasoning=context.get("reasoning"), task=context.get("task"), iteration=context.get("iteration"),
                   context_json=json.dumps(context), scope_json=json.dumps({"brief": brief, "checks": patterns}),
                   source_hash=source_hash, catalog_hash=catalog_hash, environment_hash=self.env_hash,
                   environment_json=json.dumps(self.env), git_head=git(["rev-parse", "HEAD"]), git_status=git(["status", "--short"]),
                   source_manifest_json=json.dumps(manifest), artifact_dir=str(artifacts), pid=os.getpid())
        self.store.begin_run(run, chosen)
        counts = {"pass": 0, "fail": 0, "error": 0}
        print(f"Run {run_id}: {len(chosen)} checks; source {source_hash[:12]}", flush=True)
        try:
            with tarfile.open(artifacts / "source.tar.gz", "w:gz") as archive:
                for name, data in files.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    info.mode = 0o644
                    archive.addfile(info, io.BytesIO(data))
            with tempfile.TemporaryDirectory(prefix="tinyactors-ruby-verify-") as temp:
                source = Path(temp)
                for name, data in files.items():
                    path = source / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                gates = [c for c in chosen if c["kind"] == "gate"]
                # Build and oracle finish before Ruby probes begin. No shared mutable build directory.
                order = {"environment": 0, "oracle": 1, "build": 2, "ledger": 3, "core": 4, "harness": 5}
                gates.sort(key=lambda c: order[c["operation"]])
                for c in gates:
                    self.store.record(run_id, c["id"], {"status": "running", "started_at": now()})
                    result = self.execute(c, source, artifacts)
                    self.store.record(run_id, c["id"], result)
                    counts[result["status"]] += 1
                    print(f"{result['status'].upper():5} {c['id']}: {result['message']}", flush=True)
                probes = [c for c in chosen if c["kind"] != "gate"]
                pool = concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs)
                try:
                    futures = {}
                    for c in probes:
                        self.store.record(run_id, c["id"], {"status": "running", "started_at": now()})
                        futures[pool.submit(self.execute, c, source, artifacts)] = c
                    for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
                        c, result = futures[future], future.result()
                        self.store.record(run_id, c["id"], result)
                        counts[result["status"]] += 1
                        if index % 50 == 0 or index == len(probes):
                            print(f"Checked {index}/{len(probes)} probes: {counts}", flush=True)
                finally:
                    self.stop.set()
                    pool.shutdown(wait=True, cancel_futures=True)
                if capture(source)[2] != source_hash:
                    raise RuntimeError("A build or test modified verification source; run invalidated")
            code = 0 if counts["fail"] == 0 and counts["error"] == 0 else 1
            if capture(self.root)[2] != source_hash:
                code = 1
                print("Source changed after capture. These results are historical; rerun before acceptance.", flush=True)
            self.store.finish(run_id, "completed", code)
        except BaseException:
            self.stop.set()
            self.store.finish(run_id, "interrupted", 130)
            raise
        finally:
            lock.close()
        print(json.dumps({"run_id": run_id, "counts": counts, "exit_code": code, "database": str(self.store.path)}), flush=True)
        return code
