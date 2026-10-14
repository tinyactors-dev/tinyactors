"""The Host, the loop and the effects: the rules the suite cannot see
(AGENTS.md, "Rules the suite cannot see")."""

import gc
import sys
import threading
import unittest
import weakref

import tinyactors
from support import APP, Logs, chart, host_with

# A chart that sends `out` with its event's data to the application's
# processor, and logs what the answer brings.
ECHO = chart(
    "<state id='s'>"
    "<transition event='go'><send type='%s' target='app' event='out'><content expr='_event.data'/></send></transition>"
    "<transition event='ack'><log label='ack' expr='str(_event.data)'/></transition>"
    "<transition event='error.communication'><log label='failed' expr='_event.sendid or \"none\"'/></transition>"
    "</state>" % APP, name="echo")


def spawn(host, name="echo", **options):
    return host.spawn(host.chart(name), **options)


class Views(unittest.TestCase):
    def test_a_view_is_stale_after_its_ack(self):
        kept = []

        def processor(host, send):
            kept.append(send)
            self.assertEqual(send.event, "out")

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go", {"n": 1})
        host.run_until_idle()
        self.assertEqual(kept[0].event, "out")  # cached: read in the handler
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            kept[0].data  # never read before the ack
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
        with self.assertRaises(tinyactors.TinyactorsError):
            kept[0].answer("ack")
        host.close()

    def test_fields_stay_valid_after_later_calls_in_the_batch(self):
        """One drain at a time: a typed method a handler calls is answered in
        the next take of the same turn, and the view stays readable."""
        seen = []

        def processor(host, send):
            host.host_stats(lambda host, result: seen.append(("stats", result["host"]["actors"])))
            host.send(send.session, "ack", {"later": True})
            seen.append(("view", send.event, send.data, send.session))

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        session = spawn(host)
        host.send(session, "go", [1, 2])
        host.run_until_idle()
        self.assertEqual(seen, [("view", "out", [1, 2], session), ("stats", 1)])
        self.assertEqual(logs.labelled("ack"), ["{'later': True}"])
        host.close()

    def test_answer_and_its_data(self):
        def processor(host, send):
            self.assertEqual((send.type, send.target, send.sendid), (APP, "app", None))
            send.answer("ack", {"echo": send.data})

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go", {"k": [1]})
        host.run_until_idle()
        self.assertEqual(logs.labelled("ack"), ["{'echo': {'k': [1]}}"])
        host.close()

    def test_send_data_is_the_payload_value_and_dropped_after_the_call(self):
        kept = []

        def processor(host, send):
            kept.append(send.data)
            kept.append(sys.getrefcount(send.data))

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go", ["value"])
        host.run_until_idle()
        # the row held a reference during the handler; it was dropped after
        # the ack, outside core (the deferred drop)
        self.assertLess(sys.getrefcount(kept[0]), kept[1])
        host.close()

    def test_a_log_entry_is_a_thin_view(self):
        entries = []
        doc = chart("<state id='s'><onentry><log label='l' expr=\"'text'\"/></onentry></state>")
        host = host_with([("c.scxml", doc)], log=lambda host, entry: entries.append((entry, entry.label)))
        session = spawn(host, "c")
        host.run_until_idle()
        entry, label = entries[0]
        self.assertEqual(label, "l")
        with self.assertRaises(tinyactors.TinyactorsError):
            entry.text
        self.assertEqual(entry.label, "l")
        self.assertTrue(session)
        host.close()


class Bounds(unittest.TestCase):
    def test_a_stale_view_never_answers_whatever_was_read(self):
        kept = []

        def processor(host, send):
            (send.session, send.sendid, send.type, send.traceparent)  # every field answer() needs
            kept.append(send)

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go")
        host.run_until_idle()
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            kept[0].answer("ack")
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")
        host.close()

    def test_a_dropped_host_is_freed_without_the_collector(self):
        marker = type("Marker", (), {})()
        reference = weakref.ref(marker)
        gc.disable()
        try:
            host = host_with([("echo.scxml", ECHO)], log=lambda h, e, marker=marker: marker)
            spawn(host)
            host.run_until_idle()
            del marker, host
            self.assertIsNone(reference())
        finally:
            gc.enable()

    def test_send_many_in_slices(self):
        counter = chart("<state id='s'><transition event='inc'><assign location='n' expr='n + 1'/></transition>"
                        "<transition event='show'><log label='n' expr='str(n)'/></transition></state>",
                        name="counter", data="<data id='n' expr='0'/>")
        logs = Logs()
        host = tinyactors.Host({"systemID": "t", "system": {"maxEvents": 4096}}, log=logs)
        with tinyactors.compile_charts([("counter.scxml", counter)]) as compiled:
            host.add_image(compiled.image)
        session = host.spawn(host.chart("counter"))
        self.assertEqual(host.send_many([(session, "inc")] * 1000 + [(session, "show")]), 1001)
        host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["1000"])
        with self.assertRaises(TypeError):
            host.send_many([(session, "inc")] * 300 + [("bad",)])
        host.close()


class Processors(unittest.TestCase):
    def test_a_processor_that_raises_fails_its_send(self):
        def processor(host, send):
            raise ValueError("no")

        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go")
        with self.assertRaises(ValueError):
            host.run_until_idle()
        host.run_until_idle()  # the failure is the sender's error.communication
        self.assertEqual(len(logs.labelled("failed")), 1)
        host.close()

    def test_a_processor_without_a_handler_fails_its_send(self):
        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {}, log=logs)
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertEqual(len(logs.labelled("failed")), 1)
        host.close()

    def test_an_async_processor_is_refused_under_with(self):
        async def processor(host, send):
            pass

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        with self.assertRaises(TypeError) as raised:
            with host:
                pass
        self.assertIn(APP, str(raised.exception))
        with self.assertRaises(TypeError):
            host.run_until_idle()
        host.close()

    def test_a_handler_for_a_type_the_config_lacks_is_refused(self):
        with self.assertRaises(ValueError):
            tinyactors.Host({"systemID": "t"}, {"urn:nope": print})


class Methods(unittest.TestCase):
    def test_an_error_goes_to_on_error_or_to_the_driver(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_get(print, lambda host, error: errors.append((error.code, error.reason, error.path)), session=99)
        host.run_until_idle()
        self.assertEqual(errors, [(-32001, "stale", "params.session")])
        host.actor_get(print, session=99)
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            host.run_until_idle()
        self.assertEqual(raised.exception.reason, "stale")
        host.close()

    def test_params_pass_through_to_core(self):
        errors = []
        host = host_with([("echo.scxml", ECHO)])
        host.actor_list(print, lambda host, error: errors.append(error.path), x=1)
        host.actor_list(print, lambda host, error: errors.append(error.path), limit="x")
        host.run_until_idle()
        self.assertEqual(errors, ["params.x", "params.limit"])
        host.close()

    def test_pending_handlers_are_dropped_at_close(self):
        called = []
        host = host_with([("echo.scxml", ECHO)])
        host.host_stats(lambda host, result: called.append(result))
        host.close()
        self.assertEqual(called, [])
        with self.assertRaises(tinyactors.TinyactorsError):
            host.host_stats(print)


class Lifetime(unittest.TestCase):
    def test_handler_cycles_are_collected(self):
        logs_ref = []

        def build():
            holder = {}
            host = host_with([("echo.scxml", ECHO)], log=lambda h, e: holder.get("host"))
            holder["host"] = host  # host -> log handler -> holder -> host
            marker = type("Marker", (), {})()
            holder["marker"] = marker
            logs_ref.append(weakref.ref(marker))

        build()
        gc.collect()
        self.assertIsNone(logs_ref[0]())

    def test_close_from_a_handler_waits_for_the_loop(self):
        def processor(host, send):
            host.close()
            self.assertFalse(host.closed)

        host = host_with([("echo.scxml", ECHO)], {APP: processor})
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertTrue(host.closed)

    def test_another_thread_is_refused(self):
        host = host_with([("echo.scxml", ECHO)])
        caught = []
        thread = threading.Thread(target=lambda: caught.append(self.assertRaises(tinyactors.TinyactorsError, host.chart, "echo")))
        thread.start()
        thread.join()
        host.close()

    def test_a_nested_call_and_an_exception_leave_the_host_usable(self):
        def processor(host, send):
            with self_test.assertRaises(TypeError):
                host.send(send.session, "ack", object())  # not of the value table
            host.send(send.session, "ack", {"nested": [1, 2, 3]})

        self_test = self
        logs = Logs()
        host = host_with([("echo.scxml", ECHO)], {APP: processor}, log=logs)
        host.send(spawn(host), "go")
        host.run_until_idle()
        self.assertEqual(logs.labelled("ack"), ["{'nested': [1, 2, 3]}"])
        host.close()

    def test_a_refused_config_raises_cores_error(self):
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            tinyactors.Host({"systemID": "t", "x": 1})
        self.assertEqual((raised.exception.code, raised.exception.reason, raised.exception.path), (-32602, "unknown", "x"))


class Inputs(unittest.TestCase):
    def test_send_many_and_spawn_params(self):
        counter = chart("<state id='s'><transition event='inc'><assign location='n' expr='n + (_event.data or 1)'/></transition>"
                        "<transition event='show'><log label='n' expr='str(n)'/></transition></state>",
                        name="counter", data="<data id='n' expr='0'/>")
        logs = Logs()
        host = host_with([("counter.scxml", counter)], log=logs)
        session = host.spawn(host.chart("counter"), params={"n": 10})
        self.assertEqual(host.send_many([(session, "inc"), (session, "inc", 5), (session, "show")]), 3)
        host.run_until_idle()
        self.assertEqual(logs.labelled("n"), ["16"])
        host.close()

    def test_a_send_to_no_session_is_refused(self):
        host = host_with([("echo.scxml", ECHO)])
        with self.assertRaises(tinyactors.TinyactorsError):
            host.send(12345, "go", {"x": 1})
        host.close()

    def test_terminate(self):
        host = host_with([("echo.scxml", ECHO)])
        session = spawn(host)
        host.terminate(session)
        host.run_until_idle()
        got = []
        host.actor_list(lambda host, result: got.append(result["sessions"]))
        host.run_until_idle()
        self.assertEqual(got, [[]])
        host.close()


class Traces(unittest.TestCase):
    TRACEPARENT = "00-0102030405060708090a0b0c0d0e0f10-a1a2a3a4a5a6a7a8-01"

    def test_traces_cross(self):
        relay = chart(
            "<state id='s'>"
            "<transition event='go'><send type='%s' target='app' event='first'/></transition>"
            "<transition event='ack'><send type='%s' target='app' event='second'/></transition>"
            "</state>" % (APP, APP), name="relay")
        seen = []
        mode = {"keep": True}

        def processor(host, send):
            seen.append((send.event, send.traceparent))
            if send.event == "first":
                if mode["keep"]:
                    send.answer("ack")
                else:
                    send.answer("ack", traceparent=None)

        host = host_with([("relay.scxml", relay)], {APP: processor})
        session = spawn(host, "relay")
        host.send(session, "go", traceparent=self.TRACEPARENT)
        host.run_until_idle()
        trace = self.TRACEPARENT.split("-")[1]
        self.assertEqual([event for event, _ in seen], ["first", "second"])
        self.assertEqual(seen[0][1].split("-")[1], trace)  # the send runs in the trace the input started
        self.assertEqual(seen[1][1].split("-")[1], trace)  # answer() keeps it
        seen.clear()
        mode["keep"] = False
        host.send(session, "go", traceparent=self.TRACEPARENT)
        host.run_until_idle()
        self.assertEqual(seen[0][1].split("-")[1], trace)
        self.assertNotEqual(seen[1][1].split("-")[1], trace)  # traceparent=None: a new trace
        host.close()


class Requests(unittest.TestCase):
    def test_a_fetch_at_run_time_is_refused_in_c(self):
        parent = chart(
            "<state id='s'><invoke srcexpr=\"'missing.scxml'\"/>"
            "<transition event='error.execution'><log label='refused' expr=\"'yes'\"/></transition></state>",
            name="parent")
        logs = Logs()
        host = host_with([("parent.scxml", parent)], log=logs)
        spawn(host, "parent")
        host.run_until_idle()
        self.assertEqual(logs.labelled("refused"), ["yes"])
        host.close()


if __name__ == "__main__":
    unittest.main()
