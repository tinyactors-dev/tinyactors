"""Snapshots under load: a whole host to bytes and back, its actors going
on where they stopped, most of them asleep in SQLite.

    python tools/snapshot.py [--actors N] [--resident R] [--events E] [--datamodel python|ecmascript]

The application's side of snapshots (README, "Snapshots"), as a program
would write it, to take a host out of memory for a while (a restart, a
move to another process of the same build):

1. populate: N actors of a counter chart, at most R resident, the rest
   hibernated into SQLite (tools/stress.py's store); E events to actors
   drawn at random; every resident actor gets a delayed send that falls
   due after the restore;
2. snapshot: Host.snapshot() writes the whole host (every segment's
   region, the datamodels' sessions and payloads, the images) to bytes,
   written to a file; the host is closed and every Python object of it
   collected;
3. restore: Host.restore(bytes, config, ...) makes a new host from the
   file, with the same handlers and the same store. A snapshot of the
   restored host, before it runs, must be the same bytes: nothing is
   lost or changed on the way;
4. go on: E more events to random actors (the hibernated ones wake from
   the store, as before), the clock past the delayed sends' due time;
5. verify: a sample of actors (all of them up to 25,000) reports its
   count, which must be every event it was sent, before and after the
   restore, and whether its delayed send came (the resident ones);
   after every eviction at most R actors are resident.

Exits with status 1 when a check fails. Prints the times, the sizes,
the process's peak memory and the host's region per actor.
"""

import argparse
import array
import collections
import gc
import os
import random
import resource
import sys
import tempfile
import time

import tinyactors
from stress import BATCH, Store

CHARTS = {
    "python": """<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='python' name='counter' initial='s'>
  <datamodel><data id='n' expr='0'/><data id='last' expr='None'/><data id='late' expr='0'/></datamodel>
  <state id='s'>
    <transition event='inc'><assign location='n' expr='n + 1'/><assign location='last' expr='_event.data'/></transition>
    <transition event='wait'><send event='late' delay='10s'><content expr='{"due": True}'/></send></transition>
    <transition event='late'><assign location='late' expr='late + 1'/></transition>
    <transition event='report'><log label='count' expr="f'{_sessionid} {n} {late}'"/></transition>
  </state>
</scxml>""",
    "ecmascript": """<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript' name='counter' initial='s'>
  <datamodel><data id='n' expr='0'/><data id='last' expr='null'/><data id='late' expr='0'/></datamodel>
  <state id='s'>
    <transition event='inc'><assign location='n' expr='n + 1'/><assign location='last' expr='_event.data'/></transition>
    <transition event='wait'><send event='late' delay='10s'><content expr='({due: true})'/></send></transition>
    <transition event='late'><assign location='late' expr='late + 1'/></transition>
    <transition event='report'><log label='count' expr="_sessionid + ' ' + n + ' ' + late"/></transition>
  </state>
</scxml>""",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actors", type=int, default=2_000_000)
    parser.add_argument("--resident", type=int, default=1000)
    parser.add_argument("--events", type=int, default=200_000)
    parser.add_argument("--sample", type=int, default=25_000)
    parser.add_argument("--datamodel", choices=sorted(CHARTS), default="python")
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    random.seed(options.seed)
    failures = []

    directory = tempfile.TemporaryDirectory()
    store = Store(os.path.join(directory.name, "actors.db"))
    resident = collections.OrderedDict()  # sessions in memory, least recently used first
    counts, wakes = {}, [0]

    def wake(host, session, job):
        host.wake(session, store.get(session))
        resident[session] = None
        wakes[0] += 1

    def log(host, entry):
        session, count, late = entry.text.split()
        counts[int(session)] = (int(count), int(late))

    def hibernate(host, session):
        try:
            store.put(session, host.hibernate(session))
        except tinyactors.TinyactorsError as error:
            if error.status != "TA_ERROR_STATE":
                raise
            return False  # not at rest now: it stays resident
        resident.pop(session, None)
        return True

    def evict(host):
        while len(resident) > options.resident:
            cold = host.sweep(1 << 20, len(resident) - options.resident)
            if not sum(hibernate(host, session) for session in cold):
                break
        for session in list(resident)[:max(0, len(resident) - options.resident)]:
            hibernate(host, session)

    peak = [0]

    def check_resident(host):
        got = []
        host.host_stats(lambda host, result: got.append(result["host"]))
        host.run_until_idle()
        stats = got[0]
        live = stats["actors"] - stats["hibernated"]
        peak[0] = max(peak[0], live)
        if live > options.resident:
            failures.append("%d actors resident, more than %d" % (live, options.resident))
        return stats

    resident_room = options.resident + 2 * BATCH
    config = {"systemID": "snapshot", "system": {"ticksPerSecond": 1000,
                                                 "maxActors": options.actors + resident_room, "maxEvents": 4096 + resident_room,
                                                 "maxTimers": resident_room,
                                                 "instanceBytes": 2048 * resident_room,
                                                 "stringBytes": 16384 + 64 * resident_room}}
    clock = tinyactors.SimulatedClock()
    host = tinyactors.Host(config, wake=wake, log=log, clock=clock)
    with tinyactors.compile_charts([("counter.scxml", CHARTS[options.datamodel])]) as compiled:
        host.add_image(compiled.image)
    chart = host.chart("counter")
    sessions = array.array("Q")
    sent = array.array("I", bytes(4 * options.actors))

    def traffic(host, count):
        for first in range(0, count, BATCH):
            items = []
            for _ in range(min(BATCH, count - first)):
                i = random.randrange(options.actors)
                sent[i] += 1
                items.append((sessions[i], "inc", {"k": i}))
                if sessions[i] in resident:
                    resident.move_to_end(sessions[i])
            if host.send_many(items) != len(items):
                failures.append("send_many refused events")
            host.run_until_idle()
            evict(host)
            check_resident(host)

    # 1. populate, R resident at most; events; a delayed send for each resident actor
    started = time.perf_counter()
    for i in range(options.actors):
        session = host.spawn(chart)
        sessions.append(session)
        resident[session] = None
        if len(resident) >= options.resident + BATCH:
            host.run_until_idle()
            evict(host)
    host.run_until_idle()
    evict(host)
    traffic(host, options.events)
    waiting = list(resident)  # their delayed sends are due 10 s on, after the restore
    if host.send_many([(session, "wait") for session in waiting]) != len(waiting):
        failures.append("send_many refused events")
    host.run_until_idle()
    store.flush()
    stats = check_resident(host)
    populated = time.perf_counter() - started
    region = stats["regionBytes"]
    print("populate: %d actors and %d events in %.1f s, %d resident with a delayed send, %d hibernated" % (
        options.actors, options.events, populated, len(waiting), stats["hibernated"]), flush=True)

    # 2. snapshot, to a file; the host goes
    started = time.perf_counter()
    blob = host.snapshot()
    taken = time.perf_counter() - started
    path = os.path.join(directory.name, "host.snapshot")
    with open(path, "wb") as file:
        file.write(blob)
    host.close()
    del host, blob
    gc.collect()
    print("snapshot: %.1f MB in %.2f s (%.0f B an actor), the host closed" % (
        os.path.getsize(path) / 1e6, taken, os.path.getsize(path) / options.actors), flush=True)

    # 3. restore from the file; a snapshot of it before it runs is the same bytes
    with open(path, "rb") as file:
        blob = file.read()
    started = time.perf_counter()
    host = tinyactors.Host.restore(blob, config, wake=wake, log=log, clock=clock)
    restored = time.perf_counter() - started
    again = host.snapshot()
    if again != blob:
        failures.append("the restored host's snapshot differs from the one it was restored from (%d and %d bytes)" % (
            len(again), len(blob)))
    del again, blob
    print("restore: in %.2f s" % restored, flush=True)

    # 4. go on: events, the hibernated actors wake from the store; the delayed sends fall due
    started = time.perf_counter()
    traffic(host, options.events)
    clock.increment(10_000)
    evict(host)
    went_on = time.perf_counter() - started
    print("after: %d events and the delayed sends in %.1f s, %d woken from the store in all" % (
        options.events, went_on, wakes[0]), flush=True)

    # 5. verify a sample: every event before and after, and the delayed sends
    late = set(waiting)
    sample = range(options.actors) if options.actors <= options.sample else random.sample(range(options.actors), options.sample)
    sample = sorted(set(sample) | {(s & 0xFFFFFFFF) - 1 for s in waiting[:100]})  # segment 0 numbers them 1, 2, ...
    for chunk in range(0, len(sample), BATCH):
        host.send_many([(sessions[i], "report") for i in sample[chunk:chunk + BATCH]])
        host.run_until_idle()
        evict(host)
        check_resident(host)
    wrong = [i for i in sample if counts.get(sessions[i]) != (sent[i], 1 if sessions[i] in late else 0)]
    if wrong:
        failures.append("%d of %d actors wrong, e.g. actor %d: %r, sent %d" % (
            len(wrong), len(sample), wrong[0], counts.get(sessions[wrong[0]]), sent[wrong[0]]))
    host.close()
    store.flush()
    peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    print("verify: %d actors' counts and delayed sends, %d wrong" % (len(sample), len(wrong)))
    print("memory: peak resident %.0f MB (%.0f B an actor); region %.1f MB (%.0f B an actor); at most %d actors "
          "resident after an eviction" % (peak_rss / 1e6, peak_rss / options.actors, region / 1e6,
                                          region / options.actors, peak[0]))
    for failure in failures:
        print("FAIL " + failure)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
