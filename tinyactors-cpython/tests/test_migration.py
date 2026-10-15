"""Migration: Host.migrate, the migrated handler and the job's result, on both datamodels."""

import unittest

import tinyactors
from support import Logs, fixture, host_with


def versions(datamodel):
    """Version 1 counts in n; version 2 takes n from error.platform.upgrade
    and adds `version`. quickjs's upgrade data names version 2's new
    variables too, undefined; `seen` leaves those out."""
    return (fixture("test_migration/counter-v1.%s.scxml" % datamodel),
            fixture("test_migration/counter-v2.%s.scxml" % datamodel))


def add(host, document):
    with tinyactors.compile_charts([("counter.scxml", document)]) as compiled:
        return host.add_image(compiled.image)


class Migration(unittest.TestCase):
    def migrate_one(self, datamodel):
        v1, v2 = versions(datamodel)
        logs, moved, done = Logs(), [], []

        def migrated(host, entry):
            moved.append((entry.session, entry.segment, entry.chart, entry.job))

        with host_with([("counter.scxml", v1)], log=logs, migrated=migrated) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc")
            host.send(session, "inc")
            host.run_until_idle()
            segment = add(host, v2)
            job = host.migrate(lambda host, result: done.append(result))
            host.run_until_idle()
            host.send(session, "inc")
            host.send(session, "report")
            host.run_until_idle()
        self.assertEqual(done, [{"job": job, "migrated": 1, "failed": 0, "pending": []}])
        self.assertEqual(moved, [(session, segment, "counter", job)])
        return logs.labelled("n")

    def test_a_python_actor_moves_with_its_bindings(self):
        self.assertEqual(self.migrate_one("python"), ["3 2 ['n', 'other']"])

    def test_an_ecmascript_actor_moves_with_its_bindings(self):
        self.assertEqual(self.migrate_one("ecmascript"), ['3 2 ["n","other"]'])

    def test_a_delayed_send_survives_the_move(self):
        for datamodel in ("python", "ecmascript"):
            with self.subTest(datamodel=datamodel):
                v1, v2 = versions(datamodel)
                logs, clock = Logs(), tinyactors.SimulatedClock()
                with host_with([("counter.scxml", v1)], log=logs, clock=clock) as host:
                    session = host.spawn(host.chart("counter"))
                    host.send(session, "inc")
                    host.send(session, "wait")
                    host.run_until_idle()
                    add(host, v2)
                    host.migrate(None)
                    host.run_until_idle()
                    clock.increment(1000)
                self.assertEqual(logs.labelled("later"), ["1"])

    def test_a_hibernated_actor_is_asked_for_with_its_job(self):
        v1, v2 = versions("python")
        logs, blobs, asked, done = Logs(), {}, [], []

        def wake(host, session, job):
            asked.append((session, job))
            host.wake(session, blobs.pop(session))

        with host_with([("counter.scxml", v1)], log=logs, wake=wake) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "inc")
            host.run_until_idle()
            blobs[session] = host.hibernate(session)
            add(host, v2)
            job = host.migrate(lambda host, result: done.append(result), wake=True)
            host.run_until_idle()
            host.send(session, "report")
            host.run_until_idle()
        self.assertEqual(asked, [(session, job)])
        self.assertEqual(done[0]["migrated"], 1)
        self.assertEqual(logs.labelled("n"), ["1 2 ['n', 'other']"])

    def test_an_events_wake_has_no_job(self):
        v1, _ = versions("python")
        blobs, asked = {}, []

        def wake(host, session, job):
            asked.append(job)
            host.wake(session, blobs.pop(session))

        with host_with([("counter.scxml", v1)], wake=wake) as host:
            session = host.spawn(host.chart("counter"))
            host.run_until_idle()
            blobs[session] = host.hibernate(session)
            host.send(session, "inc")
            host.run_until_idle()
        self.assertEqual(asked, [None])

    def test_without_wake_or_timeout_a_hibernated_actor_holds_the_job(self):
        """The job asks for a sleeping actor never (no wake) and so never
        ends without a timeout: the README says so."""
        v1, v2 = versions("python")
        done, clock = [], tinyactors.SimulatedClock()
        with host_with([("counter.scxml", v1)], clock=clock) as host:
            asleep = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(asleep)
            add(host, v2)
            host.migrate(lambda host, result: done.append(result))
            host.run_until_idle()
            clock.increment(60_000)
        self.assertEqual(done, [])

    def test_without_wake_a_hibernated_actor_is_left_at_the_timeout(self):
        v1, v2 = versions("python")
        done, clock = [], tinyactors.SimulatedClock()
        with host_with([("counter.scxml", v1)], clock=clock) as host:
            asleep, awake = host.spawn(host.chart("counter")), host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(asleep)
            add(host, v2)
            job = host.migrate(lambda host, result: done.append(result), timeout=500)
            host.run_until_idle()
            self.assertEqual(done, [])
            clock.increment(500)
        self.assertEqual(done, [{"job": job, "migrated": 1, "failed": 0, "pending": [asleep]}])
        self.assertNotIn(awake, done[0]["pending"])

    def test_one_session_or_one_chart(self):
        v1, v2 = versions("python")
        other = fixture("test_migration/other.python.scxml")
        done = []
        with host_with([("counter.scxml", v1), ("other.scxml", other)]) as host:
            a, b = host.spawn(host.chart("counter")), host.spawn(host.chart("counter"))
            host.run_until_idle()
            add(host, v2)
            host.migrate(lambda host, result: done.append(result), session=a)
            host.run_until_idle()
            host.migrate(lambda host, result: done.append(result), chart="other")
            host.run_until_idle()
            host.migrate(lambda host, result: done.append(result), chart="counter")
            host.run_until_idle()
        self.assertEqual([result["migrated"] for result in done], [1, 0, 1])
        self.assertTrue(a and b)

    def test_a_python_actor_with_a_value_outside_the_table_stays(self):
        v1, v2 = versions("python")
        done = []
        with host_with([("counter.scxml", v1)]) as host:
            session = host.spawn(host.chart("counter"))
            host.send(session, "odd")
            host.run_until_idle()
            add(host, v2)
            host.migrate(lambda host, result: done.append(result))
            host.run_until_idle()
            host.send(session, "inc")  # still version 1, still running
            host.run_until_idle()
        self.assertEqual(done[0]["migrated"], 0)
        self.assertEqual(done[0]["failed"], 1)

    def test_the_view_is_stale_after_the_ack(self):
        v1, v2 = versions("python")
        kept = []
        with host_with([("counter.scxml", v1)], migrated=lambda host, entry: kept.append(entry)) as host:
            host.spawn(host.chart("counter"))
            host.run_until_idle()
            add(host, v2)
            host.migrate(None)
            host.run_until_idle()
            with self.assertRaises(tinyactors.TinyactorsError) as raised:
                kept[0].session
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")

    def test_a_handler_that_raises_is_raised_by_the_driver(self):
        v1, v2 = versions("python")

        def done(host, result):
            raise ValueError("job handler")

        with host_with([("counter.scxml", v1)]) as host:
            host.spawn(host.chart("counter"))
            host.run_until_idle()
            add(host, v2)
            host.migrate(done)
            with self.assertRaises(ValueError):
                host.run_until_idle()

    def test_a_job_unfinished_at_close_never_calls_its_handler(self):
        v1, v2 = versions("python")
        done = []
        with host_with([("counter.scxml", v1)]) as host:
            asleep = host.spawn(host.chart("counter"))
            host.run_until_idle()
            host.hibernate(asleep)
            add(host, v2)
            host.migrate(lambda host, result: done.append(result))
            host.run_until_idle()
        self.assertEqual(done, [])

    def test_migrate_refuses_bad_arguments(self):
        with host_with([("counter.scxml", versions("python")[0])]) as host:
            with self.assertRaises(TypeError):
                host.migrate(None, chart=3)
            with self.assertRaises(ValueError):
                host.migrate(None, chart="")
            with self.assertRaises(ValueError):
                host.migrate(None, session=0)
            with self.assertRaises(OverflowError):
                host.migrate(None, timeout=-1)
            with self.assertRaises(TypeError):
                host.migrate(3)


if __name__ == "__main__":
    unittest.main()
