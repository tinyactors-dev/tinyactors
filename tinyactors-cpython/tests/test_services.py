"""Invoked services: Host(services=...), the invocation record, send_back,
done, receive and stop, on both datamodels."""

import gc
import unittest
import weakref

import tinyactors
from support import Logs, chart, host_with

TYPE = "urn:test:echo"
CLIENT = {
    "python": chart(
        "<state id='s'>"
        "<invoke id='echo' type='urn:test:echo' src='echo://here'><param name='hello' expr=\"'world'\"/></invoke>"
        "<transition event='ask'><send target='#_echo' event='question'><content expr='_event.data'/></send></transition>"
        "<transition event='answer'><log label='answer' expr=\"f'{_event.data} {_event.invokeid}'\"/></transition>"
        "<transition event='done.invoke.echo' target='over'><log label='done' expr='str(_event.data)'/></transition>"
        "<transition event='error.communication'><log label='error' expr=\"'communication'\"/></transition>"
        "<transition event='leave' target='over'/>"
        "</state><state id='over'/>", name="client"),
    "ecmascript": chart(
        "<state id='s'>"
        "<invoke id='echo' type='urn:test:echo' src='echo://here'><param name='hello' expr=\"'world'\"/></invoke>"
        "<transition event='ask'><send target='#_echo' event='question'><content expr='_event.data'/></send></transition>"
        "<transition event='answer'><log label='answer' expr=\"JSON.stringify(_event.data) + ' ' + _event.invokeid\"/></transition>"
        "<transition event='done.invoke.echo' target='over'><log label='done' expr='JSON.stringify(_event.data)'/></transition>"
        "<transition event='error.communication'><log label='error' expr=\"'communication'\"/></transition>"
        "<transition event='leave' target='over'/>"
        "</state><state id='over'/>", name="client", datamodel="ecmascript"),
}


class Echo:
    """A service: answers every question, finishes on 'finish'."""

    def __init__(self, record, invocation):
        self.record, self.invocation = record, invocation
        record.append(("start", invocation.session, invocation.invokeid, invocation.type, invocation.src,
                       invocation.data))

    def receive(self, host, send):
        self.record.append(("receive", send.event, send.data, send.type))
        if send.data == "finish":
            self.invocation.done({"asked": len([r for r in self.record if r[0] == "receive"])})
        else:
            self.invocation.send_back("answer", {"echo": send.data})

    def stop(self, host):
        self.record.append(("stop", self.invocation.session))


class Services(unittest.TestCase):
    def talk(self, datamodel):
        logs, record = Logs(), []
        invocations = []

        def start(host, invocation):
            invocations.append(invocation)
            return Echo(record, invocation)

        with host_with([("client.scxml", CLIENT[datamodel])], log=logs, services={TYPE: start}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.send(session, "ask", "why")
            host.run_until_idle()
            host.send(session, "ask", "finish")
            host.run_until_idle()
            self.assertFalse(invocations[0].running)
            with self.assertRaises(tinyactors.TinyactorsError) as raised:
                invocations[0].send_back("late")
            self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
        self.assertEqual(record[0], ("start", session, "echo", TYPE, "echo://here", {"hello": "world"}))
        self.assertEqual(record[1], ("receive", "question", "why", None))
        self.assertNotIn("stop", [r[0] for r in record])  # done, not cancelled
        return logs

    def test_a_python_actor_talks_to_its_service(self):
        logs = self.talk("python")
        self.assertEqual(logs.labelled("answer"), ["{'echo': 'why'} echo"])
        self.assertEqual(logs.labelled("done"), ["{'asked': 2}"])

    def test_an_ecmascript_actor_talks_to_its_service(self):
        logs = self.talk("ecmascript")
        self.assertEqual(logs.labelled("answer"), ['{"echo":"why"} echo'])
        self.assertEqual(logs.labelled("done"), ['{"asked":2}'])

    def test_leaving_the_invoking_state_stops_the_service(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                record, invocations = [], []

                def start(host, invocation):
                    invocations.append(invocation)
                    return Echo(record, invocation)

                with host_with([("client.scxml", CLIENT[datamodel])], services={TYPE: start}) as host:
                    session = host.spawn(host.chart("client"))
                    host.run_until_idle()
                    host.send(session, "leave")
                    host.run_until_idle()
                    self.assertFalse(invocations[0].running)
                    with self.assertRaises(tinyactors.TinyactorsError):
                        invocations[0].send_back("late")
                self.assertEqual([r for r in record if r[0] == "stop"], [("stop", session)])

    def test_terminating_the_invoker_stops_the_service(self):
        record = []
        with host_with([("client.scxml", CLIENT["python"])], services={TYPE: lambda host, i: Echo(record, i)}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.terminate(session)
            host.run_until_idle()
        self.assertEqual([r for r in record if r[0] == "stop"], [("stop", session)])

    def test_a_service_needs_neither_receive_nor_stop(self):
        logs, kept = Logs(), []

        def start(host, invocation):
            kept.append(invocation)
            invocation.send_back("answer", "hi")
            return None

        with host_with([("client.scxml", CLIENT["python"])], log=logs, services={TYPE: start}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.send(session, "ask", "dropped")  # nothing receives it
            host.send(session, "leave")
            host.run_until_idle()
        self.assertEqual(logs.labelled("answer"), ["hi echo"])

    def test_no_service_for_the_type_is_a_communication_error(self):
        logs = Logs()
        with host_with([("client.scxml", CLIENT["python"])], log=logs) as host:
            host.spawn(host.chart("client"))
            with self.assertRaisesRegex(tinyactors.TinyactorsError, "no service is registered"):
                host.run_until_idle()
            host.run_until_idle()
        self.assertEqual(logs.labelled("error"), ["communication"])

    def test_a_handler_that_raises_is_a_communication_error(self):
        logs = Logs()

        def start(host, invocation):
            raise ValueError("cannot start")

        with host_with([("client.scxml", CLIENT["python"])], log=logs, services={TYPE: start}) as host:
            host.spawn(host.chart("client"))
            with self.assertRaises(ValueError):
                host.run_until_idle()
            host.run_until_idle()
        self.assertEqual(logs.labelled("error"), ["communication"])

    def test_services_left_at_the_stop_are_dropped_not_stopped(self):
        record, alive = [], []

        def start(host, invocation):
            service = Echo(record, invocation)
            alive.append(weakref.ref(service))
            return service

        with host_with([("client.scxml", CLIENT["python"])], services={TYPE: start}) as host:
            host.spawn(host.chart("client"))
            host.run_until_idle()
        gc.collect()
        self.assertNotIn("stop", [r[0] for r in record])
        self.assertIsNone(alive[0]())

    def test_autoforward_reaches_the_service(self):
        document = chart("<state id='s'><invoke id='echo' type='urn:test:echo' src='echo://here' autoforward='true'/>"
                         "<transition event='answer'><log label='answer' expr='str(_event.data)'/></transition>"
                         "</state>", name="client")
        logs, record = Logs(), []

        class Forwarded:  # every external event of the invoker reaches it, its own answers too
            def __init__(self, invocation):
                self.invocation = invocation

            def receive(self, host, send):
                record.append((send.event, send.data))
                if send.event == "hello":
                    self.invocation.send_back("answer", {"echo": send.data})

        with host_with([("client.scxml", document)], log=logs, services={TYPE: lambda host, i: Forwarded(i)}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.send(session, "hello", "there")
            host.run_until_idle()
        self.assertEqual(record, [("hello", "there"), ("answer", {"echo": "there"})])
        self.assertEqual(logs.labelled("answer"), ["{'echo': 'there'}"])

    def test_a_service_done_in_its_handler_is_not_kept_nor_stopped(self):
        record, alive = [], []

        def start(host, invocation):
            service = Echo(record, invocation)
            alive.append(weakref.ref(service))
            invocation.done({"at": "start"})
            return service

        logs = Logs()
        with host_with([("client.scxml", CLIENT["python"])], log=logs, services={TYPE: start}) as host:
            host.spawn(host.chart("client"))
            host.run_until_idle()
            gc.collect()
            self.assertIsNone(alive[0]())  # the host does not hold it
        self.assertNotIn("stop", [r[0] for r in record])
        self.assertEqual(logs.labelled("done"), ["{'at': 'start'}"])

    def test_a_send_to_a_service_that_failed_to_start_fails(self):
        logs = Logs()

        def start(host, invocation):
            raise ValueError("cannot start")

        with host_with([("client.scxml", CLIENT["python"])], log=logs, services={TYPE: start}) as host:
            session = host.spawn(host.chart("client"))
            with self.assertRaises(ValueError):
                host.run_until_idle()
            host.send(session, "ask", "anyone")
            host.run_until_idle()
            host.send(session, "leave")
            host.run_until_idle()
        self.assertEqual(logs.labelled("error"), ["communication", "communication"])

    def test_a_failure_to_start_an_invocation_cancelled_already_is_not_reported(self):
        document = chart("<state id='s'><invoke id='echo' type='urn:test:echo' src='echo://here'/>"
                         "<transition event='leave' target='over'/></state>"
                         "<state id='over'><transition event='error.communication'>"
                         "<log label='error' expr=\"'late'\"/></transition></state>", name="client")
        logs = Logs()

        def start(host, invocation):
            raise ValueError("cannot start")

        with host_with([("client.scxml", document)], log=logs, services={TYPE: start}) as host:
            session = host.spawn(host.chart("client"))
            host.send(session, "leave")  # INVOKE and UNINVOKE in one batch
            with self.assertRaises(ValueError):
                host.run_until_idle()
            host.run_until_idle()
        self.assertEqual(logs.labelled("error"), [])

    def snapshot_with_a_service(self, datamodel):
        record = []
        with host_with([("client.scxml", CLIENT[datamodel])], services={TYPE: lambda host, i: Echo(record, i)}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.send(session, "ask", "before")
            host.run_until_idle()
            return session, host.snapshot()

    def restore(self, blob, **options):
        config = {"systemID": "t", "system": {"ticksPerSecond": 1000}, "ioProcessors": [{"type": "app", "sync": False}]}
        options.setdefault("clock", tinyactors.SimulatedClock())
        return tinyactors.Host.restore(blob, config, **options)

    def test_a_restore_starts_each_running_service_again(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                session, blob = self.snapshot_with_a_service(datamodel)
                logs, record, invocations = Logs(), [], []

                def start(host, invocation):
                    invocations.append(invocation)
                    return Echo(record, invocation)

                with self.restore(blob, log=logs, services={TYPE: start}) as host:
                    host.run_until_idle()
                    host.send(session, "ask", "after")
                    host.run_until_idle()
                    host.send(session, "ask", "finish")
                    host.run_until_idle()
                self.assertEqual(len(invocations), 1)
                self.assertTrue(invocations[0].restored)
                self.assertEqual(record[0], ("start", session, "echo", TYPE, "echo://here", {"hello": "world"}))
                self.assertEqual(record[1][:3], ("receive", "question", "after"))
                self.assertEqual(len(logs.labelled("answer")), 1)
                self.assertEqual(len(logs.labelled("done")), 1)

    def test_a_restore_with_a_stand_in_service(self):
        session, blob = self.snapshot_with_a_service("python")
        logs = Logs()

        class StandIn:  # a copy must not reach the real service
            def __init__(self, invocation):
                self.invocation = invocation

            def receive(self, host, send):
                self.invocation.send_back("answer", "stand-in")

        with self.restore(blob, log=logs, services={TYPE: lambda host, i: StandIn(i)}) as host:
            host.send(session, "ask", "after")
            host.run_until_idle()
        self.assertEqual(logs.labelled("answer"), ["stand-in echo"])

    def test_a_restore_without_the_service_tells_the_invoker(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                session, blob = self.snapshot_with_a_service(datamodel)
                logs = Logs()
                with self.restore(blob, log=logs) as host:
                    host.run_until_idle()  # no exception: not starting it is the application's choice
                    host.send(session, "ask", "anyone")
                    host.run_until_idle()
                self.assertEqual(logs.labelled("error"), ["communication", "communication"])

    def test_a_service_that_crashes_tells_the_invoker(self):
        logs, stopped = Logs(), []

        class Crashing:
            def receive(self, host, send):
                raise ValueError("crashed")

            def stop(self, host):
                stopped.append(True)

        with host_with([("client.scxml", CLIENT["python"])], log=logs, services={TYPE: lambda host, i: Crashing()}) as host:
            session = host.spawn(host.chart("client"))
            host.run_until_idle()
            host.send(session, "ask", "first")
            with self.assertRaises(ValueError):
                host.run_until_idle()
            host.run_until_idle()
            host.send(session, "ask", "second")
            host.run_until_idle()
            host.send(session, "leave")
            host.run_until_idle()
        self.assertEqual(logs.labelled("error"), ["communication", "communication"])
        self.assertEqual(stopped, [])  # dropped, not stopped

    def test_services_must_be_a_dict(self):
        with self.assertRaises(TypeError):
            tinyactors.Host({"systemID": "t"}, services=[TYPE])


if __name__ == "__main__":
    unittest.main()
