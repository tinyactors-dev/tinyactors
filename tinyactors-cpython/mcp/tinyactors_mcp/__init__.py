"""tinyactors_mcp: a Model Context Protocol server over running tinyactors
hosts, for an agent that inspects a system, sends to it, reads its traces
and rehearses and deploys new versions of its charts.

An opt-in package beside tinyactors: Python over that module's public API
and the MCP SDK (`mcp`), nothing else. Every language's server is the same
three parts (docs/adr/0021 in the repository root;
tinyactors-core/mcp/README.md):

- the catalog, core's tinyactors-core/mcp/tools.json, which the tinyactors
  module embeds (`tinyactors.mcp_tools()`): every tool's name, description,
  input schema, annotations, the option that gates it, and what runs it;
- the forwarding: a `snapshot` tool is a method of core's snapshot reader
  (`snapshot.request`), a `host` tool core's requests through the typed
  methods (HOST_METHODS);
- the binding operations: what only Python can do: the tap, which wraps a
  host's keywords when the host is made (a handler is fixed then) and keeps
  what the agent reads later, a copy of a snapshot restored with
  stand-ins, compile and deploy, send, ask, terminate and the services.

    t = tap()
    async with Host(compile_dir("charts"), **t.options(system="shop", services=...)) as host:
        mcp = create_mcp({"shop": {"host": host, "tap": t}}, allow_deploy=True)
        await mcp.serve_stdio()            # or: served = await mcp.listen(port=7333, token=...)
"""

import asyncio
import contextlib
import json
import re

import mcp_types as types
import tinyactors
from mcp.server.lowlevel import Server

CATALOG = json.loads(tinyactors.mcp_tools())
"""The catalog of the tinyactors module this package runs over."""

__all__ = ["CATALOG", "Tap", "arguments_of", "create_mcp", "migration_summary", "tap"]


# ---- tap: what the host's handlers see, kept for the agent -----------------------------------

class Tap:
    """What a tap keeps of what a host's handlers saw: `given` (the keywords
    given to options(), for copies of the host), `spans` (decoded, newest
    last), `logs` ({session, label, text}), `migrations` (results, newest
    last), `services` (the running invocations, by invoking session)."""

    def __init__(self, spans, logs, migrations):
        self.given = None
        self.spans = []
        self.logs = []
        self.migrations = []
        self.migration_waiters = []
        self.services = {}
        self.limits = {"spans": spans, "logs": logs, "migrations": migrations}

    @staticmethod
    def _keep(items, item, limit):
        items.append(item)
        if len(items) > limit:
            del items[: len(items) - limit]

    def options(self, **given):
        """The keywords for Host(charts, ...) or Host.restore: `given` with
        its telemetry, log, migration_done and services handlers wrapped.
        Every handler you give is still called."""
        self.given = dict(given)
        options = dict(given)
        telemetry, log, done, services = (given.get(k) for k in ("telemetry", "log", "migration_done", "services"))

        def on_telemetry(signal, data):
            if signal == "traces":
                with contextlib.suppress(Exception):  # not ours to fail the exporter
                    for span in tinyactors.decode_traces(data):
                        self._keep(self.spans, span, self.limits["spans"])
            return telemetry(signal, data) if telemetry else None

        def on_log(entry):
            self._keep(self.logs, {"session": entry.session, "label": entry.label, "text": entry.text}, self.limits["logs"])
            return log(entry) if log else None

        def on_migration_done(result):
            copy = {**result, "failed": list(result["failed"]), "pending": list(result["pending"])}
            self._keep(self.migrations, copy, self.limits["migrations"])
            waiters, self.migration_waiters = self.migration_waiters, []
            for waiter in waiters:
                if not waiter.done():
                    waiter.set_result(copy)
            return done(result) if done else None

        options.update(telemetry=on_telemetry, log=on_log, migration_done=on_migration_done)
        if services:
            def wrapped(start):
                def on_invoke(invocation):
                    running = [i for i in self.services.get(invocation.session, []) if i.running]
                    self.services[invocation.session] = [*running, invocation]
                    return start(invocation)
                return on_invoke
            options["services"] = {kind: wrapped(start) for kind, start in services.items()}
        return options


def tap(*, spans=5000, logs=1000, migrations=50):
    """A tap for a host: how many spans, log lines and migration results it
    keeps. Make the host with its keywords: Host(charts, **t.options(...))."""
    return Tap(spans, logs, migrations)


# ---- values for a model ------------------------------------------------------------------------

def _plain(value):
    """A value as JSON: an integer past 2^53 is its decimal text, bytes are their length."""
    if isinstance(value, bool) or value is None or isinstance(value, (str, float)):
        return value
    if isinstance(value, int):
        return value if abs(value) <= 2**53 - 1 else str(value)
    if isinstance(value, (bytes, bytearray, memoryview)):
        return f"<{len(value)} bytes>"
    if isinstance(value, dict):
        return {str(k): _plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(v) for v in value]
    if value is tinyactors.UNDEFINED:
        return None
    if isinstance(value, tinyactors.Address):
        return str(value)
    return str(value)


def _ok(value):
    structured = _plain(value)
    text = json.dumps(structured, indent=2)
    if not isinstance(structured, dict):
        structured = {"items": structured}
    return types.CallToolResult(content=[types.TextContent(text=text)], structured_content=structured)


def _refused(message, **structured):
    return types.CallToolResult(content=[types.TextContent(text=message)], structured_content={"error": message, **structured},
                                is_error=True)


def migration_summary(result):
    """A migration's result for a model: failures grouped by reason, five sessions of each."""
    if not result or "failed" not in result:
        return result
    failed = {}
    for failure in result["failed"]:
        group = failed.setdefault(failure["reason"], {"count": 0, "sample": []})
        group["count"] += 1
        if len(group["sample"]) < 5:
            group["sample"].append(failure["session"])
    return {"migration": result["migration"], "migrated": result["migrated"], "failed": failed, "pending": len(result["pending"])}


# ---- the catalog's arguments ----------------------------------------------------------------------

_TYPES = {
    "string": lambda v: isinstance(v, str),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "array": lambda v: isinstance(v, list),
    "object": lambda v: isinstance(v, dict),
}


def arguments_of(tool, given=None):
    """A tool's arguments checked against its schema (the catalog's subset
    of JSON Schema), its defaults filled in; TypeError for others."""
    given = given or {}
    schema = tool["inputSchema"]
    properties, required = schema.get("properties", {}), schema.get("required", [])
    for key in given:
        if key not in properties:
            raise TypeError(f"{tool['name']} takes no argument {key}")
    args = {}
    for key, rule in properties.items():
        value = given.get(key)
        if value is None:
            value = rule.get("default")
        if value is None:
            if key in required:
                raise TypeError(f"{tool['name']} needs {key}")
            continue
        if "type" in rule and not _TYPES[rule["type"]](value):
            raise TypeError(f"{tool['name']}: {key} is a {rule['type']}")
        if "enum" in rule and value not in rule["enum"]:
            raise TypeError(f"{tool['name']}: {key} is one of {', '.join(rule['enum'])}")
        args[key] = value
    return args


# ---- forwarding: core's requests through the typed methods -----------------------------------------

# A method of docs/requests.md, by its name, to the typed method of the host
# that posts it (no raw post: ADR 0012, decision 3); given no handler, each
# returns a future of the host's loop.
HOST_METHODS = {
    "host.stats": lambda host, params: host.host_stats(**params),
    "trace.watch": lambda host, params: host.trace_watch(**params),
    "trace.unwatch": lambda host, params: host.trace_unwatch(**params),
    "actor.get": lambda host, params: host.actor_get(**params),
    "actor.state": lambda host, params: host.actor_state(**params),
    "actor.context": lambda host, params: host.actor_context(**params),
}


def _without(mapping, *keys):
    return {k: v for k, v in mapping.items() if k not in keys}


# ---- the server ----------------------------------------------------------------------------------

class _MCP:
    """The tools over `systems`; see create_mcp."""

    def __init__(self, systems, options):
        self.systems = systems
        self.options = options
        self.snapshots = {}
        self.drafts = {}
        self.next_snapshot = 1
        self.next_draft = 1
        self.tools = {tool["name"]: tool for tool in CATALOG["tools"]}

    # -- the parts every operation reads

    def system(self, name):
        if name not in self.systems:
            raise LookupError(f"no system {name}; known: {', '.join(self.systems)}")
        return self.systems[name]

    @staticmethod
    def tap_of(served, what):
        if not served.get("tap"):
            raise LookupError(f"{what} needs the host to be made with tap().options(...)")
        return served["tap"]

    @staticmethod
    def actor_of(served, which):
        """An actor named by a session id or the name it was spawned with."""
        return served["host"].actor(int(which) if re.fullmatch(r"\d+", str(which)) else str(which))

    def snapshot_of(self, snapshot):
        if snapshot not in self.snapshots:
            raise LookupError(f"no snapshot {snapshot}; take one with the snapshot tool")
        return self.snapshots[snapshot]

    def copy_of(self, snap, record=None, services=None):
        """A copy of a snapshot: the same system, config and datamodel, on a
        simulated clock, with stand-ins for the world, driven synchronously
        (tinyactors-core/mcp/README.md, "Copies and stand-ins")."""
        served = self.system(snap["system"])
        given = self.tap_of(served, "a copy of a snapshot").given
        services = services or {}
        answers = []
        keywords = {"system": given.get("system"), "config": given.get("config"), "clock": tinyactors.SimulatedClock()}

        class StandIn:
            def __init__(self, kind, location):
                self.kind, self.location = kind, location

            def receive(self, send):
                if record is not None:
                    record["sends"].append(f"{self.kind} {send.event}")

        if given.get("io_processors"):
            keywords["io_processors"] = {kind: StandIn(kind, getattr(p, "location", None))
                                         for kind, p in given["io_processors"].items()}
        if given.get("services"):
            def stand_in(kind):
                def start(invocation):
                    if record is not None:
                        record["invocations"].append(kind + (" (restored)" if invocation.restored else ""))
                    answer = services.get(kind, "silent")
                    if answer != "silent":
                        answers.append((invocation, answer))  # answered after the turn that invoked it
                    return object()
                return start
            keywords["services"] = {kind: stand_in(kind) for kind in given["services"]}

        def transport(send):
            if record is not None:
                record["sends"].append(f"transport {send.event}")

        keywords["transport"] = transport
        if record is not None:
            keywords.update(record["handlers"])
        copy = tinyactors.Host.restore(snap["bytes"], [], **{k: v for k, v in keywords.items() if v is not None})
        return copy, keywords["clock"], answers

    @staticmethod
    def settle(copy, answers):
        """Turns the copy until idle, answering the stand-in services' starts."""
        copy.run_until_idle()
        while answers:
            invocation, answer = answers.pop(0)
            with contextlib.suppress(tinyactors.TinyactorsError):  # it ended meanwhile
                if answer == "done":
                    invocation.done()
                else:
                    invocation.send_back("error.communication" if answer == "error" else answer)
            copy.run_until_idle()

    @staticmethod
    def rest_of(reader, chart):
        """The resting configurations of a chart's actors: snapshot.summary's states."""
        return reader.request("snapshot.summary")["states"].get(chart, {})

    # -- the binding operations: what the catalog marks {"binding": name}

    async def op_systems(self, args):
        out = []
        for name, served in self.systems.items():
            stats = await served["host"].host_stats()
            out.append({"system": name, "charts": [c["name"] for c in stats["charts"]], "actors": stats["host"]["actors"],
                        "hibernated": stats["host"]["hibernated"], "segments": stats["host"]["segments"]})
        return _ok(out)

    async def op_send(self, args):
        actor = self.actor_of(self.system(args["system"]), args["actor"])
        actor.send(args["event"], args.get("data"))
        return _ok({"sent": args["event"], "to": actor.id})

    async def op_ask(self, args):
        actor = self.actor_of(self.system(args["system"]), args["actor"])
        reply, data = await asyncio.wait_for(actor.ask(args["event"], args.get("data")), args["timeoutMs"] / 1000)
        return _ok({"reply": reply, "data": data})

    async def op_traces(self, args):
        spans = self.tap_of(self.system(args["system"]), "traces").spans
        if "session" in args:
            spans = [s for s in spans if s.get("attributes", {}).get("scxml.session_id") == args["session"]]
        if "name" in args:
            spans = [s for s in spans if s["name"].startswith(args["name"])]
        return _ok({"total": len(spans), "spans": spans[-args["limit"]:]})

    async def op_logs(self, args):
        lines = self.tap_of(self.system(args["system"]), "logs").logs
        if "session" in args:
            lines = [line for line in lines if line["session"] == args["session"]]
        return _ok({"total": len(lines), "logs": lines[-args["limit"]:]})

    async def op_services(self, args):
        out = []
        for session, invocations in self.tap_of(self.system(args["system"]), "services").services.items():
            if "session" in args and session != args["session"]:
                continue
            out += [{"session": session, "invokeid": i.invokeid, "type": i.type, "src": i.src} for i in invocations if i.running]
        return _ok(out)

    async def op_end_service(self, args):
        invocations = self.tap_of(self.system(args["system"]), "end_service").services.get(args["session"], [])
        invocation = next((i for i in invocations if i.running and i.invokeid == args["invokeid"]), None)
        if invocation is None:
            return _refused(f"no running service {args['invokeid']} of session {args['session']}")
        if args["as"] == "done":
            invocation.done()
        else:
            invocation.send_back("error.communication")
        return _ok({"ended": args["invokeid"], "session": args["session"], "as": args["as"]})

    async def op_chart_source(self, args):
        text = (self.system(args["system"]).get("sources") or {}).get(args["chart"])
        if not text:
            return _refused(f"no source for chart {args['chart']}")
        return types.CallToolResult(content=[types.TextContent(text=text)])

    async def op_compile(self, args):
        try:
            compiled = tinyactors.compile_charts([(args["name"], args["text"])])
        except tinyactors.CompileError as error:
            return _refused(f"does not compile: {error}", diagnostics=_plain([_diagnostic(d) for d in error.diagnostics]))
        with compiled:
            draft = f"d{self.next_draft}"
            self.next_draft += 1
            diagnostics = [_diagnostic(d) for d in compiled.diagnostics]
            self.drafts[draft] = {"name": args["name"], "text": args["text"], "image": compiled.image, "diagnostics": diagnostics}
        return _ok({"draft": draft, "diagnostics": diagnostics})

    async def op_deploy(self, args):
        served = self.system(args["system"])
        draft = self.drafts.get(args["draft"])
        if draft is None:
            return _refused(f"no draft {args['draft']}")
        host = served["host"]
        segment = host.add_image(draft["image"])
        if served.get("sources") is not None:
            served["sources"][args["chart"]] = draft["text"]
        if not args["migrate"]:
            return _ok({"segment": segment})
        waiter = asyncio.get_running_loop().create_future()
        self.tap_of(served, "deploy with migrate").migration_waiters.append(waiter)
        migration = host.migrate(chart=args["chart"], wake=args["wake"])
        try:
            result = migration_summary(await asyncio.wait_for(waiter, args["timeoutMs"] / 1000))
        except TimeoutError:
            result = "still running"
        return _ok({"segment": segment, "migration": migration, "result": result})

    async def op_terminate(self, args):
        actor = self.actor_of(self.system(args["system"]), args["actor"])
        actor.terminate()
        return _ok({"terminated": actor.id})

    async def op_snapshot(self, args):
        host = self.system(args["system"])["host"]
        for attempt in range(41):
            try:
                data = host.snapshot()
                break
            except tinyactors.StateError:
                if attempt == 40:  # a reply pending or a migration running: after the next turn
                    raise
                await asyncio.sleep(0.025)
        snapshot = f"s{self.next_snapshot}"
        self.next_snapshot += 1
        reader = tinyactors.read_snapshot(data)
        self.snapshots[snapshot] = {"id": snapshot, "system": args["system"], "bytes": data, "reader": reader}
        while len(self.snapshots) > self.options.get("snapshot_keep", 8):
            del self.snapshots[next(iter(self.snapshots))]
        actors = sum(segment.get("actors", 0) for segment in reader.request("snapshot.summary")["segments"])
        return _ok({"snapshot": snapshot, "system": args["system"], "bytes": len(data), "actors": actors})

    async def op_snapshot_variables(self, args):
        snap = self.snapshot_of(args["snapshot"])
        filters = {k: args[k] for k in ("chart", "state") if k in args}
        sessions, offset = [], 0
        while len(sessions) < args["limit"]:
            page = snap["reader"].request("snapshot.actors", {**filters, "offset": offset, "limit": 512})
            for row in page["actors"]:
                if "session" in args and row["session"] != args["session"]:
                    continue
                if row["status"] != "hibernated" and len(sessions) < args["limit"]:
                    sessions.append(row["session"])
            offset += len(page["actors"])
            if not page["actors"] or offset >= page["total"]:
                break
        copy, _, _ = self.copy_of(snap)
        out = {}
        with copy:
            # posted together, before the copy's first turn runs anything: the snapshot's values
            for session in sessions:
                copy.actor_context(lambda result, s=session: out.__setitem__(s, result["variables"]),
                                   lambda error, s=session: out.__setitem__(s, {"refused": str(error)}), session=session)
            copy.run_until_idle()
        return _ok({"actors": [{"session": s, "variables": out.get(s)} for s in sessions]})

    async def op_rehearse(self, args):
        snap = self.snapshot_of(args["snapshot"])
        draft = self.drafts.get(args["draft"])
        if draft is None:
            return _refused(f"no draft {args['draft']}")
        result = {}
        record = {"sends": [], "invocations": [], "logs": [], "errors": []}
        record["handlers"] = {
            "log": lambda e: record["logs"].append(f"{e.session} {e.label}: {e.text}"),
            "migration_done": lambda r: result.update(r),
            "on_error": lambda actor, error: record["errors"].append({"session": actor.id, "error": str(error)}),
            "on_handler_error": lambda error, effect: record["errors"].append({"handler": str(error)}),
        }
        copy, clock, answers = self.copy_of(snap, record, args.get("services"))
        with copy:
            before = self.rest_of(snap["reader"], args["chart"])
            copy.add_image(draft["image"])
            migration = copy.migrate(chart=args["chart"])
            self.settle(copy, answers)
            elapsed = 0
            while elapsed < args["advanceMs"]:
                step = min(250, args["advanceMs"] - elapsed)
                clock.increment(step / 1000)
                self.settle(copy, answers)
                elapsed += step
            after = self.rest_of(tinyactors.read_snapshot(copy.snapshot()), args["chart"])

        def tally(items):
            counts = {}
            for item in items:
                counts[item] = counts.get(item, 0) + 1
            return counts

        return _ok({"migration": migration, "result": migration_summary(result or None), "before": before, "after": after,
                    "sends": tally(record["sends"]), "invocations": tally(record["invocations"]),
                    "logs": record["logs"][:20], "errors": record["errors"][:20]})

    # -- one call

    async def call(self, tool, given):
        """One tool's call: its arguments checked, its gate, then what runs it."""
        args = arguments_of(tool, given)
        gate = tool.get("gate")
        if gate and not self.options.get(_snake(gate)):
            return _refused(f"{tool['name']} is off for this server ({gate})")
        run = tool["run"]
        if "snapshot" in run:
            reader = self.snapshot_of(args["snapshot"])["reader"]
            return _ok(reader.request(run["snapshot"], _without(args, "snapshot")))
        if "host" in run:
            served = self.system(args["system"])
            params = _without(args, "system", "actor")
            if "actor" in args:
                params["session"] = self.actor_of(served, args["actor"]).id
            if isinstance(run["host"], str):
                return _ok(await HOST_METHODS[run["host"]](served["host"], params))
            out = {}
            for key, method in run["host"].items():
                try:
                    out[key] = await HOST_METHODS[method](served["host"], params)
                except tinyactors.TinyactorsError as error:
                    out[key] = {"refused": str(error)}
            return _ok(out)
        return await getattr(self, "op_" + run["binding"])(args)

    def server(self):
        """One MCP server over the tools (the SDK's low-level Server); every
        server of this object shares its snapshots and drafts."""
        listed = [types.Tool.model_validate({k: tool[k] for k in ("name", "description", "inputSchema", "annotations")})
                  for tool in CATALOG["tools"]]

        async def on_list_tools(ctx, params):
            return types.ListToolsResult(tools=listed)

        async def on_call_tool(ctx, params):
            tool = self.tools.get(params.name)
            if tool is None:
                return _refused(f"no tool {params.name}")
            try:
                return await self.call(tool, params.arguments or {})
            except Exception as error:  # every refusal is the tool's error, for the agent to read
                extra = {"reason": error.reason, "path": error.path} if getattr(error, "reason", None) else {}
                return _refused(f"{type(error).__name__}: {error}", **extra)

        return Server(CATALOG["name"], version="0.0.0", on_list_tools=on_list_tools, on_call_tool=on_call_tool)

    async def serve_stdio(self):
        """Serves the tools on standard input and output until the client closes them."""
        from mcp.server.stdio import stdio_server

        server = self.server()
        async with stdio_server() as (read_stream, write_stream):
            await server.run(read_stream, write_stream, server.create_initialization_options())

    async def listen(self, *, port=0, host="127.0.0.1", path="/mcp", token=None):
        """Serves the tools over Streamable HTTP at http://host:port/path;
        `token` a bearer token every request must carry. Returns an object
        with `url` and an awaitable `close()`."""
        import uvicorn

        app = self.server().streamable_http_app(streamable_http_path=path, host=host)
        expected = f"Bearer {token}".encode() if token else None

        async def guarded(scope, receive, send):
            if scope["type"] == "http" and expected and dict(scope["headers"]).get(b"authorization") != expected:
                await send({"type": "http.response.start", "status": 401, "headers": []})
                await send({"type": "http.response.body", "body": b""})
                return
            await app(scope, receive, send)

        served = uvicorn.Server(uvicorn.Config(guarded, host=host, port=port, log_level="warning", lifespan="on"))
        task = asyncio.create_task(served.serve())
        while not served.started:
            if task.done():
                task.result()
            await asyncio.sleep(0.01)
        bound = served.servers[0].sockets[0].getsockname()[1]

        class Served:
            url = f"http://{host}:{bound}{path}"

            async def close(self):
                served.should_exit = True
                await task

        return Served()


def _snake(name):
    return re.sub(r"[A-Z]", lambda m: "_" + m.group(0).lower(), name)


def _diagnostic(diagnostic):
    return {key: getattr(diagnostic, key) for key in ("file", "line", "column", "message", "severity") if hasattr(diagnostic, key)}


def create_mcp(systems, *, allow_deploy=False, allow_terminate=False, snapshot_keep=8):
    """The MCP tools over `systems`: {name: {"host": host, "tap": tap,
    "sources": {chart: scxml}}}, where `tap` is the tap whose options made
    the host (traces, logs, services, migrations and every copy need it)
    and `sources` each chart's SCXML (the image holds no source; deploy
    writes the draft's here). allow_deploy enables deploy and end_service,
    allow_terminate terminate. server() makes one MCP server; serve_stdio()
    and listen() serve them."""
    return _MCP(systems, {"allow_deploy": allow_deploy, "allow_terminate": allow_terminate, "snapshot_keep": snapshot_keep})
