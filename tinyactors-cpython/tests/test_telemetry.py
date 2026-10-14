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


if __name__ == "__main__":
    unittest.main()
