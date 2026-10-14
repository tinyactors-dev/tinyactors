"""The conformance suite's adapter: tinyactors-test/docs/driver.md over the
public module `tinyactors`, and nothing else (its lint refuses any other
import but the standard library).

Run by ``mise run conform``, which puts the built extension on the import
path. Frames are a four-byte big-endian length and one CBOR message, read
and written with tinyactors.decode and tinyactors.encode. Each driver
method is the binding's own calls, as an application makes them: a host
is a tinyactors.Host, a control message is the typed method of its name,
an envelope goes through Host.deliver and comes back through the Host's
peer handler. Standard output holds frames only; diagnostics go to
standard error.
"""

import sys

import tinyactors

# ta_status by the name ta_status_name gives (tinyactors/types.h): what the
# driver reports as "status".
STATUS = {
    "TA_OK": 0, "TA_ERROR_EXECUTION": -1, "TA_ERROR_COMMUNICATION": -2, "TA_ERROR_INVALID": -3,
    "TA_ERROR_NO_MEMORY": -4, "TA_ERROR_STALE": -5, "TA_ERROR_STATE": -6, "TA_ERROR_UNSUPPORTED": -7,
    "TA_ERROR_FATAL": -8, "TA_ERROR_ABORTED": -9,
}
TA_RUN_IDLE, TA_RUN_FAULT, TA_RUN_EXHAUSTED = 0, 4, 3
# The driver's datamodel names, and the vtable names a Host answers for them.
DATAMODELS = {"native": "python", "quickjs": "ecmascript"}
TYPED = {"actor.state", "actor.context", "actor.list", "actor.get", "host.stats", "host.config",
         "trace.configure", "trace.watch", "trace.unwatch", "trace.end", "trace.spans.decode"}


class DriverError(Exception):
    """The protocol's own error (driver.md, "Errors are the protocol's own")."""

    def __init__(self, code, reason, path, message):
        super().__init__(message)
        self.code, self.reason, self.path = code, reason, path


def status_of(error):
    """A ta_status for what the binding raised."""
    if isinstance(error, MemoryError):
        return STATUS["TA_ERROR_NO_MEMORY"]
    if isinstance(error, tinyactors.TinyactorsError) and error.status in STATUS:
        return STATUS[error.status]
    return STATUS["TA_ERROR_INVALID"]


def error_map(error):
    """A TinyactorsError from a reply, as core wrote it."""
    return {"code": error.code, "message": error.message or "",
            "data": {"reason": error.reason, "path": error.path}}


class Documents:
    """The resolver of a call's documents: a `src` names one by its name,
    with a leading file://, file: or ./ removed (driver.md, "src")."""

    def __init__(self, documents):
        self.documents = {document["name"]: document["text"] for document in documents}

    def resolve(self, base, source):
        for prefix in ("file://", "file:", "./"):
            if source.startswith(prefix):
                source = source[len(prefix):]
        if source not in self.documents:
            raise tinyactors.ResolveError("no document named %s" % source)
        return self.documents[source]


def compile_image(host, charts, documents):
    """charts compiled into one image, added to the host: the segment."""
    sources = [(chart["name"], chart["text"]) for chart in charts]
    with tinyactors.compile_charts(sources, Documents(documents)) as compiled:
        return host.add_image(compiled.image)


class Adapter:
    def __init__(self):
        self.host = None
        self.clock = None
        self.envelopes = []

    def peer(self, host, envelope):
        self.envelopes.append(envelope.cbor)

    def need_host(self):
        if self.host is None:
            raise DriverError(-32002, "state", "", "no host: call host.create first")
        return self.host

    # ---- the protocol vectors' methods --------------------------------------

    def host_create(self, params):
        if self.host is not None:
            raise DriverError(-32002, "state", "", "a host already: call host.free first")
        datamodel = params.get("datamodel", "quickjs")
        if datamodel not in DATAMODELS:
            raise DriverError(-32602, "range", "params.datamodel", "no such datamodel")
        config = tinyactors.decode(params["config"])
        self.clock = tinyactors.SimulatedClock()
        self.envelopes = []
        try:
            self.host = tinyactors.Host(config, peer=self.peer, clock=self.clock)
        except tinyactors.TinyactorsError as error:
            result = {"status": status_of(error)}
            if error.code is not None:
                result["error"] = tinyactors.encode({"error": error_map(error)})
            return result
        except MemoryError:
            return {"status": STATUS["TA_ERROR_NO_MEMORY"]}
        return {"status": 0, "datamodel": DATAMODELS[datamodel]}

    def host_free(self, params):
        self.need_host().close()
        self.host = None
        return {}

    def host_post(self, params):
        """The message's method is the typed method of its name; its reply
        is built from what the method's handler or on_error got, with the
        message's id (driver.md, "Kinds")."""
        host = self.need_host()
        message = tinyactors.decode(params["message"])
        if not isinstance(message, dict) or message.get("method") not in TYPED or \
                not isinstance(message.get("params", {}), dict):
            raise DriverError(-32602, "type", "params.message",
                              "a binding sends the typed methods of requests.md only")
        answered = []
        method = getattr(host, message["method"].replace(".", "_"))
        try:
            method(lambda host, result: answered.append({"result": result}),
                   lambda host, error: answered.append({"error": error_map(error)}),
                   **message.get("params", {}))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        host.run_until_idle()
        result = {"status": 0, "notifications": []}
        if answered and "id" in message:
            result["reply"] = tinyactors.encode(dict({"id": message["id"]}, **answered[0]))
        return result

    def image_add(self, params):
        host = self.need_host()
        try:
            return {"status": 0, "segment": compile_image(host, params["charts"], params["charts"])}
        except tinyactors.CompileError:
            return {"status": STATUS["TA_ERROR_INVALID"]}
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}

    def actor_spawn(self, params):
        host = self.need_host()
        try:
            session = host.spawn(host.chart(params["chart"]), params=params.get("params"))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        return {"status": 0, "session": session}

    def actor_send(self, params):
        host = self.need_host()
        try:
            host.send(params["session"], params["event"], params.get("data"))
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}
        return {"status": 0}

    def clock_advance(self, params):
        host = self.need_host()
        milliseconds = params.get("monotonic", 0) * 1000 // host.ticks_per_second
        if milliseconds > self.clock.now:
            self.clock.increment(milliseconds - self.clock.now)
        return {}

    def host_run(self, params):
        host = self.need_host()
        status = TA_RUN_IDLE
        try:
            host.run_until_idle()
        except MemoryError:
            status = TA_RUN_EXHAUSTED
        except tinyactors.TinyactorsError:
            status = TA_RUN_FAULT
        envelopes, self.envelopes = self.envelopes, []
        return {"status": status, "envelopes": envelopes, "notifications": []}

    def peer_add(self, params):
        host = self.need_host()
        try:
            return {"status": 0, "peer": host.peer_add(params["id"], trusted=bool(params.get("trusted")))}
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}

    def envelope_deliver(self, params):
        host = self.need_host()
        try:
            return {"status": 0, "route": host.deliver(params["peer"], params["cbor"])}
        except (tinyactors.TinyactorsError, MemoryError) as error:
            return {"status": status_of(error)}

    # ---- scenarios --------------------------------------------------------------

    def scenario_run(self, params):
        if params.get("datamodel", "quickjs") not in DATAMODELS:
            raise DriverError(-32602, "range", "params.datamodel", "no such datamodel")
        return {"outcome": Scenario(params["documents"]).run(params["steps"])}


class Refused(Exception):
    """A step the host refused: the scenario fails (driver.md)."""


class Scenario:
    """scenario.run in the scenario environment (driver.md): a host of its
    own on a simulated clock, the steps' calls, the loop until idle after
    each, and the first Outcome log."""

    def __init__(self, documents):
        self.documents = documents
        self.aliases = {}
        self.outcome = None
        self.clock = tinyactors.SimulatedClock()
        self.blobs = {}  # the implementation's store (driver.md): hibernated actors' blobs
        self.snapshots = {}
        self.host = None

    def wake(self, host, session, job):
        """WAKE: the actor's blob from the store."""
        host.wake(session, self.blobs.pop(session))

    def log(self, host, entry):
        if self.outcome is None and entry.label == "Outcome":
            self.outcome = "pass" if entry.text == "pass" else "fail"

    def value(self, value):
        """"@alias" at any depth: that actor's address."""
        if isinstance(value, str) and value.startswith("@") and value[1:] in self.aliases:
            return "#_scxml_%d" % self.aliases[value[1:]]
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, dict):
            return {key: self.value(item) for key, item in value.items()}
        return value

    CONFIG = {"systemID": "conformance", "system": {"ticksPerSecond": 1000}}

    def run(self, steps):
        self.host = tinyactors.Host(self.CONFIG, log=self.log, clock=self.clock, wake=self.wake)
        try:
            for step in steps:
                try:
                    self.step(self.host, step)
                    if self.outcome is None:
                        self.host.run_until_idle()
                except tinyactors.CompileError as error:
                    print("conform: a document does not compile: %s" % error, file=sys.stderr)
                    self.outcome = "compile"
                except (Refused, tinyactors.TinyactorsError, MemoryError) as error:
                    print("conform: the step %r was refused: %r" % (step, error), file=sys.stderr)
                    self.outcome = self.outcome or "fail"
                if self.outcome is not None:
                    break
        finally:
            self.host.close()
        return self.outcome or "timeout"

    def step(self, host, step):
        if "image" in step:
            named = {document["name"]: document for document in self.documents}
            missing = [name for name in step["image"] if name not in named]
            if missing:
                raise Refused("image names documents the call does not have: %r" % missing)
            compile_image(host, [named[name] for name in step["image"]], self.documents)
        elif "spawn" in step:
            self.aliases[step["spawn"]] = host.spawn(host.chart(step["chart"]), name=step["spawn"],
                                                     params=self.value(step.get("params")))
        elif "send" in step:
            host.send(self.session(step["send"]), step["event"], self.value(step.get("data")))
        elif "sendMany" in step:
            items = [(self.session(item["to"]), item["event"], self.value(item.get("data")))
                     for item in step["sendMany"]]
            if host.send_many(items) != len(items):
                raise Refused("send_many took fewer events")
        elif "advance" in step:
            self.clock.increment(step["advance"])  # ticksPerSecond 1000: ticks are milliseconds
        elif "hibernate" in step:
            session = self.session(step["hibernate"])
            self.blobs[session] = host.hibernate(session)
        elif "snapshot" in step:
            self.snapshots[step["snapshot"]] = host.snapshot()
        elif "restore" in step:
            if step["restore"] not in self.snapshots:
                raise Refused("a restore of a snapshot no step took: %s" % step["restore"])
            restored = tinyactors.Host.restore(self.snapshots[step["restore"]], self.CONFIG, log=self.log,
                                               clock=self.clock, wake=self.wake)
            host.close()
            self.host = restored  # a restart: the restored host replaces the running one
        elif "migrate" in step:
            done = []
            host.migrate(lambda host, result: done.append(result), wake=bool(step["migrate"].get("wake")),
                         timeout=step["migrate"].get("timeout", 0))
            for _ in range(100):  # the loop until its JOB_DONE, bounded as ta-conform's settles
                host.run_until_idle()
                if done:
                    break
            if not done:
                raise Refused("the migration did not end")
        else:
            raise Refused("not in the binding's API yet: %s" % ", ".join(step))

    def session(self, alias):
        if alias not in self.aliases:
            raise Refused("a send to an alias no spawn made: %s" % alias)
        return self.aliases[alias]


def read_exactly(stream, size):
    """size bytes of a frame; EOF inside a frame raises EOFError."""
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            raise EOFError("incomplete driver frame")
        chunks.extend(chunk)
    return bytes(chunks)


def answer(adapter, call):
    method = call.get("method")
    function = getattr(adapter, method.replace(".", "_"), None) if isinstance(method, str) else None
    if function is None or method.startswith("_") or method in ("need.host", "peer"):
        return {"id": call["id"], "error": {"code": -32601, "message": "%s is not offered" % method,
                                             "data": {"reason": "unknown", "path": "method"}}}
    try:
        return {"id": call["id"], "result": function(call.get("params") or {})}
    except KeyError as missing:  # a required param left out
        return {"id": call["id"], "error": {"code": -32602, "message": "params.%s: missing" % missing.args[0],
                                             "data": {"reason": "missing", "path": "params.%s" % missing.args[0]}}}
    except DriverError as error:
        return {"id": call["id"], "error": {"code": error.code, "message": str(error),
                                             "data": {"reason": error.reason, "path": error.path}}}


def main():
    source, target = sys.stdin.buffer, sys.stdout.buffer
    adapter = Adapter()
    while True:
        first = source.read(1)
        if not first:
            return
        header = first + read_exactly(source, 3)
        call = tinyactors.decode(read_exactly(source, int.from_bytes(header, "big")))
        encoded = tinyactors.encode(answer(adapter, call))
        target.write(len(encoded).to_bytes(4, "big") + encoded)
        target.flush()


if __name__ == "__main__":
    main()
