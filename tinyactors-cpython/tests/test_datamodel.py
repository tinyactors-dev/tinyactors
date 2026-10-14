"""The "python" datamodel (AGENTS.md, "The "python" datamodel"): the
session's scope, the rewrite, _event, capture and In()."""

import signal
import unittest

import tinyactors
from support import APP, Logs, chart, host_with


def run(document, events=(), params=None, processors=None):
    """Spawn the chart, send it the events, run until idle: its logs."""
    logs = Logs()
    host = host_with([("c.scxml", document)], processors, log=logs)
    session = host.spawn(host.chart("c"), params=params)
    for event in events:
        host.send(session, *event) if isinstance(event, tuple) else host.send(session, event)
    host.run_until_idle()
    host.close()
    return logs


class Scope(unittest.TestCase):
    def test_an_undeclared_location_is_error_execution(self):
        logs = run(chart(
            "<state id='s'><transition event='go'><assign location='nope' expr='1'/>"
            "<assign location='n' expr='2'/></transition>"
            "<transition event='error.execution'><log label='n' expr='str(n)'/></transition></state>",
            data="<data id='n' expr='0'/>"), ["go"])
        self.assertEqual(logs.labelled("n"), ["0"])  # the rest of the block did not run

    def test_a_system_variable_is_read_only(self):
        for location in ("_sessionid", "_event", "_name"):
            with self.subTest(location=location):
                logs = run(chart(
                    "<state id='s'><transition event='go'><assign location='%s' expr='1'/></transition>"
                    "<transition event='error.execution'><log label='e' expr=\"'refused'\"/></transition></state>"
                    % location), ["go"])
                self.assertEqual(logs.labelled("e"), ["refused"])

    def test_comprehensions_see_session_variables(self):
        logs = run(chart(
            "<state id='s'><onentry>"
            "<log label='l' expr='str([x * k for x in items])'/>"
            "<log label='g' expr='str(sum(x + k for x in items))'/>"
            "<log label='f' expr=\"f'{k}-{len(items)}'\"/>"
            "</onentry></state>", data="<data id='k' expr='10'/><data id='items' expr='[1, 2]'/>"))
        self.assertEqual(logs.lines, [("l", "[10, 20]"), ("g", "23"), ("f", "10-2")])

    def test_refused_constructs_are_error_execution(self):
        for expression in ("lambda: 1", "__import__('os')", "open('x')", "k.__class__", "(y := 1)"):
            with self.subTest(expression=expression):
                logs = run(chart(
                    "<state id='s'><onentry><log label='v' expr=\"%s\"/></onentry>"
                    "<transition event='error.execution'><log label='e' expr=\"'refused'\"/></transition></state>"
                    % expression.replace("'", "&apos;"), data="<data id='k' expr='1'/>"))
                self.assertEqual(logs.labelled("e"), ["refused"])
                self.assertEqual(logs.labelled("v"), [])

    def test_script_defs_are_methods(self):
        logs = run(chart(
            "<script>def double(x):\n    total = x * 2\n    return total + bonus\n</script>"
            "<state id='s'><onentry><script>result = double(4)</script>"
            "<log label='r' expr='str(result)'/></onentry></state>",
            data="<data id='bonus' expr='1'/><data id='result'/>"))
        self.assertEqual(logs.labelled("r"), ["9"])

    def test_params_override_data(self):
        logs = run(chart("<state id='s'><onentry><log label='n' expr='str(n)'/></onentry></state>",
                         data="<data id='n' expr='0'/>"), params={"n": 7})
        self.assertEqual(logs.labelled("n"), ["7"])

    def test_content_that_is_no_expression_is_text(self):
        logs = run(chart("<state id='s'><onentry><log label='t' expr='t'/><log label='l' expr='str(l)'/></onentry></state>",
                         data="<data id='t'>hello world</data><data id='l'>[1, 2]</data>"))
        self.assertEqual(logs.lines, [("t", "hello world"), ("l", "[1, 2]")])

    def test_system_variables(self):
        logs = run(chart("<state id='s'><onentry><log label='n' expr='_name'/>"
                         "<log label='s' expr='_sessionid'/>"
                         "<log label='p' expr='str(sorted(_ioprocessors))'/></onentry></state>"))
        self.assertEqual(logs.labelled("n"), ["c"])
        self.assertEqual(logs.labelled("s"), ["1"])
        self.assertIn("scxml", logs.labelled("p")[0])


class Events(unittest.TestCase):
    def test_event_fields(self):
        logs = run(chart("<state id='s'><transition event='go'>"
                         "<log label='e' expr='str((_event.name, _event.type, _event.sendid, _event.data))'/>"
                         "</transition></state>"), [("go", {"k": 1})])
        self.assertEqual(logs.labelled("e"), ["('go', 'external', None, {'k': 1})"])

    def test_event_is_immutable(self):
        logs = run(chart("<state id='s'><transition event='go'><assign location='_event.name' expr=\"'x'\"/></transition>"
                         "<transition event='error.execution'><log label='e' expr=\"'refused'\"/></transition></state>"),
                   ["go"])
        self.assertEqual(logs.labelled("e"), ["refused"])

    def test_in_and_foreach(self):
        logs = run(chart(
            "<parallel id='p'><state id='a'/><state id='b'><onentry>"
            "<foreach item='item' index='i' array='items'><assign location='total' expr='total + item * i'/></foreach>"
            "<log label='in' expr=\"str((In('a'), In('nope')))\"/><log label='t' expr='str(total)'/>"
            "</onentry></state></parallel>", initial="p",
            data="<data id='items' expr='[1, 2, 3]'/><data id='total' expr='0'/><data id='item'/><data id='i'/>"))
        self.assertEqual(logs.lines, [("in", "(True, False)"), ("t", "8")])

    def test_captured_data_is_a_copy(self):
        got = []

        def processor(host, send):
            got.append(send.data)

        document = chart("<state id='s'><onentry>"
                         "<send type='%s' target='app' event='e'><param name='v' expr='items'/></send>"
                         "<script>items.append(4)</script></onentry></state>" % APP,
                         data="<data id='items' expr='[1, 2, 3]'/>")
        run(document, processors={APP: processor})
        self.assertEqual(got, [{"v": [1, 2, 3]}])  # the session's later change is not seen

    def test_a_value_outside_the_table_does_not_cross(self):
        logs = run(chart("<state id='s'><onentry>"
                         "<send type='%s' target='app' event='e'><content expr='(1, 2)'/></send></onentry>"
                         "<transition event='error.execution'><log label='e' expr=\"'refused'\"/></transition></state>"
                         % APP), processors={APP: lambda host, send: None})
        self.assertEqual(logs.labelled("e"), ["refused"])


class Interrupts(unittest.TestCase):
    def test_a_base_exception_is_error_execution_and_raised_after_the_run(self):
        document = chart(
            "<state id='s'><transition event='go'><script>while True:\n    n = n + 1\n</script></transition>"
            "<transition event='error.execution'><log label='e' expr=\"'execution'\"/></transition></state>",
            data="<data id='n' expr='0'/>")
        logs = Logs()
        host = host_with([("c.scxml", document)], log=logs)
        session = host.spawn(host.chart("c"))
        host.send(session, "go")

        class Stop(KeyboardInterrupt):
            pass

        def interrupt(number, frame):
            raise Stop()

        previous = signal.signal(signal.SIGALRM, interrupt)
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.05)
            with self.assertRaises(Stop):
                host.run_until_idle()
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous)
        host.run_until_idle()
        self.assertEqual(logs.labelled("e"), ["execution"])
        host.close()


if __name__ == "__main__":
    unittest.main()
