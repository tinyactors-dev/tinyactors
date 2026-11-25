import asyncio
import json

import tinyactors


class Payments:
    charges = 0

    def receive(self, send):        # the chart's <send type="payments">
        print("charge", json.dumps(send.data, separators=(",", ":")))
        self.charges += 1
        if self.charges == 1:
            send.fail()             # down for the first charge: the order gets error.communication
        else:
            send.answer("charged")  # an event back to the order


async def main():
    with open("order.scxml") as file:
        chart = file.read()
    with tinyactors.compile_charts([("order.scxml", chart)]) as compiled:
        host = tinyactors.Host(compiled, system="shop",
                               io_processors={"payments": Payments()},
                               log=lambda entry: print("log", entry.text))
    async with host:
        order = host.spawn("order")
        order.send("add", {"item": "notebook", "price": 30})
        order.send("add", {"item": "pen", "price": 12})
        order.send("checkout")
        print("paid", await order.done)  # the order retried on its own


asyncio.run(main())
