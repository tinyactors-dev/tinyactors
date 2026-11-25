import tinyactors


class Payments:
    def receive(self, send):        # the chart's <send type="payments">
        print("charge", send.data["amount"])
        send.answer("charged")      # an event back to the order


with open("order.scxml") as file:
    chart = file.read()
with tinyactors.compile_charts([("order.scxml", chart)]) as compiled:
    host = tinyactors.Host(compiled, system="shop",
                           io_processors={"payments": Payments()},
                           on_done=lambda actor, donedata: print("paid", donedata["total"]))

with host:
    order = host.spawn("order")
    order.send("add", {"price": 30})
    order.send("add", {"price": 12})
    order.send("checkout")
    host.run_until_idle()
