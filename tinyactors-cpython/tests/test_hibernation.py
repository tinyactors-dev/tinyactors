"""Hibernation: sweep, hibernate, the WAKE handler and wake, on both datamodels."""

import asyncio
import gc
import unittest

import tinyactors
from support import APP, Logs, chart, host_with

COUNTER = {
    "python": chart(
        "<state id='s'>"
        "<transition event='inc'><assign location='n' expr='n + 1'/><assign location='items' expr='items + [_event.data]'/></transition>"
        "<transition event='report'><log label='n' expr=\"f'{n} {items} {_event.name}'\"/></transition>"
        "<transition event='last'><log label='last' expr='str(_event.data)'/></transition>"
        "<transition event='wait'><send event='later' delay='1s'/></transition>"
        "</state>", name="counter", data="<data id='n' expr='0'/><data id='items' expr='[]'/>"),
    "ecmascript": chart(
        "<state id='s'>"
        "<transition event='inc'><assign location='n' expr='n + 1'/><assign location='items' expr='items.concat([_event.data])'/></transition>"
        "<transition event='report'><log label='n' expr=\"n + ' ' + JSON.stringify(items) + ' ' + _event.name\"/></transition>"
        "<transition event='last'><log label='last' expr='JSON.stringify(_event.data)'/></transition>"
        "<transition event='wait'><send event='later' delay='1s'/></transition>"
        "</state>", name="counter", datamodel="ecmascript", data="<data id='n' expr='0'/><data id='items' expr='[]'/>"),
}


class Store:
    """A WAKE handler over a dict, as an application's store would be."""

    def __init__(self):
        self.blobs = {}
        self.wakes = []

    def __call__(self, host, session):
        self.wakes.append(session)
        host.wake(session, self.blobs.pop(session))


class Hibernation(unittest.TestCase):
    def roundtrip(self, datamodel):
        logs, store = Logs(), Store()
        with host_with([("counter.scxml", COUNTER[datamodel])], log=logs, wake=store) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", {"k": 1})
            host.send(session, "inc", "two")
            host.run_until_idle()
            blob = host.hibernate(session)
            self.assertIsInstance(blob, bytes)
            store.blobs[session] = blob
            host.send(session, "report")
            host.run_until_idle()
            self.assertEqual(store.wakes, [session])
        return logs.labelled("n")

    def test_a_python_actor_wakes_with_its_variables(self):
        self.assertEqual(self.roundtrip("python"), ["2 [{'k': 1}, 'two'] report"])

    def test_an_ecmascript_actor_wakes_with_its_variables(self):
        self.assertEqual(self.roundtrip("ecmascript"), ['2 [{"k":1},"two"] report'])

    def test_an_actor_wakes_with_its_last_event(self):
        """_event stays bound to the last event across hibernation (SCXML
        5.10): the blob carries it, and the <onexit> of an actor terminated
        at rest reads it."""
        exits = {"python": "<onexit><log label='exit' expr=\"f'{_event.name} {_event.data}'\"/></onexit>",
                 "ecmascript": "<onexit><log label='exit' expr=\"_event.name + ' ' + JSON.stringify(_event.data)\"/></onexit>"}
        for datamodel, expected in (("python", "inc {'k': 7}"), ("ecmascript", 'inc {"k":7}')):
            with self.subTest(datamodel=datamodel):
                logs, store = Logs(), Store()
                document = COUNTER[datamodel].replace("<state id='s'>", "<state id='s'>" + exits[datamodel])
                with host_with([("counter.scxml", document)], log=logs, wake=store) as host:
                    session = host.spawn(host.chart("counter"))
                    host.send(session, "inc", {"k": 7})
                    host.run_until_idle()
                    blob = host.hibernate(session)
                    host.wake(session, blob)
                    host.terminate(session)
                    host.run_until_idle()
                self.assertEqual(logs.labelled("exit"), [expected])

    def test_events_wait_until_the_wake(self):
        """One WAKE per hibernation, however many events come; they run in
        order once the application wakes the actor."""
        logs = Logs()
        wakes = []
        with host_with([("counter.scxml", COUNTER["python"])], log=logs, wake=lambda host, s: wakes.append(s)) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            blob = host.hibernate(session)
            for i in range(3):
                host.send(session, "inc", i)
            host.send(session, "report")
            host.run_until_idle()
            self.assertEqual(wakes, [session])
            self.assertEqual(logs.labelled("n"), [])
            host.wake(session, blob)
            host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["3 [0, 1, 2] report"])

    def test_the_sweep_returns_actors_that_did_not_run_since_the_last_one(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            ref = host.chart("counter")
            sessions = [host.spawn(ref) for _ in range(4)]
            host.run_until_idle()
            host.sweep(1024, 16)  # clears every actor's bit
            host.send(sessions[1], "inc", 0)
            host.run_until_idle()
            cold = host.sweep(1024, 16)
            self.assertEqual(sorted(cold), sorted(sessions[:1] + sessions[2:]))
            self.assertEqual(len(host.sweep(1024, 2)), 2)

    def test_a_delayed_send_wakes_the_actor_it_was_sent_to(self):
        """A pending delayed send goes into the blob; when it falls due the
        host asks for the actor (a WAKE) and delivers it."""
        logs, store = Logs(), Store()
        clock = tinyactors.SimulatedClock()
        document = COUNTER["python"].replace("</state>", "<transition event='later'><log label='later' expr='str(n)'/></transition></state>")
        with host_with([("counter.scxml", document)], log=logs, wake=store, clock=clock) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc", 0)
            host.send(session, "wait")
            host.run_until_idle()
            store.blobs[session] = host.hibernate(session)
            clock.increment(999)
            self.assertEqual(store.wakes, [])
            clock.increment(1)
            self.assertEqual(store.wakes, [session])
        self.assertEqual(logs.labelled("later"), ["1"])

    def test_hibernate_refuses_an_actor_with_events_waiting(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.send(session, "inc", 1)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)
            host.run_until_idle()
            host.hibernate(session)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)  # already out of memory

    def test_a_value_outside_the_table_keeps_the_actor_resident(self):
        logs = Logs()
        document = chart("<state id='s'>"
                         "<transition event='odd'><assign location='v' expr='{1, 2}'/></transition>"
                         "<transition event='report'><log label='v' expr='str(sorted(v))'/></transition></state>",
                         name="odd", data="<data id='v' expr='0'/>")
        with host_with([("odd.scxml", document)], log=logs) as host:
            session = host.spawn(host.chart("odd"))
            host.send(session, "odd")
            host.run_until_idle()
            with self.assertRaises(tinyactors.TinyactorsError):
                host.hibernate(session)
            host.send(session, "report")
            host.run_until_idle()
        self.assertEqual(logs.labelled("v"), ["[1, 2]"])

    def test_a_hibernated_python_actor_holds_no_python_object(self):
        """Its instance (of the chart's class, named after the chart) and
        its values go: the blob is all that is left."""
        def instances():
            gc.collect()
            return sum(1 for each in gc.get_objects() if type(each).__name__ == "counter")

        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            ref = host.chart("counter")
            sessions = [host.spawn(ref) for _ in range(3)]
            for session in sessions:
                host.send(session, "inc", ["kept"])
            host.run_until_idle()
            self.assertEqual(instances(), 3)
            for session in sessions[:2]:
                host.hibernate(session)
            self.assertEqual(instances(), 1)

    def test_wake_refuses_a_blob_it_cannot_read(self):
        with host_with([("counter.scxml", COUNTER["python"])]) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(session)
            with self.assertRaises(tinyactors.TinyactorsError):
                host.wake(session, b"\x00not a blob")

    def test_a_wake_handler_that_raises_is_raised_by_the_driver(self):
        def wake(host, session):
            raise ValueError("store down")

        with host_with([("counter.scxml", COUNTER["python"])], wake=wake) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(session)
            host.send(session, "inc", 1)
            with self.assertRaises(ValueError):
                host.run_until_idle()

    def test_an_async_wake_handler_under_with_raises_type_error(self):
        async def wake(host, session):
            pass

        with self.assertRaisesRegex(TypeError, "wake"):
            with tinyactors.Host({"systemID": "t"}, wake=wake):
                pass

    def test_an_async_wake_handler_runs_as_a_task(self):
        logs, blobs = Logs(), {}

        async def wake(host, session):
            await asyncio.sleep(0)
            host.wake(session, blobs.pop(session))

        async def main():
            config = {"systemID": "t", "ioProcessors": [{"type": APP}]}
            async with tinyactors.Host(config, log=logs, wake=wake) as host:
                with tinyactors.compile_charts([("counter.scxml", COUNTER["python"])]) as compiled:
                    host.add_image(compiled.image)
                session = host.spawn(host.chart("counter"))
                await host.idle()
                blobs[session] = host.hibernate(session)
                host.send(session, "inc", 5)
                host.send(session, "report")
                await host.idle()
                for _ in range(5):
                    await asyncio.sleep(0)
                await host.idle()

        asyncio.run(main())
        self.assertEqual(logs.labelled("n"), ["1 [5] report"])


if __name__ == "__main__":
    unittest.main()
