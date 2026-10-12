"""scenario.run (driver.md, "Scenario steps" and "The scenario
environment"): a Host of its own on a SimulatedClock, each step one call
of the binding's API, every input running the turn until the host is idle
at the current time. The charts check what happened; the first Outcome
log decides."""

import tinyactors

from . import params as p
from .frames import INVALID_PARAMS, Refused, log
from .vectors import datamodel_of

CONFIG = {"systemID": "conformance", "system": {"ticksPerSecond": 1000}}
PREFIXES = ("file://", "file:", "./")


class Stop(Exception):
    """The scenario has its outcome: no step runs after it."""


class Scenario:
    def __init__(self, documents):
        self.documents = {document["name"]: document["text"] for document in documents}
        # a src names a document by its name, with a leading file://, file:
        # or ./ (driver.md, "src")
        self.files = {prefix + name: text for name, text in self.documents.items() for prefix in ("",) + PREFIXES}
        self.clock = tinyactors.SimulatedClock()
        self.host = None
        self.aliases = {}
        self.outcome = None

    def on_log(self, session, label, text):
        if label == "Outcome" and not self.outcome:
            self.outcome = "pass" if text == "pass" else "fail"

    def check(self):
        if self.outcome:
            raise Stop

    def refused(self, why):
        log(why)
        self.outcome = self.outcome or "fail"
        raise Stop

    def value(self, value):
        """A step's value, each text "@alias" (at any depth) the alias's
        address, #_scxml_<session>."""
        if isinstance(value, str) and value.startswith("@") and value[1:] in self.aliases:
            return f"#_scxml_{self.aliases[value[1:]]}"
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, dict):
            return {key: self.value(item) for key, item in value.items()}
        return value

    # ---- steps -----------------------------------------------------------------------

    def image(self, step, names):
        if not isinstance(names, list) or not names or any(name not in self.documents for name in names):
            raise Refused(INVALID_PARAMS, "type", f"params.steps[{step}]", "image names documents of the call")
        try:
            self.host.add_image({name: self.documents[name] for name in names}, self.files)
        except tinyactors.TinyactorsError as error:
            for diagnostic in getattr(error, "diagnostics", []):
                log(f"{diagnostic[4]}:{diagnostic[2]}: {diagnostic[5]}")
            self.outcome = self.outcome or "compile"
            raise Stop from None

    def spawn(self, step, alias, operation):
        try:
            session = self.host.spawn(operation["chart"], self.value(operation.get("params")), alias)
        except tinyactors.TinyactorsError as error:
            self.refused(f"spawn {alias}: {error}")
        self.aliases[alias] = session

    def event(self, item, to):
        if item.get(to) not in self.aliases:
            self.refused(f"a send to an alias no spawn made: {item.get(to)!r}")
        return (self.aliases[item[to]], item["event"], self.value(item.get("data")))

    def send(self, step, alias, operation):
        session, event, data = self.event(operation, "send")
        try:
            self.host.send(session, event, data)
        except tinyactors.TinyactorsError as error:
            self.refused(f"send {alias} {event}: {error}")

    def send_many(self, step, items):
        events = [self.event(item, "to") for item in items]
        try:
            taken = self.host.send_many(events)
        except tinyactors.TinyactorsError as error:
            self.refused(f"sendMany: {error}")
        if taken != len(events):
            self.refused(f"sendMany: {taken} of {len(events)} taken")

    def advance(self, step, ticks):
        if not isinstance(ticks, int):
            raise Refused(INVALID_PARAMS, "type", f"params.steps[{step}]", "advance takes ticks")
        self.clock.increment(ticks)  # the config's ticksPerSecond is 1000

    def run_step(self, index, step):
        if not isinstance(step, dict):
            raise Refused(INVALID_PARAMS, "type", f"params.steps[{index}]", "a step is a map")
        if "image" in step:
            self.image(index, step["image"])
        elif "spawn" in step:
            self.spawn(index, step["spawn"], step)
        elif "send" in step:
            self.send(index, step["send"], step)
        elif "sendMany" in step:
            self.send_many(index, step["sendMany"])
        elif "advance" in step:
            self.advance(index, step["advance"])
        elif any(kind in step for kind in ("hibernate", "snapshot", "restore", "migrate")):
            self.refused(f"step {index}: {next(iter(step))} is a later milestone's (ROADMAP.md)")
        else:
            raise Refused(INVALID_PARAMS, "type", f"params.steps[{index}]", "not a step of driver.md")
        self.check()

    def run(self, steps):
        with tinyactors.Host(CONFIG, clock=self.clock, on_log=self.on_log) as self.host:
            try:
                for index, step in enumerate(steps):
                    self.run_step(index, step)
            except Stop:
                pass
            except tinyactors.TinyactorsError as error:
                log(f"a turn raised: {error}")
                self.outcome = self.outcome or "fail"
        return self.outcome or "timeout"


def run(vectors, params):
    p.check(params, ("documents", "steps", "datamodel", "realm"))
    documents = p.get(params, "documents", list, required=True)
    steps = p.get(params, "steps", list, required=True)
    datamodel_of(params)
    for index, document in enumerate(documents):
        if not isinstance(document, dict) or not isinstance(document.get("name"), str) \
                or not isinstance(document.get("text"), str):
            raise Refused(INVALID_PARAMS, "type", f"params.documents[{index}]", "a document is a map of name and text")
    return {"outcome": Scenario(documents).run(steps)}
