"""Telemetry: the OTLP requests the host's encoder writes, handed to the
application's handler as bytes, metrics pulled, and the last take at close."""

import asyncio
import unittest

import tinyactors
from support import chart

DOCUMENT = chart("<state id='s'><transition event='go'><log label='l' expr=\"'hi'\"/>"
                 "<assign location='n' expr='n + 1'/></transition></state>", data="<data id='n' expr='0'/>")


def config(**telemetry):
    return {"systemID": "t", "system": {"ticksPerSecond": 1000}, "telemetry": dict({"flushInterval": 100}, **telemetry)}


def host_with(handler, clock=None, **telemetry):
    host = tinyactors.Host(config(**telemetry), telemetry=handler, clock=clock or tinyactors.SimulatedClock())
    with tinyactors.compile_charts([("c.scxml", DOCUMENT)]) as compiled:
        host.add_image(compiled.image)
    return host


def span_names(host, requests):
    names = []
    for signal, data in requests:
        if signal == "traces":
            host.trace_spans_decode(lambda host, result: names.extend(span["name"] for span in result["spans"]),
                                    traces=data)
    host.run_until_idle()
    return names


class Telemetry(unittest.TestCase):
    def test_traces_and_logs_reach_the_handler_as_bytes(self):
        got = []
        clock = tinyactors.SimulatedClock()
        host = host_with(lambda host, signal, data: got.append((signal, data)), clock)
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        self.assertEqual(got, [])  # staged until flushInterval passed
        clock.increment(200)
        self.assertEqual(sorted(signal for signal, _ in got), ["logs", "traces"])
        self.assertTrue(all(isinstance(data, bytes) and data for _, data in got))
        self.assertIn("scxml.macrostep go", span_names(host, got))
        host.close()

    def test_close_hands_over_the_last_take(self):
        got = []
        host = host_with(lambda host, signal, data: got.append((signal, data)), flushInterval=0)
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        self.assertEqual(got, [])  # no flush interval: only a full staging or the stop
        host.close()
        self.assertIn("traces", [signal for signal, _ in got])
        decoder = tinyactors.Host({"systemID": "d"})
        self.assertIn("scxml.macrostep go", span_names(decoder, got))
        decoder.close()

    def test_metrics_are_pulled(self):
        host = host_with(None)
        self.assertTrue(host.metrics())
        self.assertTrue(host.metrics())  # the batch of the first was acknowledged
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()  # and nothing was left open for the run
        host.close()
        plain = tinyactors.Host({"systemID": "t"})
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            plain.metrics()
        self.assertEqual(raised.exception.status, "TA_ERROR_STATE")
        plain.close()

    def test_metrics_from_a_handler_join_the_batch(self):
        got = []

        def handler(host, signal, data):
            got.append(len(host.metrics()))

        clock = tinyactors.SimulatedClock()
        host = host_with(handler, clock)
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        clock.increment(200)
        self.assertTrue(got and all(got))
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        host.close()

    def test_a_handler_that_raises_is_raised_after_the_turn(self):
        def handler(host, signal, data):
            raise ConnectionError("collector down")

        clock = tinyactors.SimulatedClock()
        host = host_with(handler, clock)
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        with self.assertRaises(ConnectionError):
            clock.increment(200)
        host.send(host.spawn(host.chart("c")), "go")  # the encoder goes on: the request was taken
        host.run_until_idle()
        with self.assertRaises(ConnectionError):  # the last take at close, raised after the host is freed
            host.close()
        self.assertTrue(host.closed)

    def test_an_async_handler_runs_as_a_task(self):
        got = []

        async def handler(host, signal, data):
            await asyncio.sleep(0)
            got.append(signal)

        async def main():
            async with tinyactors.Host(config(flushInterval=0), telemetry=handler) as host:
                with tinyactors.compile_charts([("c.scxml", DOCUMENT)]) as compiled:
                    host.add_image(compiled.image)
                host.send(host.spawn(host.chart("c")), "go")
                await host.idle()
            for _ in range(5):
                await asyncio.sleep(0)

        asyncio.run(main())
        self.assertIn("traces", got)  # the last take, at the exit of async with


class Stopping(unittest.TestCase):
    """The last take at close, when its handler calls the host again."""

    def staged(self, handler):
        host = host_with(handler, flushInterval=0)
        host.send(host.spawn(host.chart("c")), "go")
        host.run_until_idle()
        return host

    def test_close_from_the_handler_of_the_last_take(self):
        got = []

        def handler(host, signal, data):
            got.append(signal)
            host.close()  # deferred: the close running now finishes

        host = self.staged(handler)
        host.close()
        self.assertTrue(host.closed)
        self.assertIn("traces", got)

    def test_inputs_from_the_handler_of_the_last_take_end(self):
        calls = []

        def handler(host, signal, data):
            calls.append(signal)
            host.send(host.spawn(host.chart("c")), "go")  # more records, at every take

        host = self.staged(handler)
        host.close()
        self.assertTrue(host.closed)
        self.assertLess(len(calls), 200)  # the last take is bounded

    def test_metrics_from_the_handler_of_the_last_take(self):
        sizes = []
        host = self.staged(lambda host, signal, data: sizes.append(len(host.metrics())))
        host.close()
        self.assertTrue(sizes and all(sizes))

    def test_a_close_a_handler_asked_for_raises_from_the_driver(self):
        state = {"closing": False}

        def handler(host, signal, data):
            if state["closing"]:
                raise ConnectionError("down at close")

        def log(host, entry):
            state["closing"] = True
            host.close()

        host = tinyactors.Host(config(flushInterval=0), telemetry=handler, log=log, clock=tinyactors.SimulatedClock())
        with tinyactors.compile_charts([("c.scxml", DOCUMENT)]) as compiled:
            host.add_image(compiled.image)
        host.send(host.spawn(host.chart("c")), "go")
        with self.assertRaises(ConnectionError):
            host.run_until_idle()
        self.assertTrue(host.closed)

    def test_async_with_awaits_the_async_last_take(self):
        got = []

        async def handler(host, signal, data):
            await asyncio.sleep(0.01)
            got.append(signal)
            if signal == "logs":
                raise ConnectionError("down")

        async def main():
            with self.assertRaises(ConnectionError):
                async with tinyactors.Host(config(flushInterval=0), telemetry=handler) as host:
                    with tinyactors.compile_charts([("c.scxml", DOCUMENT)]) as compiled:
                        host.add_image(compiled.image)
                    host.send(host.spawn(host.chart("c")), "go")
                    await host.idle()

        asyncio.run(main())  # nothing after the async with: the tasks were awaited by it
        self.assertEqual(sorted(got), ["logs", "traces"])


TWO = [("a.scxml", chart("<state id='s'><transition event='go'/></state>", name="a")),
       ("b.scxml", chart("<state id='s'><transition event='go'/></state>", name="b"))]


def charts_traced(host, requests):
    """The charts whose `go` macrosteps the traces requests hold."""
    traced = []
    for signal, data in requests:
        if signal == "traces":
            host.trace_spans_decode(lambda host, result: traced.extend(
                span["attributes"].get("tinyactors.chart.name") for span in result["spans"]
                if span["name"] == "scxml.macrostep go"), traces=data)
    host.run_until_idle()
    requests.clear()
    return sorted(traced)


class Watches(unittest.TestCase):
    def test_a_watch_traces_what_it_matches_until_it_ends(self):
        got, replies = [], []
        clock = tinyactors.SimulatedClock()
        host = tinyactors.Host(config(flushInterval=10), telemetry=lambda host, signal, data: got.append((signal, data)),
                               clock=clock)
        with tinyactors.compile_charts(TWO) as compiled:
            host.add_image(compiled.image)
        a, b = host.spawn(host.chart("a")), host.spawn(host.chart("b"))

        def step():
            host.send(a, "go")
            host.send(b, "go")
            host.run_until_idle()
            clock.increment(20)  # past flushInterval: the requests come
            return charts_traced(host, got)

        host.trace_configure(lambda host, result: replies.append(result), sample=0)  # nothing is sampled but what a watch keeps
        host.trace_watch(lambda host, result: replies.append(result),
                         rules=[{"chart": "b", "sample": "sample", "detail": "full"}], seconds=1)
        host.run_until_idle()
        self.assertEqual(replies[-1], {"id": 1})
        self.assertEqual(step(), ["b"])
        clock.increment(1000)  # the watch's end: a deadline of the host's clock
        self.assertEqual(step(), [])
        host.trace_watch(lambda host, result: replies.append(result), rules=[{"chart": "a", "sample": "sample"}],
                         seconds=None)
        host.run_until_idle()
        self.assertEqual(step(), ["a"])
        host.trace_unwatch(lambda host, result: replies.append(result), id=replies[-1]["id"])
        host.run_until_idle()
        self.assertEqual(step(), [])
        errors = []
        host.trace_unwatch(print, lambda host, error: errors.append(error.reason), id=2)
        host.run_until_idle()
        self.assertEqual(errors, ["stale"])
        host.close()

    def test_a_watch_ends_on_the_real_clock_under_asyncio(self):
        got = []

        async def main():
            config = {"systemID": "t", "telemetry": {"flushInterval": 1_000_000}}  # 1 ms: ticks are ns by default
            async with tinyactors.Host(config, telemetry=lambda host, signal, data: got.append((signal, data))) as host:
                with tinyactors.compile_charts(TWO) as compiled:
                    host.add_image(compiled.image)

                async def settle():  # the host's own timer flushes what is staged
                    for _ in range(10):
                        await asyncio.sleep(0.005)
                        await host.idle()

                a = host.spawn(host.chart("a"))
                host.trace_configure(lambda host, result: None, sample=0)
                host.trace_watch(lambda host, result: None, rules=[{"chart": "a", "sample": "sample"}], seconds=0.1)
                await settle()
                got.clear()
                host.send(a, "go")
                await settle()
                during = charts_traced(host, got)
                await asyncio.sleep(0.1)  # past the watch's end, a deadline of the host's timer
                await settle()
                host.send(a, "go")
                await settle()
                return during, charts_traced(host, got)

        during, after = asyncio.run(main())
        self.assertEqual((during, after), (["a"], []))


if __name__ == "__main__":
    unittest.main()
