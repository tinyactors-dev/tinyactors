"""The "python" datamodel: the rules the suite cannot see (AGENTS.md,
"Rules the suite cannot see"), each with a test of its own, and the
datamodel's own behaviour (docs/datamodel.md)."""

import builtins
import unittest

import tinyactors
from tinyactors import Host, SimulatedClock, TinyactorsError

CONFIG = {"systemID": "h", "system": {"ticksPerSecond": 1000}}


def chart(body, datamodel="", name="c", initial="s"):
    return (f"<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='python' name='{name}' initial='{initial}'>"
            f"{datamodel and '<datamodel>' + datamodel + '</datamodel>'}{body}</scxml>")


OUTCOME = ("<final id='pass'><onentry><log label='Outcome' expr=\"'pass'\"/></onentry></final>"
           "<final id='fail'><onentry><log label='Outcome' expr=\"'fail'\"/></onentry></final>")


class Run:
    """A host with one chart, its logs and DONE data recorded."""

    def __init__(self, text, params=None, **handlers):
        self.logs, self.done = [], []
        self.host = Host(CONFIG, clock=SimulatedClock(), on_log=lambda s, label, t: self.logs.append((label, t)),
                         on_done=lambda s, data: self.done.append(data), **handlers)
        self.host.add_image({"c.scxml": text})
        self.session = self.host.spawn("c", params)

    def outcome(self):
        return [text for label, text in self.logs if label == "Outcome"]

    def context(self):
        return self.host.request("actor.context", {"session": self.session})["variables"]

    def close(self):
        self.host.close()


class ExpressionsTest(unittest.TestCase):
    def test_data_conditions_assign_script_log_and_done(self):
        run = Run(chart(
            "<state id='s'><onentry><script>Var3 = [x * Var1 for x in range(3)]</script>"
            "<log label='l' expr='Var2 + str(len(Var3))'/></onentry>"
            "<transition cond='Var1 == 2 and Var3 == [0, 2, 4]' target='f'><assign location='Var1' expr='Var1 + 1'/>"
            "</transition></state>"
            "<final id='f'><donedata><param name='n' expr='Var1'/><param name='t' expr='(1, True)'/></donedata></final>",
            "<data id='Var1' expr='2'/><data id='Var2' expr=\"'x'\"/><data id='Var3'/>"))
        self.assertEqual(run.logs, [("l", "x3")])
        self.assertEqual(run.done, [{"n": 3, "t": [1, True]}])
        run.close()

    def test_one_globals_dict_and_no_locals(self):
        # generator expressions and lambdas see the session's variables
        run = Run(chart(
            "<state id='s'><onentry><script>f = lambda y: y + Var1\nVar2 = sum(f(x) for x in [1, 2])</script>"
            "</onentry><transition cond='Var2 == 23' target='pass'/><transition target='fail'/></state>" + OUTCOME,
            "<data id='Var1' expr='10'/><data id='Var2'/>"))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_content_is_json_or_text(self):
        run = Run(chart(
            "<state id='s'><transition cond=\"Var1 == [1, {'a': None}] and Var2 == 'a b'\" target='pass'/>"
            "<transition target='fail'/></state>" + OUTCOME,
            "<data id='Var1'>[1, {\"a\": null}]</data><data id='Var2'>  a\n  b </data>"))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_spawn_params_override_data(self):
        run = Run(chart("<state id='s'><onentry><log label='v' expr='Var1'/></onentry></state>",
                        "<data id='Var1' expr='1'/>"), {"Var1": 7})
        self.assertEqual(run.logs, [("v", "7")])
        run.close()

    def test_in_event_and_system_variables(self):
        run = Run(chart(
            "<parallel id='p'><state id='a'><transition event='go' cond=\"In('b') and not In('nothing')\">"
            "<assign location='Var1' expr='(_event.name, _event.type, _event.data, _event.sendid, _name, "
            "_sessionid, sorted(_ioprocessors))'/></transition></state><state id='b'/></parallel>",
            "<data id='Var1'/>", initial="p"))
        run.host.send(run.session, "go", {"k": [1]})
        name, kind, data, sendid, actor, session, processors = run.context()["Var1"]
        self.assertEqual((name, kind, data, sendid, actor), ("go", "external", {"k": [1]}, None, "c"))
        self.assertEqual(session, str(run.session))
        self.assertIn("http://www.w3.org/TR/scxml/#SCXMLEventProcessor", processors)
        run.close()

    def test_foreach_declares_its_item_and_copies_the_array(self):
        run = Run(chart(
            "<state id='s'><onentry><foreach array='Var1' item='item' index='i'>"
            "<script>Var1.append(0)\nVar2 = Var2 + [item * 10 + i]</script></foreach></onentry>"
            "<transition cond='Var2 == [10, 21] and len(Var1) == 4' target='pass'/><transition target='fail'/>"
            "</state>" + OUTCOME,
            "<data id='Var1' expr='[1, 2]'/><data id='Var2' expr='[]'/>"))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_send_with_params_and_namelist_to_another_actor(self):
        text = chart(
            "<state id='s'><transition event='go'>"
            "<send event='got' targetexpr='_event.data[\"to\"]' namelist='Var1'><param name='p' expr='Var1 * 2'/>"
            "</send></transition><transition event='got' target='f'><assign location='Var1' expr='_event.data'/>"
            "</transition></state><final id='f'><donedata><content expr='Var1'/></donedata></final>",
            "<data id='Var1' expr='3'/>")
        run = Run(text)
        other = run.host.spawn("c")
        run.host.send(run.session, "go", {"to": f"#_scxml_{other}"})
        self.assertEqual(run.done, [{"Var1": 3, "p": 6}])
        run.close()


class ScopeRulesTest(unittest.TestCase):
    """System variables stay as they were; assign checks its location."""

    def check(self, executable):
        run = Run(chart(
            f"<state id='s'><onentry>{executable}<assign location='Var1' expr='2'/></onentry>"
            "<transition event='error.execution' cond='Var1 == 1' target='pass'/><transition event='*' target='fail'/>"
            "</state>" + OUTCOME, "<data id='Var1' expr='1'/>"))
        outcome = run.outcome()
        run.close()
        return outcome, None

    def test_a_changed_system_variable_is_restored_and_an_error(self):
        for executable in ("<script>_sessionid = 'x'</script>", "<script>_name = 1</script>",
                           "<script>_ioprocessors.clear() or globals().pop('_ioprocessors')</script>",
                           "<script>_event = 1</script>", "<script>_x = 1</script>",
                           "<script>del _sessionid</script>"):
            with self.subTest(executable):
                self.assertEqual(self.check(executable)[0], ["pass"])

    def test_assign_to_a_system_variable_or_an_undeclared_location(self):
        for location in ("_sessionid", "_event", "nothing", "nothing.x", "Var1["):
            with self.subTest(location):
                self.assertEqual(self.check(f"<assign location='{location}' expr='5'/>")[0], ["pass"])

    def test_the_system_variable_keeps_its_value(self):
        run = Run(chart("<state id='s'><onentry><script>_sessionid = 'x'</script></onentry></state>",
                        "<data id='Var1' expr='_sessionid'/>"))
        self.assertEqual(run.context()["Var1"], str(run.session))
        run.close()

    def test_variable_and_context_skip_builtins(self):
        run = Run(chart("<state id='s'/>", "<data id='Var1' expr='1'/>"))
        context = run.context()
        self.assertEqual(context, {"Var1": 1})
        run.close()

    def test_an_expression_that_does_not_compile_is_error_execution(self):
        self.assertEqual(self.check("<assign location='Var1' expr='1 +'/>")[0], ["pass"])


class ValuesTest(unittest.TestCase):
    def test_bool_before_int_crosses_as_bool(self):
        run = Run(chart("<final id='s'><donedata><content expr='[True, 1, False, 0]'/></donedata></final>"))
        self.assertEqual(run.done, [[True, 1, False, 0]])
        self.assertIs(run.done[0][0], True)
        run.close()

    def test_a_lone_surrogate_fails_the_capture(self):
        run = Run(chart(
            "<state id='s'><onentry><send event='e'><content expr=\"'a\\udc00b'\"/></send></onentry>"
            "<transition event='error.execution' target='pass'/><transition event='e' target='fail'/></state>"
            + OUTCOME))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_an_object_outside_the_table_fails_the_capture(self):
        run = Run(chart(
            "<state id='s'><onentry><send event='e'><content expr='{1, 2}'/></send></onentry>"
            "<transition event='error.execution' target='pass'/><transition event='e' target='fail'/></state>"
            + OUTCOME))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_what_crosses_is_a_copy(self):
        run = Run(chart(
            "<state id='s'><onentry><script>Var2 = [1]</script><send event='e'><content expr='Var2'/></send>"
            "<script>Var2.append(2)</script></onentry>"
            "<transition event='e' cond='_event.data == [1] and Var2 == [1, 2]' target='pass'/>"
            "<transition event='e' target='fail'/></state>" + OUTCOME, "<data id='Var2'/>"))
        self.assertEqual(run.outcome(), ["pass"])
        run.close()

    def test_undefined_and_address_keep_their_types(self):
        run = Run(chart("<state id='s'><transition event='v' target='f'><assign location='Var1' expr='_event.data'/>"
                        "</transition></state><final id='f'><donedata><content expr='Var1'/></donedata></final>",
                        "<data id='Var1'/>"))
        value = [tinyactors.UNDEFINED, tinyactors.Address("tinyactors://h/#_scxml_1")]
        run.host.send(run.session, "v", value)
        self.assertEqual(run.done, [value])
        self.assertIs(run.done[0][0], tinyactors.UNDEFINED)
        run.close()


class FinalizerTest(unittest.TestCase):
    """Dropping a value's last reference can run Python code: it waits
    until the outer call into core returned."""

    def test_a_finalizer_runs_after_the_run_and_may_call_the_host(self):
        calls = []

        def hook():
            try:
                run.host.request("actor.list")
                calls.append("ok")
            except TinyactorsError as error:
                calls.append(error.status)

        builtins.ta_python_test_hook = hook
        try:
            run = Run(chart(
                "<state id='s'><onentry><script>\n"
                "class Finalized:\n    def __del__(self):\n        ta_python_test_hook()\n"
                "keep = Finalized()</script></onentry>"
                "<transition event='end' target='f'/></state><final id='f'/>"))
            run.host.send(run.session, "end")  # the session ends: its globals go
            self.assertEqual(calls, ["ok"])
            run.close()
        finally:
            del builtins.ta_python_test_hook

    def test_closing_drops_what_the_sessions_held(self):
        calls = []
        builtins.ta_python_test_hook = lambda: calls.append(1)
        try:
            run = Run(chart(
                "<state id='s'><onentry><script>\n"
                "class Finalized:\n    def __del__(self):\n        ta_python_test_hook()\n"
                "keep = Finalized()</script></onentry></state>"))
            run.close()
            self.assertEqual(calls, [1])  # no collection needed: the session's globals were cleared
        finally:
            del builtins.ta_python_test_hook


class BaseExceptionTest(unittest.TestCase):
    def test_a_keyboard_interrupt_is_error_execution_then_raised_again(self):
        interrupt = KeyboardInterrupt("stop")
        builtins.ta_python_test_interrupt = interrupt
        try:
            run = Run(chart(
                "<state id='s'><transition event='go'><script>raise ta_python_test_interrupt</script></transition>"
                "<transition event='error.execution' target='pass'/></state>" + OUTCOME))
            with self.assertRaises(KeyboardInterrupt) as raised:
                run.host.send(run.session, "go")
            self.assertIs(raised.exception, interrupt)
            self.assertEqual(run.outcome(), ["pass"])  # the chart saw error.execution
            run.close()
        finally:
            del builtins.ta_python_test_interrupt

    def test_an_exception_is_only_error_execution(self):
        run = Run(chart("<state id='s'><transition event='go'><script>raise ValueError()</script></transition>"
                        "<transition event='error.execution' target='pass'/></state>" + OUTCOME))
        run.host.send(run.session, "go")
        self.assertEqual(run.outcome(), ["pass"])
        run.close()


class HandlesTest(unittest.TestCase):
    """Sessions and payloads are slots with generations: a reused slot
    serves its new session, never the old one's values."""

    def test_reused_slots(self):
        with Host(CONFIG, clock=SimulatedClock()) as host:
            host.add_image({"c.scxml": chart("<state id='s'/>", "<data id='Var1'/>")})
            for round in range(20):
                session = host.spawn("c", {"Var1": round})
                self.assertEqual(host.request("actor.context", {"session": session})["variables"], {"Var1": round})
                host.terminate(session)


if __name__ == "__main__":
    unittest.main()
