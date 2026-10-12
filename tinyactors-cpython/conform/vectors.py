"""The methods the protocol vectors use (driver.md, from host.create on):
one Host between calls, each method one call of the binding's API.

The host runs on a SimulatedClock: its time moves only with clock.advance,
as the vectors' setups expect, and every input runs the turn at the
current time, so host.run returns the envelopes the turns since the last
host.run wrote (on_peer, in order)."""

import tinyactors

from . import params as p
from .frames import INVALID_PARAMS, STATE, Refused

DATAMODELS = {"quickjs": "ecmascript", "native": "python"}


def datamodel_of(params):
    """The datamodel a call names (driver.md, "Datamodels"): its name, as
    host.create answers it. Every Host holds both; a chart runs on the one
    it names."""
    name = p.get(params, "datamodel", str, "quickjs")
    realm = p.get(params, "realm", str, "auto")
    if name not in DATAMODELS:
        raise Refused(INVALID_PARAMS, "range", "params.datamodel", f"no datamodel {name!r}")
    if realm != "auto":
        raise Refused(INVALID_PARAMS, "range", "params.realm", "the binding offers quickjs's default realm only")
    return DATAMODELS[name]


def refusal(error):
    """A refused config as the error core wrote, from the TinyactorsError
    that carries it unchanged (docs/errors.md)."""
    return tinyactors.encode({"error": {"code": error.code, "message": str(error),
                                        "data": {"reason": error.reason, "path": error.path}}})


def status_of(error):
    return error.status if error.status is not None else tinyactors.ERROR_INVALID


class Vectors:
    def __init__(self):
        self.host = None
        self.clock = None
        self.ticks_per_second = 1000
        self.now = 0
        self.envelopes = []

    def close(self):
        if self.host:
            self.host.close()
        self.host = None

    def host_of(self):
        if not self.host:
            raise Refused(STATE, "state", "", "no host: call host.create first")
        return self.host

    def create(self, params):
        p.check(params, ("config", "datamodel", "realm"))
        if self.host:
            raise Refused(STATE, "state", "", "a host already: call host.free first")
        config = p.get(params, "config", bytes, required=True)
        name = datamodel_of(params)
        try:
            value = tinyactors.decode(config)
        except tinyactors.TinyactorsError:
            # ROADMAP.md, request 1: bytes that are not one item, which no
            # config value of the API can be
            raise Refused(INVALID_PARAMS, "type", "params.config", "not a config the API can take") from None
        system = value.get("system") if isinstance(value, dict) else None
        ticks = system.get("ticksPerSecond") if isinstance(system, dict) else None
        self.ticks_per_second = ticks if isinstance(ticks, int) and ticks > 0 else 1000
        self.clock, self.now, self.envelopes = tinyactors.SimulatedClock(), 0, []
        try:
            self.host = tinyactors.Host(value, clock=self.clock, on_peer=self.on_peer)
        except tinyactors.TinyactorsError as error:
            if error.code is None:
                return {"status": status_of(error)}
            return {"status": status_of(error), "error": refusal(error)}
        return {"status": 0, "datamodel": name}

    def on_peer(self, peer, envelope):
        self.envelopes.append(envelope)

    def free(self, params):
        p.check(params, ())
        self.host_of()
        self.close()
        return {}

    def request(self, params):
        p.check(params, ("request", "capacity"))
        host = self.host_of()
        data = p.get(params, "request", bytes, required=True)
        try:
            request = tinyactors.decode(data)
        except tinyactors.TinyactorsError:
            request = None
        # ROADMAP.md, request 1: what request(method, params) cannot send
        if not isinstance(request, dict) or set(request) - {"method", "params", "id"} or "method" not in request \
                or "id" not in request or not isinstance(request["id"], (int, str, type(None))) \
                or isinstance(request["id"], bool):
            raise Refused(INVALID_PARAMS, "range", "params.request", "a request the binding's API cannot send")
        try:
            reply = {"id": request["id"], "result": host.request(request["method"], request.get("params"))}
        except tinyactors.TinyactorsError as error:
            if error.code is None:
                return {"status": status_of(error), "length": 0}
            reply = {"id": request["id"], "error": {"code": error.code, "message": str(error),
                                                    "data": {"reason": error.reason, "path": error.path}}}
        data = tinyactors.encode(reply)
        return {"status": 0, "length": len(data), "reply": data}

    def image(self, params):
        p.check(params, ("charts",))
        host = self.host_of()
        charts = p.get(params, "charts", list, required=True)
        try:
            segment = host.add_image({chart["name"]: chart["text"] for chart in charts})
        except tinyactors.TinyactorsError as error:
            return {"status": status_of(error)}
        return {"status": 0, "segment": segment}

    def spawn(self, params):
        p.check(params, ("chart", "params"))
        host = self.host_of()
        chart = p.get(params, "chart", str, required=True)
        try:
            session = host.spawn(chart, params.get("params"))
        except tinyactors.TinyactorsError as error:
            return {"status": status_of(error)}
        return {"status": 0, "session": session}

    def send(self, params):
        p.check(params, ("session", "event", "data"))
        host = self.host_of()
        session = p.get(params, "session", int, required=True)
        event = p.get(params, "event", str, required=True)
        try:
            host.send(session, event, params.get("data"))
        except tinyactors.TinyactorsError as error:
            return {"status": status_of(error)}
        return {"status": 0}

    def advance(self, params):
        """The monotonic clock to `monotonic` ticks: increment by the
        milliseconds between (the host's config gives its tick rate)."""
        p.check(params, ("monotonic", "wallNs"))
        self.host_of()
        monotonic = p.get(params, "monotonic", int, 0)
        p.get(params, "wallNs", int, 0)
        if monotonic > self.now:
            self.clock.increment((monotonic - self.now) * 1000 // self.ticks_per_second)
            self.now = monotonic
        return {}

    def run(self, params):
        p.check(params, ())
        self.host_of().run()
        envelopes, self.envelopes = self.envelopes, []
        return {"status": 0, "envelopes": envelopes}

    def peer(self, params):
        p.check(params, ("id", "trusted"))
        host = self.host_of()
        try:
            peer = host.add_peer(p.get(params, "id", str, required=True), trusted=p.get(params, "trusted", bool, False))
        except tinyactors.TinyactorsError as error:
            return {"status": status_of(error)}
        return {"status": 0, "peer": peer}

    def deliver(self, params):
        p.check(params, ("peer", "cbor"))
        host = self.host_of()
        peer = p.get(params, "peer", int, required=True)
        envelope = p.get(params, "cbor", bytes, required=True)
        try:
            route = host.deliver(peer, envelope)
        except tinyactors.TinyactorsError as error:
            return {"status": status_of(error)}
        return {"status": 0, "route": route}

    METHODS = {
        "host.create": create,
        "host.free": free,
        "host.request": request,
        "image.add": image,
        "actor.spawn": spawn,
        "actor.send": send,
        "clock.advance": advance,
        "host.run": run,
        "peer.add": peer,
        "envelope.deliver": deliver,
    }
