"""Stable check IDs and executable specifications. No test is a TODO or an unconditional failure."""
import hashlib
import json
import re
from pathlib import Path

BASE = "bindings/ruby/briefs/"
CLASSES = {
    "raw.Result": (["new"], ["status", "value", "ok?"]),
    "raw.Allocator": (["malloc", "new"], []),
    "raw.Arena": ([], ["closed?"]),
    "raw.Memory": (["allocate"], ["size", "read", "write", "closed?", "release"]),
    "raw.Value": (["new"], ["object"]),
    "raw.System": ([], ["closed?"]),
    "raw.Definition": ([], []),
    "raw.Context": ([], []),
    "raw.ActorRef": (["new"], ["system", "id", "==", "hash"]),
    "raw.RequestRef": (["new"], ["system", "id", "==", "hash"]),
    "raw.InvocationRef": (["new"], ["system", "id", "==", "hash"]),
    "raw.InspectCursor": (["new"], ["reset"]),
    "high.Error": (["check"], ["status", "operation", "diagnostic", "cause"]),
    "high.Event": (["from_raw"], ["name", "type", "data", "sendid", "origin", "origintype", "invokeid", "message_id", "trace", "to_raw"]),
    "high.Target": (["self", "internal", "parent", "instance", "name", "invoke", "uri", "parse"], ["kind", "instance", "text", "to_raw"]),
    "high.Message": (["new"], ["name", "data", "trace", "to_raw"]),
    "high.CodeRegistry": (["new"], ["register", "fetch", "seal", "code_owner"]),
    "high.Action": (["custom", "script", "raise", "assign", "cancel", "if", "foreach", "log"], ["compile"]),
    "high.Send": (["new", "reply"], ["compile", "evaluate"]),
    "high.Invoke": (["new"], ["compile"]),
    "high.StateBuilder": ([], ["state", "initial", "on", "always", "entry", "exit", "invoke", "final", "parallel", "history", "to_h"]),
    "high.MachineBuilder": ([], ["initial", "state", "to_h"]),
    "high.Definition": (["build", "prepare"], ["raw", "revision", "description", "footprint", "release"]),
    "high.Context": ([], ["state", "event", "_event", "self_ref", "now_ns", "phase", "in_state?", "send", "reply", "raise", "cancel"]),
    "high.DataModel": (["new"], ["name", "snapshot_policy", "raw_value_ops", "raw_data_ops"]),
    "high.Snapshot": ([], ["ref", "status", "reason", "revision", "sessionid", "registered_name", "state", "active", "history", "event", "event_included?"]),
    "high.Inspection": (["new"], ["next_page", "reset", "each"]),
    "high.Actor": ([], ["system", "ref", "id", "post", "send", "stop", "release", "snapshot", "set_limits", "upgrade", "to_target"]),
    "high.IORequest": ([], ["ref", "kind", "actor_ref", "revision", "invocation", "invokeid", "type", "target", "event", "body", "trace", "accepted_ns", "due_ns", "complete", "completed?"]),
    "high.Invocation": ([], ["system", "ref", "id", "emit", "done", "fail", "cancelled?"]),
    "high.System": (["new"], ["raw", "define", "prepare", "spawn", "publish", "unpublish", "lookup_definition", "bind", "unbind", "lookup_actor", "lookup_session", "post", "run", "next_deadline", "stats", "inspect_stream", "io_next", "receive", "receive_error", "take_errors", "shutdown", "close", "snapshot_policy"]),
}


def stem(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def owner(key):
    layer, name = key.split(".")
    return BASE + ("raw/" if layer == "raw" else "high_level/") + stem(name) + ".md"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def build(root):
    checks = []
    def add(id, title, brief, kind, **spec):
        item = dict(id=id, title=title, brief=brief, kind=kind, **spec)
        item["revision"] = digest(item)
        checks.append(item)
    for id, title in [("environment", "Ruby supports the replacement extension"),
                      ("oracle", "Compile public C constants as the reference oracle"),
                      ("build", "Build the native extension from the captured source"),
                      ("ledger", "C API assignment ledger agrees with the header and exports"),
                      ("core", "C runtime regression and freestanding checks pass")]:
        add("gate." + id, title, BASE + "00-bootstrap.md", "gate", operation=id, shared=True)
    add("gate.native_loaded", "Load the freshly built native extension through the shipped loader", BASE + "00-bootstrap.md", "ruby", probe="native_loaded", shared=True)
    add("gate.harness", "Verification harness regression tests pass", BASE + "99-integration.md", "gate", operation="harness", shared=True)
    header = (root / "src/tinyactors.h").read_text()
    clean = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
    enums = re.findall(r"\benum\s+\w+\s*\{([^}]+)\}", clean, re.S)
    numeric = sorted({name for block in enums for name in re.findall(r"\bTA_[A-Z0-9_]+(?=\s*(?:=|,|$))", block)} |
                     {"TA_INDEX_NONE", "TA_REVISION_ANY", "TA_TIME_NEVER", "TA_UNLIMITED"})
    for symbol in numeric + ["TA_SCXML_IOPROCESSOR", "TA_SCXML_INVOKE_TYPE", "TA_BASIC_HTTP_IOPROCESSOR"]:
        add("constant." + symbol, "Export the exact C value for " + symbol, BASE + "00-bootstrap.md", "ruby", probe="constant", symbol=symbol, numeric=symbol in numeric)
    ledger = json.loads((root / "bindings/ruby/api-coverage.json").read_text())
    for entry in ledger["entries"]:
        add("raw.implemented." + entry["symbol"], "Record implementation evidence for " + entry["symbol"],
            entry["brief"], "ruby", probe="ledger_entry", symbol=entry["symbol"])
        for probe in ("export", "native_export"):
            add("raw." + probe + "." + entry["symbol"], probe.replace("_", " ") + ": " + entry["symbol"], entry["brief"], "ruby", probe=probe, symbol=entry["symbol"])
    for key, (class_methods, instance_methods) in CLASSES.items():
        add("class." + key, "Expose class " + key, owner(key), "ruby", probe="class", klass=key)
        for side, methods in [("class", class_methods), ("instance", instance_methods)]:
            for method in methods:
                add(f"method.{key}.{side}.{method}", f"Expose {key} {side} method {method}", owner(key), "ruby", probe="method", klass=key, side=side, method=method)
        layer, name = key.split(".")
        file = "bindings/ruby/test/" + ("raw" if layer == "raw" else "high_level") + "/" + stem(name) + "_test.rb"
        add("agent_suite." + key, "Execute nonempty assertion-bearing tests for " + key, owner(key), "agent_suite", files=[file])
    for name, brief, file in [("bootstrap","00-bootstrap.md","bootstrap_test.rb"), ("records","01-records.md","raw/records_test.rb"), ("callbacks","02-callbacks.md","raw/callbacks_test.rb")]:
        add("agent_suite." + name, "Execute " + name + " regression tests", BASE + brief, "agent_suite", files=["bindings/ruby/test/" + file])
    add("agent_suite.integration", "Execute complete binding integration tests", BASE + "99-integration.md", "agent_suite", files=["bindings/ruby/test/integration/**/*_test.rb"])
    # Behavioral variants carry data, not generated TODO tests. Every variant executes assertions in probes.rb.
    behaviors = json.loads((root / "tools/ruby_verify/behaviors.json").read_text())
    for item in behaviors:
        key = item.pop("owner")
        id = item.pop("id")
        title = item.pop("title")
        add("behavior." + id, title, owner(key) if "." in key else BASE + key + ".md", "ruby", **item)
    ids = [c["id"] for c in checks]
    if len(set(ids)) != len(ids):
        raise ValueError("Duplicate catalog IDs")
    for c in checks:
        if not (root / c["brief"]).is_file():
            raise ValueError("Missing brief: " + c["brief"])
    return sorted(checks, key=lambda c: c["id"])
