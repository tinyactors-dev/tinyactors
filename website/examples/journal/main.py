import asyncio
import json

import tinyactors

journal = []


def apply(host, entry):
    if "spawn" in entry:
        host.spawn(entry["spawn"], name=entry["name"])
    else:
        host.send(entry["send"])


def record(host, entry):
    """Every input goes through record(): the host's time, then the input."""
    line = json.dumps({"at": host.now(), **entry})
    journal.append(line)  # in production: append to a file or a table first
    apply(host, json.loads(line))


class Payments:
    def __init__(self, charged):
        self.charged = charged

    def receive(self, send):
        self.charged.append(send.data["amount"])


class Nobody:
    def receive(self, send):  # charges nobody
        pass


async def main():
    with open("order.scxml") as file:
        chart = file.read()
    with tinyactors.compile_charts([("order.scxml", chart)]) as compiled:
        image = compiled.image

    order = {"target": "tinyactors://shop/@order-1"}
    clock = tinyactors.SimulatedClock()
    charged = []
    async with tinyactors.Host(image, system="shop", clock=clock,
                               io_processors={"payments": Payments(charged)}) as live:
        record(live, {"spawn": "order", "name": "order-1"})
        record(live, {"send": {**order, "event": "add", "data": {"sku": "pen"}}})
        record(live, {"send": {**order, "event": "add", "data": {"sku": "notebook"}}})
        record(live, {"send": {**order, "event": "checkout"}})
        await live.idle()
        clock.increment(30)  # no answer: the chart charges again
        await live.idle()
        clock.increment(5)
        record(live, {"send": {**order, "event": "paid"}})  # the provider's webhook
        await live.idle()
        hosts = [("live", live)]

        replay_clock = tinyactors.SimulatedClock()
        async with tinyactors.Host(image, system="shop", clock=replay_clock,
                                   io_processors={"payments": Nobody()}) as replay:
            for line in journal:
                entry = json.loads(line)
                replay_clock.increment(entry["at"] - replay.now())
                await replay.idle()
                apply(replay, entry)
                await replay.idle()
            hosts.append(("replay", replay))

            # Go on from the replay on the runtime's clock, with the real processor.
            async with tinyactors.Host.restore(replay.snapshot(), image, system="shop",
                                               io_processors={"payments": Payments(charged)}) as restored:
                hosts.append(("restored", restored))
                for name, host in hosts:
                    actor = host.actor("order-1")
                    value = (await actor.state())["value"]
                    variables = (await actor.context())["variables"]
                    print(f'{name}: {json.dumps(value)} after {variables["attempts"]} attempts, '
                          f'{", ".join(variables["items"])}')
                print(f'{len(journal)} inputs journaled; charges: {", ".join(str(c) for c in charged)}')


asyncio.run(main())
