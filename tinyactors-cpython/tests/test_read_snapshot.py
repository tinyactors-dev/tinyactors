"""read_snapshot: a pack read without a host (tinyactors-core/docs/embedding.md,
section 12). Core's readers answer the moment of the snapshot: an actor's
state, mailbox and delayed sends, its chart's structure, and the spans its
journal kept, while the host goes on."""

import unittest

import tinyactors

ORDER = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="%s" name="order" initial="s">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="s">
    <onentry><send event="nag" id="nag" delay="10s"/></onentry>
    <transition event="order.pay" cond="n &lt; 3" target="paid"/>
    <transition event="*"><assign location="n" expr="n + 1"/></transition>
  </state>
  <state id="paid"/>
</scxml>"""
CONFIG = {"system": {"ticksPerSecond": 1000}, "telemetry": {"flushInterval": 100}}


def shop(datamodel, config=CONFIG):
    """A host whose one actor has a delayed send and an event waiting, and its snapshot."""
    clock = tinyactors.SimulatedClock()
    with tinyactors.compile_charts([("order.scxml", ORDER % datamodel)]) as compiled:
        host = tinyactors.Host(compiled, system="t", config=config, clock=clock, telemetry=lambda signal, data: None)
    host.__enter__()
    actor = host.spawn("order", name="order-1")
    host.run_until_idle()
    actor.send("poke")
    host.run_until_idle()
    clock.increment(0.2)
    actor.send("order.pay")  # queued: the snapshot comes before a turn
    return host, actor, tinyactors.read_snapshot(host.snapshot())


class ReadSnapshot(unittest.TestCase):
    def test_actors_their_states_mailboxes_and_delayed_sends(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                host, actor, snapshot = shop(datamodel)
                self.assertIsInstance(snapshot, tinyactors.Snapshot)
                self.assertEqual((snapshot.system_id, snapshot.segments, snapshot.time), ("t", 1, 200))
                [handle] = snapshot.actors(0)
                read = snapshot.actor(0, handle)
                self.assertEqual((read["session"], read["name"], read["chart_name"]), (actor.id, "order-1", "order"))
                self.assertEqual((read["status"], read["parent"], read["mailbox"]), ("runnable", None, 1))
                chart = snapshot.charts(0)[read["chart"]]
                self.assertEqual([chart["states"][i]["id"] for i in snapshot.configuration(0, handle)], ["s"])
                self.assertEqual(snapshot.mailbox(0, handle), [{"event": "order.pay", "sendid": None}])
                self.assertEqual(snapshot.timers(0, handle), [{"due": 10000, "event": "nag", "sendid": "nag"}])
                self.assertEqual(snapshot.stats(0)["ticks_per_second"], 1000)
                host.run_until_idle()  # the live host goes on; the snapshot holds its moment
                self.assertEqual(len(snapshot.mailbox(0, handle)), 1)
                host.__exit__(None, None, None)

    def test_a_chart_as_the_image_has_it(self):
        host, _, snapshot = shop("python")
        [chart] = snapshot.charts(0)
        self.assertEqual((chart["name"], chart["data"]), ("order", ["n"]))
        self.assertEqual([(s["id"], s["kind"], s["parent"]) for s in chart["states"]],
                         [("", "compound", None), ("s", "atomic", 0), ("paid", "atomic", 0)])
        [pay] = [t for t in chart["transitions"] if t["events"] == ["order.pay"]]
        self.assertEqual((pay["cond"], chart["states"][pay["targets"][0]]["id"], pay["type"]), ("n < 3", "paid", "external"))
        self.assertTrue(any(t["events"] == ["*"] and not t["targets"] for t in chart["transitions"]))
        self.assertTrue(any(t["type"] == "initial" for t in chart["transitions"]))
        host.__exit__(None, None, None)

    def test_the_spans_the_journal_kept(self):
        host, _, snapshot = shop("python")
        kept = snapshot.trace_kept(0)
        self.assertGreater(kept["records"], 0)
        spans = tinyactors.decode_traces(snapshot.traces(0))
        self.assertIn("scxml.spawned", [span["name"] for span in spans])
        self.assertGreaterEqual(len([s for s in spans if s["name"].startswith("scxml.macrostep")]), 2)
        self.assertTrue(all(span["start"] <= span["end"] for span in spans))
        wall = snapshot.stats(0)["wall_ns"]  # the wall clock the host gave, placing the spans
        self.assertGreater(wall, 0)
        self.assertTrue(all(span["end"] <= wall + 10**9 for span in spans))
        host.__exit__(None, None, None)
        quiet, _, snapshot = shop("python", {"system": {"ticksPerSecond": 1000, "tracing": False}})
        self.assertIsNone(snapshot.trace_kept(0))
        self.assertIsNone(snapshot.traces(0))
        quiet.__exit__(None, None, None)

    def test_refusals(self):
        host, _, snapshot = shop("python")
        with self.assertRaises(tinyactors.InvalidError):
            tinyactors.read_snapshot(bytes(64))
        with self.assertRaises(TypeError):
            tinyactors.read_snapshot("bytes")
        with self.assertRaises(IndexError):
            snapshot.actors(1)
        with self.assertRaises(IndexError):
            snapshot.actor(0, 12345)
        with self.assertRaises(TypeError):
            tinyactors.Snapshot()
        host.__exit__(None, None, None)


if __name__ == "__main__":
    unittest.main()
