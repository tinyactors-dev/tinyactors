import json

import tinyactors


class Payments:
    up = False                      # the payment service is down at first

    def receive(self, send):        # the chart's <send type="payments">
        print("charge", json.dumps(send.data, separators=(",", ":")))
        if self.up:
            send.answer("charged")  # an event back to the order
        else:
            send.fail()             # the order gets error.communication


payments = Payments()
with open("order.scxml") as file:
    chart = file.read()
with tinyactors.compile_charts([("order.scxml", chart)]) as compiled:
    host = tinyactors.Host(compiled, system="shop",
                           io_processors={"payments": payments},
                           log=lambda entry: print("log", entry.text),
                           on_done=lambda actor, donedata: print("paid", donedata))

with host:
    order = host.spawn("order")
    order.send("add", {"item": "notebook", "price": 30})
    order.send("add", {"item": "pen", "price": 12})
    order.send("checkout")
    host.run_until_idle()
    payments.up = True
    order.send("checkout")          # the cart kept its lines: check out again
    host.run_until_idle()
