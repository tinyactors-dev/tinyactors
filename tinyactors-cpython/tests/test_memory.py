"""The binding's own blocks of a host and quickjs's heap (AGENTS.md, "The binding's memory").

The host's tables, arena and the "python" datamodel's kept writer grow through a counting
allocator, apart from core's host, which counts its own (host_stats' allocatedBytes). The
module's private _binding_memory(host) reads the count; it is no public API
(docs/proposals/memory-accounting.md, O1)."""

import unittest

import tinyactors

COUNTER = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="{datamodel}"
       name="counter" initial="s">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="s"><transition event="inc"><assign location="count" expr="count + 1"/></transition></state>
</scxml>"""


def image(datamodel):
    with tinyactors.compile_charts([("counter.scxml", COUNTER.format(datamodel=datamodel))]) as compiled:
        return compiled.image


class BindingMemory(unittest.TestCase):
    def test_the_bindings_own_blocks_are_counted_and_none_is_left_after_close(self):
        for datamodel in ["python", "ecmascript"]:
            with self.subTest(datamodel=datamodel):
                host = tinyactors.Host(image(datamodel), system="t")
                with host:
                    start = tinyactors._binding_memory(host)
                    actors = [host.spawn("counter") for _ in range(1000)]
                    for actor in actors:
                        actor.send("inc")
                    host.run_until_idle()
                    grown = tinyactors._binding_memory(host)
                    if datamodel == "python":  # a session is a row of the binding's session table
                        self.assertGreaterEqual(grown["bytes"], start["bytes"] + 1000 * 24)
                    self.assertGreater(grown["bytes"], 0)
                    self.assertGreaterEqual(grown["peak"], grown["bytes"])
                closed = tinyactors._binding_memory(host)
                self.assertEqual(closed["bytes"], 0)
                self.assertEqual(closed["blocks"], grown["blocks"], "close allocates nothing")

    def test_binding_memory_refuses_what_is_not_a_host(self):
        with self.assertRaises(TypeError):
            tinyactors._binding_memory(object())


class QuickjsHeap(unittest.TestCase):
    def test_quickjs_heap_bytes_and_blocks_grow_with_its_actors_and_are_none_once_closed(self):
        host = tinyactors.Host(image("ecmascript"), system="t")
        with host:
            bytes_before, blocks_before = host.quickjs_heap_bytes, host.quickjs_heap_blocks
            for _ in range(1000):
                host.spawn("counter")
            host.run_until_idle()
            self.assertGreater(host.quickjs_heap_bytes, bytes_before)
            self.assertGreater(host.quickjs_heap_blocks, blocks_before)
        self.assertIsNone(host.quickjs_heap_bytes)
        self.assertIsNone(host.quickjs_heap_blocks)


if __name__ == "__main__":
    unittest.main()
