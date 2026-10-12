"""tinyactors.Host on quickjs: config, requests, inputs, the turn and its
effects, the simulated clock, and what the suite cannot see (a handler's
exception, closing from a handler, the creating thread, cycles)."""

import gc
import threading
import unittest
import weakref

import tinyactors
from tinyactors import Host, SimulatedClock, TinyactorsError

CONFIG = {"systemID": "h", "system": {"ticksPerSecond": 1000}}

COUNTER = """<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='counter' initial='a'>
<datamodel><data id='n' expr='0'/></datamodel>
<state id='a'>
  <onentry><send event='tick' delay='50ms'/></onentry>
  <transition event='inc'><assign location='n' expr='n + _event.data.by'/><log label='n' expr='n'/></transition>
  <transition event='tick' target='done'/>
</state>
<final id='done'><donedata><param name='total' expr='n'/></donedata></final>
</scxml>"""

FAILING = """<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='failing' initial='a'>
<state id='a'><transition event='go' target='b'/></state>
<state id='b'><onentry><log label='one' expr='1'/><log label='two' expr='2'/></onentry></state>
</scxml>"""


class Recorder:
    def __init__(self):
        self.logs, self.done, self.errors = [], [], []

    def handlers(self):
        return {"on_log": lambda *a: self.logs.append(a), "on_done": lambda *a: self.done.append(a),
                "on_error": lambda *a: self.errors.append(a)}


class ConfigTest(unittest.TestCase):
    def test_a_refused_config_raises_cores_error_unchanged(self):
        with self.assertRaises(TinyactorsError) as raised:
            Host({"systemID": "h", "x": 1})
        error = raised.exception
        self.assertEqual((error.code, error.reason, error.path, error.status), (-32602, "unknown", "x", -3))

    def test_the_config_passes_through_unchecked(self):
        with self.assertRaises(TinyactorsError) as raised:
            Host(1)
        self.assertEqual((raised.exception.code, raised.exception.reason), (-32600, "type"))

    def test_a_config_outside_the_value_table(self):
        with self.assertRaises(TinyactorsError) as raised:
            Host({"systemID": {1}})
        self.assertEqual(raised.exception.status, tinyactors.ERROR_INVALID)


class RequestTest(unittest.TestCase):
    def test_result_and_error(self):
        with Host(CONFIG) as host:
            self.assertEqual(host.request("actor.list"), {"sessions": [], "cursor": None})
            with self.assertRaises(TinyactorsError) as raised:
                host.request("actor.get", {"session": 99})
            error = raised.exception
            self.assertEqual((error.code, error.reason, error.path, error.status), (-32001, "stale", "params.session",
                                                                                     None))

    def test_values_pass_through_to_core(self):
        with Host(CONFIG) as host:
            with self.assertRaises(TinyactorsError) as raised:
                host.request(1)
            self.assertEqual((raised.exception.code, raised.exception.path), (-32600, "method"))
            with self.assertRaises(TinyactorsError) as raised:
                host.request("actor.list", 1)
            self.assertEqual((raised.exception.code, raised.exception.path), (-32600, "params"))


class TurnTest(unittest.TestCase):
    def test_inputs_run_the_turn_and_the_clock_moves_time(self):
        record, clock = Recorder(), SimulatedClock()
        with Host(CONFIG, clock=clock, **record.handlers()) as host:
            host.add_image({"counter.scxml": COUNTER})
            session = host.spawn("counter", name="c")
            host.send(session, "inc", {"by": 2})
            self.assertEqual(host.send_many([(session, "inc", {"by": 3}), (session, "inc", {"by": 4})]), 2)
            self.assertEqual(record.logs, [(session, "n", "2"), (session, "n", "5"), (session, "n", "9")])
            clock.increment(49)
            self.assertEqual(record.done, [])
            clock.increment(1)
            self.assertEqual(record.done, [(session, {"total": 9})])

    def test_spawn_params_and_terminate(self):
        record = Recorder()
        with Host(CONFIG, clock=SimulatedClock(), **record.handlers()) as host:
            host.add_image({"counter.scxml": COUNTER})
            session = host.spawn("counter", {"n": 10})
            host.send(session, "inc", {"by": 1})
            self.assertEqual(record.logs[-1], (session, "n", "11"))
            host.terminate(session)
            with self.assertRaises(TinyactorsError):
                host.request("actor.get", {"session": session})

    def test_refusals_are_cores(self):
        with Host(CONFIG) as host:
            with self.assertRaises(TinyactorsError):
                host.spawn("nothing")
            with self.assertRaises(TinyactorsError) as raised:
                host.add_image({"x.scxml": "<scxml"})
            self.assertEqual(raised.exception.diagnostics[0][4], "x.scxml")

    def test_a_handlers_exception_is_raised_after_the_turn(self):
        seen = []

        def on_log(session, label, text):
            seen.append(label)
            raise ValueError(label)

        with Host(CONFIG, on_log=on_log) as host:
            host.add_image({"failing.scxml": FAILING})
            session = host.spawn("failing")
            with self.assertRaises(ValueError) as raised:
                host.send(session, "go")
            self.assertEqual(str(raised.exception), "one")  # the first
            self.assertEqual(seen, ["one", "two"])  # the turn went on
            self.assertEqual(host.request("actor.state", {"session": session})["value"], "b")

    def test_closing_from_a_handler_waits_for_the_turn(self):
        def on_log(session, label, text):
            host.close()

        host = Host(CONFIG, on_log=on_log)
        host.add_image({"failing.scxml": FAILING})
        host.send(host.spawn("failing"), "go")
        with self.assertRaises(TinyactorsError) as raised:
            host.request("actor.list")
        self.assertEqual(raised.exception.status, tinyactors.ERROR_STATE)

    def test_an_input_from_a_handler_runs_in_the_same_turn(self):
        record = Recorder()

        def on_log(session, label, text):
            record.logs.append(label)
            if label == "one" and len(record.logs) == 1:
                host.send(other, "go")

        with Host(CONFIG, on_log=on_log) as host:
            host.add_image({"failing.scxml": FAILING})
            first, other = host.spawn("failing"), host.spawn("failing")
            host.send(first, "go")
            self.assertEqual(record.logs, ["one", "two", "one", "two"])


class ClockTest(unittest.TestCase):
    def test_increment_takes_milliseconds(self):
        with self.assertRaises(TypeError):
            SimulatedClock().increment("1")

    def test_a_clock_keeps_no_host_alive(self):
        clock = SimulatedClock()
        host = Host(CONFIG, clock=clock)
        reference = weakref.ref(host)
        del host
        gc.collect()
        self.assertIsNone(reference())
        clock.increment(1)

    def test_without_ticks_per_second_the_clock_is_refused(self):
        # ROADMAP.md, request 2: the binding cannot read the host's tick rate
        clock = SimulatedClock()
        with Host({"systemID": "h"}, clock=clock):
            with self.assertRaises(TinyactorsError) as raised:
                clock.increment(1)
            self.assertEqual(raised.exception.status, tinyactors.ERROR_UNSUPPORTED)


class LifetimeTest(unittest.TestCase):
    def test_one_thread(self):
        with Host(CONFIG) as host:
            errors = []

            def call():
                try:
                    host.request("actor.list")
                except TinyactorsError as error:
                    errors.append(error.status)

            thread = threading.Thread(target=call)
            thread.start()
            thread.join()
            self.assertEqual(errors, [tinyactors.ERROR_STATE])
            host.request("actor.list")

    def test_close_twice_and_calls_after_close(self):
        host = Host(CONFIG)
        host.close()
        host.close()
        with self.assertRaises(TinyactorsError) as raised:
            host.spawn("x")
        self.assertEqual(raised.exception.status, tinyactors.ERROR_STATE)

    def test_a_handler_that_closes_over_its_host_is_collected(self):
        # Py_tp_traverse and Py_tp_clear (AGENTS.md, "Handler closures make cycles")
        def make():
            holder = {}
            host = Host(CONFIG, on_log=lambda *a: holder["host"].request("actor.list"))
            holder["host"] = host
            return weakref.ref(host)

        reference = make()
        gc.collect()
        self.assertIsNone(reference())

    def test_handlers_are_callable(self):
        with self.assertRaises(TypeError):
            Host(CONFIG, on_log=1)


if __name__ == "__main__":
    unittest.main()
